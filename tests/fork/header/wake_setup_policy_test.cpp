// Tests for src/fork/wake_setup_policy.h: what turning this PC on from off needs from Windows and
// the network adapter's driver, what a fix changes and an undo puts back, how the PC was last
// powered on, and the JSON LuzLunar gets. A standalone program, so CI builds it without the rest
// of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/wake_setup_policy_test.cpp

#include "fork/wake_setup_policy.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace wake_setup_policy;
using namespace std::literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static std::vector<option_t> on_off(std::string off = "Disabled", std::string on = "Enabled") {
  return {{"0", off}, {"1", on}};
}

static param_t param(std::string keyword, std::string desc, std::string value, bool set, std::vector<option_t> options) {
  return param_t {std::move(keyword), std::move(desc), std::move(value), set, std::move(options)};
}

static const check_t *find(const std::vector<check_t> &checks, std::string_view id, std::string_view keyword = {}) {
  for (const auto &check : checks) {
    if (check.id == id && (keyword.empty() || check.keyword == keyword)) {
      return &check;
    }
  }
  return nullptr;
}

/// A Realtek 2.5GbE adapter with Shutdown Wake-On-Lan off, and Fast Startup on
static facts_t realtek() {
  facts_t facts;
  facts.adapter.link = link_e::ethernet;
  facts.adapter.name = "Ethernet";
  facts.adapter.description = "Realtek PCIe 2.5GbE Family Controller";
  facts.adapter.mac = "aa:bb:cc:dd:ee:ff";
  facts.adapter.guid = "{1111-2222}";
  facts.adapter.driver_key = "0001";
  facts.adapter.params = {
    param("*WakeOnMagicPacket", "Wake on Magic Packet", "1", true, on_off()),
    param("*WakeOnPattern", "Wake on pattern match", "1", true, on_off()),
    param("S5WakeOnLan", "Shutdown Wake-On-Lan", "0", true, on_off()),
    param("WolShutdownLinkSpeed", "WOL & Shutdown Link Speed", "0", true, {{"0", "10 Mbps First"}, {"1", "100 Mbps First"}, {"2", "Not Speed Down"}}),
    param("*EEE", "Energy-Efficient Ethernet", "1", true, on_off()),
    param("PowerSavingMode", "Power Saving Mode", "1", true, on_off()),
  };
  facts.fast_startup = {true, 1u, std::nullopt};
  facts.last_power_on = power_on_e::power_switch;
  facts.uptime_s = 3600;
  return facts;
}

/// An Intel I225-V with Wake on Magic Packet at its default (off), Enable PME and the power-off
/// setting off, and Fast Startup already off
static facts_t intel() {
  facts_t facts;
  facts.adapter.link = link_e::ethernet;
  facts.adapter.name = "Ethernet 2";
  facts.adapter.description = "Intel(R) Ethernet Controller I225-V";
  facts.adapter.guid = "{3333-4444}";
  facts.adapter.driver_key = "0007";
  facts.adapter.params = {
    param("*WakeOnMagicPacket", "Wake on Magic Packet", "0", false, on_off()),
    param("EnablePME", "Enable PME", "0", true, on_off()),
    param("WakeOnPowerOff", "Wake on Magic Packet from power off state", "0", true, on_off()),
    param("WakeOnLink", "Wake on Link Settings", "0", true, {{"0", "Disabled"}, {"1", "OS Controlled"}, {"2", "Forced"}}),
    param("*EEE", "Energy-Efficient Ethernet", "1", true, {{"0", "Off"}, {"1", "On"}}),
  };
  facts.fast_startup = {true, 0u, std::nullopt};
  return facts;
}

static void test_matching() {
  auto pme = param("EnablePME", "Enable PME", "0", true, on_off());
  CHECK(shutdown_wake(pme));
  CHECK(shutdown_wake(param("S5WAKEONLAN", "Anything", "0", true, on_off())));  // keywords ignore case
  CHECK(shutdown_wake(param("X", "Shutdown Wake-On-Lan", "0", true, on_off())));
  CHECK(shutdown_wake(param("X", "Wake on Magic Packet from power off state", "0", true, on_off())));
  CHECK(shutdown_wake(param("X", "Enable PME", "0", true, on_off())));
  CHECK(shutdown_wake(param("X", "Wake from S5", "0", true, on_off())));
  // About the link, not about staying armed
  CHECK(!shutdown_wake(param("WolShutdownLinkSpeed", "WOL & Shutdown Link Speed", "0", true, on_off())));
  CHECK(!shutdown_wake(param("WakeOnLink", "Wake on Link Settings", "0", true, on_off())));
  // PME only as a word of its own
  CHECK(!shutdown_wake(param("X", "Equipment Development Mode", "0", true, on_off())));
  CHECK(!shutdown_wake(param("*WakeOnMagicPacket", "Wake on Magic Packet", "0", true, on_off())));
  CHECK(!shutdown_wake(param("*EEE", "Energy-Efficient Ethernet", "1", true, on_off())));

  // The option that turns it on: named Enabled or On, in English or Spanish
  CHECK(shutdown_wake_target(pme) == "1");
  CHECK(shutdown_wake_target(param("S5WakeOnLan", "Activación tras apagado", "0", true, on_off("Deshabilitado", "Habilitado"))) == "1");
  CHECK(shutdown_wake_target(param("X", "Wake from S5", "2", true, {{"2", "Off"}, {"5", "On"}})) == "5");
  // A known keyword with 0 and 1 in another language: 1
  CHECK(shutdown_wake_target(param("S5WakeOnLan", "Herunterfahren-WOL", "0", true, on_off("Aus", "Ein"))) == "1");
  // A setting matched by its name alone needs an option named on; two of them is ambiguous
  CHECK(!shutdown_wake_target(param("X", "Wake from S5", "0", true, {{"0", "No"}, {"1", "Yes"}})));
  CHECK(!shutdown_wake_target(param("X", "Wake from S5", "0", true, {{"0", "Disabled"}, {"1", "Enabled"}, {"2", "Enabled"}})));

  CHECK(is_virtual("Hyper-V Virtual Ethernet Adapter"));
  CHECK(is_virtual("TAP-Windows Adapter V9"));
  CHECK(is_virtual("Tailscale Tunnel"));
  CHECK(is_virtual("WireGuard Tunnel"));
  CHECK(!is_virtual("Realtek PCIe 2.5GbE Family Controller"));
  CHECK(!is_virtual("Intel(R) Ethernet Controller I225-V"));

  CHECK(is_intel(intel().adapter));
  CHECK(!is_intel(realtek().adapter));
}

static void test_realtek() {
  auto facts = realtek();
  auto list = checks(facts);
  CHECK(find(list, "link") && find(list, "link")->state == state_e::ok);
  CHECK(find(list, "magicPacket") && find(list, "magicPacket")->state == state_e::ok);
  auto s5 = find(list, "shutdownWake");
  CHECK(s5 && s5->state == state_e::fix && s5->keyword == "S5WakeOnLan" && s5->setting == "Shutdown Wake-On-Lan");
  CHECK(s5 && s5->current == "Disabled" && s5->target == "Enabled" && s5->target_value == "1");
  CHECK(!find(list, "shutdownWake", "WolShutdownLinkSpeed"));
  CHECK(!find(list, "eee"));  // only on Intel
  CHECK(find(list, "fastStartup") && find(list, "fastStartup")->state == state_e::fix);
  CHECK(find(list, "macOverride") && find(list, "macOverride")->state == state_e::ok);
  CHECK(find(list, "bios") && find(list, "bios")->state == state_e::manual);
  CHECK(list.front().id == "link" && list.back().id == "bios");
  CHECK(from_off(list) == "fixable");

  auto changes = plan(facts);
  CHECK(changes.size() == 2);
  CHECK(changes[0].kind == change_t::kind_e::param && changes[0].keyword == "S5WakeOnLan" && changes[0].before == "0" &&
        changes[0].after == "1" && changes[0].guid == "{1111-2222}" && changes[0].driver_key == "0001");
  CHECK(changes[1].kind == change_t::kind_e::hiberboot && changes[1].keyword == "HiberbootEnabled" && changes[1].before == "1" &&
        changes[1].after == "0");
}

static void test_intel() {
  auto facts = intel();
  auto list = checks(facts);
  auto magic = find(list, "magicPacket");
  CHECK(magic && magic->state == state_e::fix && magic->target_value == "1");
  CHECK(find(list, "shutdownWake", "EnablePME") && find(list, "shutdownWake", "EnablePME")->state == state_e::fix);
  CHECK(find(list, "shutdownWake", "WakeOnPowerOff") && find(list, "shutdownWake", "WakeOnPowerOff")->state == state_e::fix);
  CHECK(!find(list, "shutdownWake", "WakeOnLink"));
  auto eee = find(list, "eee");
  CHECK(eee && eee->state == state_e::fix && eee->current == "On" && eee->target == "Off" && eee->target_value == "0");
  CHECK(find(list, "fastStartup")->state == state_e::ok);
  CHECK(from_off(list) == "fixable");

  auto changes = plan(facts);
  CHECK(changes.size() == 4);
  // Wake on Magic Packet was at the driver's default: an undo deletes the value again
  CHECK(changes[0].keyword == "*WakeOnMagicPacket" && !changes[0].before && changes[0].after == "1");
  CHECK(changes[1].keyword == "EnablePME" && changes[2].keyword == "WakeOnPowerOff" && changes[3].keyword == "*EEE");
  for (const auto &change : changes) {
    CHECK(change.kind == change_t::kind_e::param);
  }

  // Everything set: ready, and nothing to plan
  for (auto &p : facts.adapter.params) {
    if (p.keyword == "*EEE") {
      p.value = "0";
    } else if (p.keyword != "WakeOnLink") {
      p.value = "1";
    }
  }
  list = checks(facts);
  CHECK(from_off(list) == "ready");
  CHECK(plan(facts).empty());
}

static void test_unknown_and_blocked() {
  // A driver without a wake-from-off setting
  auto facts = realtek();
  facts.adapter.params = {param("*WakeOnMagicPacket", "Wake on Magic Packet", "1", true, on_off())};
  facts.fast_startup.value = 0;
  auto list = checks(facts);
  auto s5 = find(list, "shutdownWake");
  CHECK(s5 && s5->state == state_e::unknown && s5->reason == "missing");
  CHECK(from_off(list) == "unknown");

  // No standard magic packet keyword
  facts.adapter.params = {param("S5WakeOnLan", "Shutdown Wake-On-Lan", "1", true, on_off())};
  list = checks(facts);
  CHECK(find(list, "magicPacket")->state == state_e::unknown && find(list, "magicPacket")->reason == "missing");
  CHECK(from_off(list) == "unknown");

  // A setting whose on option isn't clear is left alone
  facts.adapter.params = {
    param("*WakeOnMagicPacket", "Wake on Magic Packet", "1", true, on_off()),
    param("X", "Wake from S5", "0", true, {{"0", "No"}, {"1", "Yes"}}),
  };
  list = checks(facts);
  CHECK(find(list, "shutdownWake")->state == state_e::unknown && find(list, "shutdownWake")->reason == "ambiguous");
  CHECK(plan(facts).empty());

  // Wi-Fi: blocked, and the adapter's settings aren't checked
  facts = realtek();
  facts.adapter.link = link_e::wifi;
  list = checks(facts);
  CHECK(find(list, "link")->state == state_e::blocked && find(list, "link")->reason == "wifi");
  CHECK(!find(list, "magicPacket") && !find(list, "shutdownWake") && !find(list, "macOverride"));
  CHECK(from_off(list) == "blocked");
  auto changes = plan(facts);
  CHECK(changes.size() == 1 && changes[0].kind == change_t::kind_e::hiberboot);

  // A virtual adapter: unknown, nothing about it changed
  facts.adapter.link = link_e::virtual_adapter;
  facts.fast_startup.value = 0;
  list = checks(facts);
  CHECK(find(list, "link")->state == state_e::unknown && find(list, "link")->reason == "virtual");
  CHECK(from_off(list) == "unknown");
  CHECK(plan(facts).empty());

  facts.adapter.link = link_e::none;
  CHECK(from_off(checks(facts)) == "blocked");

  // A changed MAC is worth knowing, not fixed
  facts = realtek();
  facts.adapter.mac_override = true;
  CHECK(find(checks(facts), "macOverride")->state == state_e::warn);
}

static void test_fast_startup() {
  auto facts = intel();
  auto state = [&](fast_startup_t startup) {
    facts.fast_startup = startup;
    return find(checks(facts), "fastStartup")->state;
  };
  CHECK(state({true, 1u, std::nullopt}) == state_e::fix);
  CHECK(state({true, std::nullopt, std::nullopt}) == state_e::fix);  // missing: on, as Windows defaults
  CHECK(state({true, 0u, std::nullopt}) == state_e::ok);
  CHECK(state({false, 1u, std::nullopt}) == state_e::ok);  // no hibernation file, no Fast Startup
  // A policy decides: a warning, since LuzLunar's Shut Down still does a full shutdown
  CHECK(state({true, 0u, 1u}) == state_e::warn);
  CHECK(state({true, 1u, 0u}) == state_e::ok);

  // Missing: an undo deletes it again
  facts = realtek();
  facts.fast_startup = {true, std::nullopt, std::nullopt};
  auto changes = plan(facts);
  CHECK(changes.size() == 2 && changes[1].kind == change_t::kind_e::hiberboot && !changes[1].before && changes[1].after == "0");

  // Under a policy, nothing is planned for it
  facts.fast_startup = {true, 1u, 1u};
  changes = plan(facts);
  CHECK(changes.size() == 1 && changes[0].kind == change_t::kind_e::param);
}

static void test_journal() {
  auto facts = realtek();
  auto journal = merge({}, plan(facts));
  CHECK(journal.changes.size() == 2);

  // After the fix, someone sets Shutdown Wake-On-Lan off again; a second fix keeps the first "before"
  facts.adapter.params[2].value = "0";
  facts.fast_startup.value = 0;
  auto again = merge(journal, plan(facts));
  CHECK(again.changes.size() == 2);
  CHECK(again.changes[0].before == "0" && again.changes[0].after == "1");
  CHECK(again.changes[1].before == "1");

  // Round trip, and what isn't a journal
  CHECK(from_json(to_json(again)) == again);
  journal_t with_none;
  with_none.changes.push_back(change_t {change_t::kind_e::param, "{g}", "0001", "*WakeOnMagicPacket", std::nullopt, "1"});
  CHECK(from_json(to_json(with_none)) == with_none);
  CHECK(!from_json(""));
  CHECK(!from_json("[]"));
  CHECK(!from_json(R"({"version":2,"changes":[]})"));
  CHECK(!from_json(R"({"version":1})"));
  CHECK(!from_json(R"({"version":1,"changes":[{"kind":"other","guid":"","driverKey":"","keyword":"k","after":"1","before":null}]})"));
  CHECK(!from_json(R"({"version":1,"changes":[{"kind":"param","guid":"","driverKey":"","keyword":"k","after":"1"}]})"));
  CHECK(!from_json(R"({"version":1,"changes":[{"kind":"param","guid":"","driverKey":"","keyword":"","after":"1","before":null}]})"));
  CHECK(!from_json(R"({"version":1,"changes":[{"kind":"param","guid":"","driverKey":"","keyword":"k","after":1,"before":null}]})"));
  CHECK(from_json(R"({"version":1,"changes":[]})") == journal_t {});
}

static void test_undo() {
  auto facts = realtek();
  auto journal = merge({}, plan(facts));

  // After the fix: both are written back
  auto fixed = facts;
  fixed.adapter.params[2].value = "1";
  fixed.fast_startup.value = 0;
  auto undo = undo_plan(journal, fixed);
  CHECK(undo.steps.size() == 2 && undo.kept.empty() && undo.skipped.empty());
  CHECK(undo.steps[0].change.keyword == "S5WakeOnLan" && undo.steps[0].write == "0");
  CHECK(undo.steps[1].change.kind == change_t::kind_e::hiberboot && undo.steps[1].write == "1");

  // A setting someone changed since is theirs
  auto changed = fixed;
  changed.fast_startup.value = 1;
  undo = undo_plan(journal, changed);
  CHECK(undo.steps.size() == 1 && undo.skipped.size() == 1 && undo.skipped[0].kind == change_t::kind_e::hiberboot);

  // Another adapter now: its changes stay in the journal
  auto other = fixed;
  other.adapter.guid = "{9999}";
  undo = undo_plan(journal, other);
  CHECK(undo.steps.size() == 1 && undo.kept.size() == 1 && undo.kept[0].keyword == "S5WakeOnLan");

  // A value that wasn't set is deleted again
  journal_t unset;
  unset.changes.push_back(change_t {change_t::kind_e::param, "{1111-2222}", "0001", "*WakeOnMagicPacket", std::nullopt, "1"});
  undo = undo_plan(unset, fixed);
  CHECK(undo.steps.size() == 1 && !undo.steps[0].write);

  // A journal can't make an undo write what a fix wouldn't
  journal_t tampered;
  tampered.changes = {
    change_t {change_t::kind_e::param, "{1111-2222}", "0001", "NetworkAddress", "001122334455", "aabbccddeeff"},
    change_t {change_t::kind_e::param, "{1111-2222}", "0001", "PowerSavingMode", "0", "1"},
    change_t {change_t::kind_e::param, "{1111-2222}", "0001", "S5WakeOnLan", "7", "1"},  // not an option
    change_t {change_t::kind_e::param, "{1111-2222}", "0001", "*EEE", "0", "1"},  // EEE is Intel's only
    change_t {change_t::kind_e::hiberboot, "", "", "HiberbootEnabled", "7", "0"},
    change_t {change_t::kind_e::hiberboot, "", "", "Other", "1", "0"},
  };
  undo = undo_plan(tampered, fixed);
  CHECK(undo.steps.empty() && undo.skipped.size() == tampered.changes.size());
}

static std::vector<uint8_t> smbios(std::vector<uint8_t> table, uint32_t declared = 0) {
  uint32_t length = declared ? declared : (uint32_t) table.size();
  std::vector<uint8_t> raw {0, 3, 4, 0, (uint8_t) length, (uint8_t) (length >> 8), (uint8_t) (length >> 16), (uint8_t) (length >> 24)};
  raw.insert(raw.end(), table.begin(), table.end());
  return raw;
}

static std::vector<uint8_t> structure(uint8_t type, uint8_t length, uint8_t wake, std::vector<std::string> strings) {
  std::vector<uint8_t> out(length, 0);
  out[0] = type;
  out[1] = length;
  out[2] = 0x01;
  if (length > 0x18) {
    out[0x18] = wake;
  }
  for (const auto &s : strings) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
  }
  if (strings.empty()) {
    out.push_back(0);
  }
  out.push_back(0);
  return out;
}

static void test_smbios() {
  auto bios = structure(0, 0x18, 0, {"American Megatrends", "1.2"});
  auto system = structure(1, 0x1B, 5, {"Micro-Star", "MS-7D25", "1.0"});
  auto end = structure(127, 4, 0, {});

  std::vector<uint8_t> table = bios;
  table.insert(table.end(), system.begin(), system.end());
  table.insert(table.end(), end.begin(), end.end());
  auto raw = smbios(table);
  CHECK(parse_wake_up_type(raw.data(), raw.size()) == power_on_e::lan);

  for (auto [byte, expected] : std::vector<std::pair<uint8_t, power_on_e>> {
         {6, power_on_e::power_switch},
         {7, power_on_e::pme},
         {8, power_on_e::ac_restored},
         {2, power_on_e::unknown},
         {0, power_on_e::unknown},
         {42, power_on_e::unknown},
       }) {
    auto t = bios;
    auto s = structure(1, 0x1B, byte, {"A"});
    t.insert(t.end(), s.begin(), s.end());
    auto r = smbios(t);
    CHECK(parse_wake_up_type(r.data(), r.size()) == expected);
  }

  // Type 1 without strings, first in the table
  auto bare = smbios(structure(1, 0x19, 5, {}));
  CHECK(parse_wake_up_type(bare.data(), bare.size()) == power_on_e::lan);

  // SMBIOS 2.0's short System Information has no Wake-up Type
  auto old = bios;
  auto short_system = structure(1, 0x08, 0, {"A"});
  old.insert(old.end(), short_system.begin(), short_system.end());
  auto r = smbios(old);
  CHECK(parse_wake_up_type(r.data(), r.size()) == power_on_e::unknown);

  // No System Information, the end of the table first, nothing, or less than was declared
  auto none = bios;
  none.insert(none.end(), end.begin(), end.end());
  r = smbios(none);
  CHECK(parse_wake_up_type(r.data(), r.size()) == power_on_e::unknown);
  auto ended = end;
  ended.insert(ended.end(), system.begin(), system.end());
  r = smbios(ended);
  CHECK(parse_wake_up_type(r.data(), r.size()) == power_on_e::unknown);
  CHECK(parse_wake_up_type(nullptr, 0) == power_on_e::unknown);
  CHECK(parse_wake_up_type(raw.data(), 7) == power_on_e::unknown);
  auto cut = smbios(std::vector<uint8_t>(system.begin(), system.begin() + 0x10), 4096);
  CHECK(parse_wake_up_type(cut.data(), cut.size()) == power_on_e::unknown);
  // A broken structure length stops the walk
  auto broken = smbios({1, 2, 0, 0, 0, 0});
  CHECK(parse_wake_up_type(broken.data(), broken.size()) == power_on_e::unknown);

  CHECK(key(power_on_e::lan) == "lan");
  CHECK(key(power_on_e::power_switch) == "powerSwitch");
  CHECK(key(power_on_e::pme) == "pme");
}

static void test_status_json() {
  auto facts = realtek();
  auto list = checks(facts);
  auto json = nlohmann::json::parse(status_json(facts, list, false, std::nullopt, "tok"));
  CHECK(json["fromOff"] == "fixable");
  CHECK(json["adapter"]["name"] == "Ethernet" && json["adapter"]["link"] == "ethernet" && json["adapter"]["mac"] == "aa:bb:cc:dd:ee:ff");
  CHECK(json["checks"].size() == list.size());
  CHECK(json["checks"][2]["id"] == "shutdownWake" && json["checks"][2]["state"] == "fix" && json["checks"][2]["setting"] == "Shutdown Wake-On-Lan");
  CHECK(json["lastPowerOn"] == "powerSwitch");
  CHECK(json["uptimeSeconds"] == 3600);
  CHECK(json["canFix"] == true && json["canUndo"] == false && json["reason"] == "none");
  CHECK(json["token"] == "tok");
  CHECK(!json.contains("lastResult"));

  // While streaming: nothing can run, no token
  facts.streaming = true;
  json = nlohmann::json::parse(status_json(facts, list, true, std::nullopt, "tok"));
  CHECK(json["canFix"] == false && json["canUndo"] == false && json["reason"] == "streaming" && !json.contains("token"));
  facts.streaming = false;
  facts.installing = true;
  CHECK(nlohmann::json::parse(status_json(facts, list, true, std::nullopt, "tok"))["reason"] == "updating");
  facts.installing = false;
  facts.applying = true;
  json = nlohmann::json::parse(status_json(facts, list, true, std::nullopt, "tok"));
  CHECK(json["reason"] == "applying" && json["applying"] == true);

  // Nothing to fix, but an undo: a token for it
  facts = intel();
  for (auto &p : facts.adapter.params) {
    p.value = p.keyword == "*EEE" ? "0" : "1";
  }
  list = checks(facts);
  result_t last {"fix", true, false, 4, {}, ""};
  json = nlohmann::json::parse(status_json(facts, list, true, last, "tok"));
  CHECK(json["fromOff"] == "ready" && json["canFix"] == false && json["canUndo"] == true && json["token"] == "tok");
  CHECK(json["lastResult"]["action"] == "fix" && json["lastResult"]["ok"] == true && json["lastResult"]["changed"] == 4);
  json = nlohmann::json::parse(status_json(facts, list, false, std::nullopt, "tok"));
  CHECK(!json.contains("token"));

  // A driver's name that isn't valid UTF-8 doesn't break the answer
  facts.adapter.description = "Bad \xff name";
  json = nlohmann::json::parse(status_json(facts, checks(facts), false, std::nullopt, ""), nullptr, false);
  CHECK(!json.is_discarded());

  json = nlohmann::json::parse(accepted_json(action_e::undo));
  CHECK(json["applying"] == true && json["action"] == "undo");
}

static void test_parse_request() {
  std::string error;
  auto request = parse_request(R"({"token":"abc","action":"fix"})", error);
  CHECK(request && request->token == "abc" && request->action == action_e::fix);
  request = parse_request(R"({"token":"abc","action":"undo"})", error);
  CHECK(request && request->action == action_e::undo);

  auto rejected = [](std::string_view body) {
    std::string problem;
    return !parse_request(body, problem) && !problem.empty();
  };
  CHECK(rejected(""));
  CHECK(rejected("[]"));
  CHECK(rejected("{}"));
  CHECK(rejected(R"({"token":"abc"})"));
  CHECK(rejected(R"({"token":"abc","action":"write"})"));
  CHECK(rejected(R"({"token":"abc","action":1})"));
  CHECK(rejected(R"({"token":"","action":"fix"})"));
  CHECK(rejected(R"({"token":")" + std::string(max_token + 1, 'a') + R"(","action":"fix"})"));
}

int main() {
  test_matching();
  test_realtek();
  test_intel();
  test_unknown_and_blocked();
  test_fast_startup();
  test_journal();
  test_undo();
  test_smbios();
  test_status_json();
  test_parse_request();

  if (s_Failures == 0) {
    std::printf("wake_setup_policy: all tests passed\n");
    return 0;
  }
  std::printf("wake_setup_policy: %d failure(s)\n", s_Failures);
  return 1;
}
