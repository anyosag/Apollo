/**
 * @file src/fork/wake_setup_policy.h
 * @brief Turning this PC on from off with LuzLunar's Wake PC, apart from any I/O: what Windows and
 * the network adapter's driver have to allow, what to change, the journal that undoes it, how the
 * PC was last powered on, and the JSON LuzLunar gets.
 *
 * Waking from sleep and turning on from off use the same magic packet, but not the same settings.
 * Windows disarms the adapter at a Fast Startup (hybrid) shutdown, which a Start menu shutdown is
 * by default; the adapter's driver has a setting of its own for staying armed once the PC is off
 * (Realtek's "Shutdown Wake-On-Lan", Intel's "Enable PME" and "Wake on Magic Packet from power off
 * state"); and the BIOS has to keep the adapter powered, which only the BIOS can check.
 *
 * LuzLunar asks GET /actions/wake, which says what's set and hands out a token when there is
 * something to fix or undo; POST {"token", "action": "fix"|"undo"} then changes the settings, after
 * saving what they were in a journal. A client never chooses what is written: the keys are the ones
 * below, and the values are the driver's own options.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace wake_setup_policy {
  /// After answering POST, how long before the change runs, so the answer gets out first:
  /// restarting the network adapter drops the connection
  constexpr auto settle = std::chrono::milliseconds(1500);

  /// The standardized keywords (Microsoft's "Standardized INF Keywords for Power Management")
  constexpr std::string_view magic_packet_keyword = "*WakeOnMagicPacket";
  constexpr std::string_view eee_keyword = "*EEE";

  /// The Fast Startup value, under Session Manager\Power, and under the policy key that overrides it
  constexpr std::string_view hiberboot_keyword = "HiberbootEnabled";

  /// Vendor keywords for staying armed once the PC is off: Realtek's "Shutdown Wake-On-Lan" and
  /// Intel's "Enable PME". From community write-ups, not vendor documentation, so the setting's
  /// name (ParamDesc) is matched as well (shutdown_wake()).
  constexpr std::string_view shutdown_wake_keywords[] = {"S5WakeOnLan", "EnablePME"};

  /// How LuzLunar reaches this PC
  enum class link_e {
    none,  ///< No adapter has the address
    ethernet,
    wifi,  ///< A PC can't be turned on over Wi-Fi
    virtual_adapter,  ///< Hyper-V's switch, a VPN: the settings belong to another adapter
    other,
  };

  /// One option of a driver setting's list
  struct option_t {
    std::string value;
    std::string desc;  ///< As Device Manager shows it
  };

  /// One of the driver's settings (its Advanced tab), from Ndi\params\<keyword>
  struct param_t {
    std::string keyword;  ///< The subkey's name, and the value's name in the driver's key
    std::string desc;  ///< ParamDesc, as Device Manager shows it
    std::string value;  ///< The driver key's value, or the default when it has none
    bool set = false;  ///< The driver's key has the value (rather than the default)
    std::vector<option_t> options;  ///< An enum's options; empty for other kinds
  };

  /// The network adapter LuzLunar reaches this PC through
  struct adapter_t {
    link_e link = link_e::none;
    std::string name;  ///< Its name in Windows, like "Ethernet"
    std::string description;  ///< Like "Realtek PCIe 2.5GbE Family Controller"
    std::string mac;  ///< As /serverinfo reports it
    std::string guid;  ///< Its NetCfgInstanceId
    std::string driver_key;  ///< Its subkey in the network class key, like "0001"
    bool mac_override = false;  ///< The driver has a NetworkAddress: Windows uses another MAC
    std::vector<param_t> params;
  };

  /// Fast Startup
  struct fast_startup_t {
    bool hibernation = false;  ///< There's a hibernation file, which Fast Startup needs
    std::optional<uint32_t> value;  ///< HiberbootEnabled
    std::optional<uint32_t> policy;  ///< The policy's HiberbootEnabled, which wins
  };

  /// What powered the PC on, from SMBIOS (System Information, Wake-up Type)
  enum class power_on_e {
    unknown,
    other,
    apm_timer,
    modem_ring,
    lan,  ///< LAN Remote: Wake-on-LAN
    power_switch,
    pme,  ///< PCI PME#: a PCIe network adapter's wake-up, on many boards
    ac_restored,
  };

  /// What the PC is like and doing now
  struct facts_t {
    adapter_t adapter;
    fast_startup_t fast_startup;
    power_on_e last_power_on = power_on_e::unknown;
    uint64_t uptime_s = 0;
    bool streaming = false;  ///< A stream runs, to any client
    bool installing = false;  ///< Apollo is installing an update and will restart
    bool applying = false;  ///< A fix or undo is being carried out
  };

  /// A check's outcome
  enum class state_e {
    ok,
    fix,  ///< Wrong, and a fix sets it
    blocked,  ///< Wrong, and nothing on this PC can change it
    unknown,  ///< Can't tell, or can't tell what to set
    warn,  ///< Worth knowing; not something a fix changes
    manual,  ///< Only someone at the PC can check it (the BIOS)
  };

  struct check_t {
    std::string id;  ///< link, magicPacket, shutdownWake (one per setting), eee, fastStartup, macOverride, bios
    state_e state = state_e::ok;
    std::string reason;  ///< Why, when it isn't plain: wifi, virtual, none, other, missing, ambiguous, policy, ...
    std::string keyword;  ///< The driver setting, or HiberbootEnabled
    std::string setting;  ///< Its name as Device Manager shows it
    std::string current;  ///< The current option's name
    std::string target;  ///< The option a fix sets
    std::string current_value;
    std::string target_value;
  };

  /// One change a fix makes, as the journal keeps it
  struct change_t {
    enum class kind_e {
      param,  ///< A driver setting
      hiberboot,  ///< Fast Startup
    };

    kind_e kind = kind_e::param;
    std::string guid;  ///< The adapter, for a driver setting
    std::string driver_key;
    std::string keyword;
    std::optional<std::string> before;  ///< What it was, or none when it wasn't set
    std::string after;

    bool operator==(const change_t &) const = default;
  };

  /// The journal: every change fixes made, with what each was before the first of them
  struct journal_t {
    std::vector<change_t> changes;

    bool operator==(const journal_t &) const = default;
  };

  /// How the last fix or undo went, for LuzLunar
  struct result_t {
    std::string action;  ///< fix or undo
    bool ok = false;
    bool restart_needed = false;  ///< Windows says the adapter needs the PC restarted
    int changed = 0;
    std::vector<std::string> failed;  ///< The keywords that couldn't be written
    std::string error;  ///< Like "access_denied" when Apollo doesn't run as its service
  };

  enum class action_e {
    fix,
    undo,
  };

  struct request_t {
    std::string token;
    action_e action = action_e::fix;
  };

  /// A token is 1 to this many characters
  constexpr std::size_t max_token = 64;

  namespace detail {
    inline std::string lower(std::string_view text) {
      std::string out(text);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      return out;
    }

    inline bool iequals(std::string_view a, std::string_view b) {
      return lower(a) == lower(b);
    }

    inline bool contains(std::string_view haystack, std::string_view needle) {
      return haystack.find(needle) != std::string_view::npos;
    }

    /// The needle as a word of its own: "Enable PME", not "Equipment"
    inline bool contains_word(std::string_view haystack, std::string_view needle) {
      auto alnum = [](char c) {
        return std::isalnum((unsigned char) c) != 0;
      };
      for (auto pos = haystack.find(needle); pos != std::string_view::npos; pos = haystack.find(needle, pos + 1)) {
        auto end = pos + needle.size();
        if ((pos == 0 || !alnum(haystack[pos - 1])) && (end == haystack.size() || !alnum(haystack[end]))) {
          return true;
        }
      }
      return false;
    }

    inline std::string trim_lower(std::string_view text) {
      auto first = text.find_first_not_of(" \t");
      if (first == std::string_view::npos) {
        return {};
      }
      auto last = text.find_last_not_of(" \t");
      return lower(text.substr(first, last - first + 1));
    }

    inline bool is_on_word(std::string_view desc) {
      auto word = trim_lower(desc);
      return word == "enabled" || word == "enable" || word == "on" || word == "habilitado" || word == "activado";
    }

    inline bool is_off_word(std::string_view desc) {
      auto word = trim_lower(desc);
      return word == "disabled" || word == "disable" || word == "off" || word == "deshabilitado" || word == "desactivado";
    }

    inline bool has_option(const std::vector<option_t> &options, std::string_view value) {
      return std::any_of(options.begin(), options.end(), [&](const option_t &option) {
        return option.value == value;
      });
    }

    /// The options are exactly 0 and 1
    inline bool zero_one(const param_t &param) {
      return param.options.size() == 2 && has_option(param.options, "0") && has_option(param.options, "1");
    }

    inline std::string option_desc(const param_t &param, std::string_view value) {
      for (const auto &option : param.options) {
        if (option.value == value) {
          return option.desc;
        }
      }
      return std::string {value};
    }

    /// The one option whose name means on (or off); none when no option or several do
    inline std::optional<std::string> option_named(const param_t &param, bool on) {
      std::optional<std::string> found;
      for (const auto &option : param.options) {
        if (on ? is_on_word(option.desc) : is_off_word(option.desc)) {
          if (found) {
            return std::nullopt;
          }
          found = option.value;
        }
      }
      return found;
    }

    /// The value to set a standardized on/off keyword to, if its options allow it
    inline std::optional<std::string> standard_target(const param_t &param, std::string_view value) {
      if (param.options.empty() || has_option(param.options, value)) {
        return std::string {value};
      }
      return std::nullopt;
    }

    inline check_t setting_check(std::string id, const param_t &param, const std::optional<std::string> &target) {
      check_t check;
      check.id = std::move(id);
      check.keyword = param.keyword;
      check.setting = param.desc.empty() ? param.keyword : param.desc;
      check.current_value = param.value;
      check.current = option_desc(param, param.value);
      if (!target) {
        check.state = state_e::unknown;
        check.reason = "ambiguous";
        return check;
      }
      check.target_value = *target;
      check.target = option_desc(param, *target);
      check.state = param.value == *target ? state_e::ok : state_e::fix;
      return check;
    }
  }  // namespace detail

  inline bool is_intel(const adapter_t &adapter) {
    return detail::lower(adapter.description).starts_with("intel");
  }

  /// A description that names a virtual adapter: Hyper-V's switch, VPNs and tunnels
  inline bool is_virtual(std::string_view description) {
    auto text = detail::lower(description);
    for (auto word : {"virtual", "vpn", "tap-windows", "wintun", "wireguard", "tailscale", "zerotier", "vmware", "virtualbox", "loopback", "tunnel"}) {
      if (detail::contains(text, word)) {
        return true;
      }
    }
    return false;
  }

  inline const param_t *find_param(const adapter_t &adapter, std::string_view keyword) {
    for (const auto &param : adapter.params) {
      if (detail::iequals(param.keyword, keyword)) {
        return &param;
      }
    }
    return nullptr;
  }

  /// Whether a driver setting is a known shutdown-wake keyword
  inline bool shutdown_wake_keyword(const param_t &param) {
    return std::any_of(std::begin(shutdown_wake_keywords), std::end(shutdown_wake_keywords), [&](std::string_view keyword) {
      return detail::iequals(param.keyword, keyword);
    });
  }

  /**
   * @brief Whether a driver setting keeps the adapter armed once the PC is off: a known keyword,
   * or a name like "Shutdown Wake-On-Lan", "Wake on Magic Packet from power off state" or "Enable
   * PME". Not "WOL & Shutdown Link Speed" or "Wake on Link Settings", which are about the link.
   */
  inline bool shutdown_wake(const param_t &param) {
    if (shutdown_wake_keyword(param)) {
      return true;
    }
    if (detail::iequals(param.keyword, magic_packet_keyword)) {
      return false;
    }
    auto desc = detail::lower(param.desc);
    if (detail::contains(desc, "speed") || detail::contains(desc, "link")) {
      return false;
    }
    for (auto phrase : {"shutdown wake", "wake from shutdown", "wake on lan after shutdown", "power off state", "wake from s5", "s5 wake"}) {
      if (detail::contains(desc, phrase)) {
        return true;
      }
    }
    return detail::contains_word(desc, "pme");
  }

  /// The option that turns a shutdown-wake setting on; none when that isn't clear
  inline std::optional<std::string> shutdown_wake_target(const param_t &param) {
    if (auto named = detail::option_named(param, true)) {
      return named;
    }
    // A known keyword whose options are 0 and 1, named in another language say: 1 is on, as for
    // Windows' own keywords. A setting matched by its name alone needs an option named on.
    if (shutdown_wake_keyword(param) && detail::zero_one(param)) {
      return std::string {"1"};
    }
    return std::nullopt;
  }

  /// Whether a fix may change this driver setting, which is also what an undo may write back
  inline bool managed(const adapter_t &adapter, const param_t &param) {
    return detail::iequals(param.keyword, magic_packet_keyword) || (is_intel(adapter) && detail::iequals(param.keyword, eee_keyword)) ||
           shutdown_wake(param);
  }

  /// A check without a driver setting's details
  inline check_t make_check(std::string id, state_e state = state_e::ok, std::string reason = {}, std::string keyword = {}) {
    check_t check;
    check.id = std::move(id);
    check.state = state;
    check.reason = std::move(reason);
    check.keyword = std::move(keyword);
    return check;
  }

  /**
   * @brief The checks, in the order LuzLunar shows them. The adapter's own settings are checked
   * only when LuzLunar reaches the PC over Ethernet: over Wi-Fi it can't be turned on, and a
   * virtual adapter's settings aren't the network card's.
   */
  inline std::vector<check_t> checks(const facts_t &facts) {
    std::vector<check_t> out;
    const auto &adapter = facts.adapter;

    auto link = make_check("link");
    switch (adapter.link) {
      case link_e::ethernet:
        break;
      case link_e::wifi:
        link.state = state_e::blocked;
        link.reason = "wifi";
        break;
      case link_e::virtual_adapter:
        link.state = state_e::unknown;
        link.reason = "virtual";
        break;
      case link_e::none:
        link.state = state_e::blocked;
        link.reason = "none";
        break;
      case link_e::other:
        link.state = state_e::unknown;
        link.reason = "other";
        break;
    }
    out.push_back(link);

    if (adapter.link == link_e::ethernet) {
      if (auto param = find_param(adapter, magic_packet_keyword)) {
        out.push_back(detail::setting_check("magicPacket", *param, detail::standard_target(*param, "1")));
      } else {
        out.push_back(make_check("magicPacket", state_e::unknown, "missing", std::string {magic_packet_keyword}));
      }

      bool found = false;
      for (const auto &param : adapter.params) {
        if (shutdown_wake(param)) {
          found = true;
          out.push_back(detail::setting_check("shutdownWake", param, shutdown_wake_target(param)));
        }
      }
      if (!found) {
        out.push_back(make_check("shutdownWake", state_e::unknown, "missing"));
      }

      // Intel's steps for waking from off turn Energy Efficient Ethernet off
      if (is_intel(adapter)) {
        if (auto param = find_param(adapter, eee_keyword)) {
          out.push_back(detail::setting_check("eee", *param, detail::standard_target(*param, "0")));
        }
      }
    }

    auto fast = make_check("fastStartup", state_e::ok, {}, std::string {hiberboot_keyword});
    fast.setting = "Fast Startup";
    const auto &startup = facts.fast_startup;
    auto on = [](std::optional<uint32_t> v) {
      return v && *v != 0;
    };
    if (startup.policy) {
      // A policy decides; a full shutdown from LuzLunar still leaves the adapter armed
      fast.state = on(startup.policy) ? state_e::warn : state_e::ok;
      fast.reason = "policy";
      fast.current = on(startup.policy) ? "On" : "Off";
    } else if (!startup.hibernation) {
      fast.reason = "no_hibernation";
      fast.current = "Off";
    } else if (startup.value && *startup.value == 0) {
      fast.current = "Off";
    } else {
      fast.state = state_e::fix;
      fast.current = "On";
      fast.target = "Off";
      fast.current_value = startup.value ? std::to_string(*startup.value) : std::string {};
      fast.target_value = "0";
    }
    out.push_back(fast);

    if (adapter.link == link_e::ethernet) {
      out.push_back(make_check("macOverride", adapter.mac_override ? state_e::warn : state_e::ok));
    }

    out.push_back(make_check("bios", state_e::manual));
    return out;
  }

  /// The whole answer: ready (as far as Windows goes), fixable, blocked or unknown
  inline std::string_view from_off(const std::vector<check_t> &checks) {
    auto any = [&](state_e state) {
      return std::any_of(checks.begin(), checks.end(), [&](const check_t &check) {
        return check.state == state;
      });
    };
    if (any(state_e::blocked)) {
      return "blocked";
    }
    if (any(state_e::fix)) {
      return "fixable";
    }
    for (const auto &check : checks) {
      if (check.state == state_e::unknown && (check.id == "link" || check.id == "magicPacket" || check.id == "shutdownWake")) {
        return "unknown";
      }
    }
    return "ready";
  }

  /// The changes a fix makes
  inline std::vector<change_t> plan(const facts_t &facts, const std::vector<check_t> &checks) {
    std::vector<change_t> changes;
    for (const auto &check : checks) {
      if (check.state != state_e::fix) {
        continue;
      }
      if (check.id == "fastStartup") {
        change_t change;
        change.kind = change_t::kind_e::hiberboot;
        change.keyword = std::string {hiberboot_keyword};
        change.after = check.target_value;
        if (facts.fast_startup.value) {
          change.before = std::to_string(*facts.fast_startup.value);
        }
        changes.push_back(change);
        continue;
      }
      auto param = find_param(facts.adapter, check.keyword);
      if (!param || !managed(facts.adapter, *param)) {
        continue;
      }
      change_t change;
      change.guid = facts.adapter.guid;
      change.driver_key = facts.adapter.driver_key;
      change.keyword = param->keyword;
      change.after = check.target_value;
      if (param->set) {
        change.before = param->value;
      }
      changes.push_back(change);
    }
    return changes;
  }

  inline std::vector<change_t> plan(const facts_t &facts) {
    return plan(facts, checks(facts));
  }

  namespace detail {
    inline bool same_target(const change_t &a, const change_t &b) {
      return a.kind == b.kind && iequals(a.guid, b.guid) && iequals(a.keyword, b.keyword);
    }
  }  // namespace detail

  /// The journal after a fix: an earlier fix's "before" stays, so an undo goes back to the start
  inline journal_t merge(const journal_t &journal, const std::vector<change_t> &changes) {
    journal_t out = journal;
    for (const auto &change : changes) {
      auto it = std::find_if(out.changes.begin(), out.changes.end(), [&](const change_t &entry) {
        return detail::same_target(entry, change);
      });
      if (it == out.changes.end()) {
        out.changes.push_back(change);
      } else {
        it->after = change.after;
        it->driver_key = change.driver_key;
      }
    }
    return out;
  }

  /// One write an undo makes
  struct undo_t {
    change_t change;  ///< The journal's entry
    std::optional<std::string> write;  ///< The value before, or none to delete the value
  };

  struct undo_plan_t {
    std::vector<undo_t> steps;  ///< To write back
    std::vector<change_t> kept;  ///< For an adapter that isn't this one; they stay in the journal
    std::vector<change_t> skipped;  ///< Changed since, or not something a fix changes; dropped
  };

  /**
   * @brief What an undo writes back. Only what a fix could have changed, only values the setting
   * allows, and only where the setting still has what the fix set: a setting someone changed since
   * is theirs.
   */
  inline undo_plan_t undo_plan(const journal_t &journal, const facts_t &facts) {
    undo_plan_t out;
    for (const auto &change : journal.changes) {
      if (change.kind == change_t::kind_e::hiberboot) {
        auto valid = [](const std::optional<std::string> &v) {
          return !v || *v == "0" || *v == "1";
        };
        auto current = facts.fast_startup.value ? std::optional<std::string> {std::to_string(*facts.fast_startup.value)} : std::nullopt;
        if (!detail::iequals(change.keyword, hiberboot_keyword) || !valid(change.before) || (change.after != "0" && change.after != "1") ||
            current != std::optional<std::string> {change.after}) {
          out.skipped.push_back(change);
          continue;
        }
        out.steps.push_back(undo_t {change, change.before});
        continue;
      }

      if (change.guid.empty() || !detail::iequals(change.guid, facts.adapter.guid)) {
        out.kept.push_back(change);
        continue;
      }
      auto param = find_param(facts.adapter, change.keyword);
      if (!param || !managed(facts.adapter, *param) || param->value != change.after ||
          (change.before && !param->options.empty() && !detail::has_option(param->options, *change.before))) {
        out.skipped.push_back(change);
        continue;
      }
      out.steps.push_back(undo_t {change, change.before});
    }
    return out;
  }

  inline std::string_view kind_key(change_t::kind_e kind) {
    return kind == change_t::kind_e::hiberboot ? "hiberboot" : "param";
  }

  inline std::string to_json(const journal_t &journal) {
    nlohmann::json changes = nlohmann::json::array();
    for (const auto &change : journal.changes) {
      nlohmann::json entry {
        {"kind", std::string {kind_key(change.kind)}},
        {"guid", change.guid},
        {"driverKey", change.driver_key},
        {"keyword", change.keyword},
        {"after", change.after},
      };
      entry["before"] = change.before ? nlohmann::json(*change.before) : nlohmann::json(nullptr);
      changes.push_back(entry);
    }
    return nlohmann::json {{"version", 1}, {"changes", changes}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }

  /// A journal's text, or none if it isn't one this version wrote
  inline std::optional<journal_t> from_json(std::string_view text) {
    auto json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("version", 0) != 1) {
      return std::nullopt;
    }
    auto changes = json.find("changes");
    if (changes == json.end() || !changes->is_array()) {
      return std::nullopt;
    }
    journal_t journal;
    for (const auto &entry : *changes) {
      if (!entry.is_object()) {
        return std::nullopt;
      }
      auto string = [&](const char *key) -> std::optional<std::string> {
        auto it = entry.find(key);
        if (it == entry.end() || !it->is_string()) {
          return std::nullopt;
        }
        return it->get<std::string>();
      };
      auto kind = string("kind");
      auto guid = string("guid");
      auto driver_key = string("driverKey");
      auto keyword = string("keyword");
      auto after = string("after");
      auto before = entry.find("before");
      if (!kind || (*kind != "param" && *kind != "hiberboot") || !guid || !driver_key || !keyword || keyword->empty() || !after ||
          before == entry.end() || !(before->is_null() || before->is_string())) {
        return std::nullopt;
      }
      change_t change;
      change.kind = *kind == "hiberboot" ? change_t::kind_e::hiberboot : change_t::kind_e::param;
      change.guid = *guid;
      change.driver_key = *driver_key;
      change.keyword = *keyword;
      change.after = *after;
      if (before->is_string()) {
        change.before = before->get<std::string>();
      }
      journal.changes.push_back(change);
    }
    return journal;
  }

  inline power_on_e power_on_from_byte(uint8_t value) {
    switch (value) {
      case 1:
        return power_on_e::other;
      case 3:
        return power_on_e::apm_timer;
      case 4:
        return power_on_e::modem_ring;
      case 5:
        return power_on_e::lan;
      case 6:
        return power_on_e::power_switch;
      case 7:
        return power_on_e::pme;
      case 8:
        return power_on_e::ac_restored;
      default:
        return power_on_e::unknown;
    }
  }

  /**
   * @brief How the PC was last powered on, from GetSystemFirmwareTable('RSMB')'s buffer: the
   * 8-byte RawSMBIOSData header (calling method, major and minor version, DMI revision, a 32-bit
   * length), then the SMBIOS structures. System Information (type 1) has the Wake-up Type at
   * offset 18h, in SMBIOS 2.1 and later. Each structure is followed by its strings, which end with
   * two NULs.
   */
  inline power_on_e parse_wake_up_type(const uint8_t *data, std::size_t size) {
    constexpr std::size_t raw_header = 8;
    if (data == nullptr || size < raw_header) {
      return power_on_e::unknown;
    }
    std::size_t length = (std::size_t) data[4] | ((std::size_t) data[5] << 8) | ((std::size_t) data[6] << 16) | ((std::size_t) data[7] << 24);
    length = std::min(length, size - raw_header);
    const uint8_t *table = data + raw_header;

    std::size_t pos = 0;
    while (pos + 4 <= length) {
      uint8_t type = table[pos];
      uint8_t formatted = table[pos + 1];
      if (formatted < 4 || pos + formatted > length) {
        break;
      }
      if (type == 1) {
        return formatted > 0x18 ? power_on_from_byte(table[pos + 0x18]) : power_on_e::unknown;
      }
      if (type == 127) {  // End of table
        break;
      }
      std::size_t next = pos + formatted;
      while (next + 1 < length && !(table[next] == 0 && table[next + 1] == 0)) {
        next++;
      }
      pos = next + 2;
    }
    return power_on_e::unknown;
  }

  inline std::string_view key(power_on_e power_on) {
    switch (power_on) {
      case power_on_e::unknown:
        return "unknown";
      case power_on_e::other:
        return "other";
      case power_on_e::apm_timer:
        return "apmTimer";
      case power_on_e::modem_ring:
        return "modemRing";
      case power_on_e::lan:
        return "lan";
      case power_on_e::power_switch:
        return "powerSwitch";
      case power_on_e::pme:
        return "pme";
      case power_on_e::ac_restored:
        return "acRestored";
    }
    return "unknown";
  }

  /// For the log: "LAN (Wake-on-LAN)", "the power switch", ...
  inline std::string_view describe(power_on_e power_on) {
    switch (power_on) {
      case power_on_e::lan:
        return "LAN (Wake-on-LAN)";
      case power_on_e::pme:
        return "PCI PME# (a PCIe device, like the network adapter)";
      case power_on_e::power_switch:
        return "the power switch";
      case power_on_e::ac_restored:
        return "power coming back";
      case power_on_e::apm_timer:
        return "a timer";
      case power_on_e::modem_ring:
        return "a modem ring";
      case power_on_e::other:
        return "something else";
      case power_on_e::unknown:
        return "an unknown event";
    }
    return "an unknown event";
  }

  inline std::string_view key(state_e state) {
    switch (state) {
      case state_e::ok:
        return "ok";
      case state_e::fix:
        return "fix";
      case state_e::blocked:
        return "blocked";
      case state_e::unknown:
        return "unknown";
      case state_e::warn:
        return "warn";
      case state_e::manual:
        return "manual";
    }
    return "unknown";
  }

  inline std::string_view key(link_e link) {
    switch (link) {
      case link_e::none:
        return "none";
      case link_e::ethernet:
        return "ethernet";
      case link_e::wifi:
        return "wifi";
      case link_e::virtual_adapter:
        return "virtual";
      case link_e::other:
        return "other";
    }
    return "none";
  }

  /// Why a fix or undo can't run now, or "none"
  inline std::string_view blocker(const facts_t &facts) {
    if (facts.streaming) {
      return "streaming";
    }
    if (facts.installing) {
      return "updating";
    }
    if (facts.applying) {
      return "applying";
    }
    return "none";
  }

  inline nlohmann::json result_json(const result_t &result) {
    return nlohmann::json {
      {"action", result.action},
      {"ok", result.ok},
      {"restartNeeded", result.restart_needed},
      {"changed", result.changed},
      {"failed", result.failed},
      {"error", result.error},
    };
  }

  /**
   * @brief GET's answer: {"fromOff", "adapter", "checks", "lastPowerOn", "uptimeSeconds",
   * "undoable", "applying", "canFix", "canUndo", "reason", "lastResult", "token"}. The token is
   * there only when a fix or an undo can run now.
   */
  inline std::string status_json(const facts_t &facts, const std::vector<check_t> &checks, bool undoable, const std::optional<result_t> &last, const std::string &token) {
    auto reason = blocker(facts);
    bool can_fix = reason == "none" && !plan(facts, checks).empty();
    bool can_undo = reason == "none" && undoable;

    nlohmann::json list = nlohmann::json::array();
    for (const auto &check : checks) {
      list.push_back({
        {"id", check.id},
        {"state", std::string {key(check.state)}},
        {"reason", check.reason},
        {"keyword", check.keyword},
        {"setting", check.setting},
        {"current", check.current},
        {"target", check.target},
      });
    }

    nlohmann::json json {
      {"fromOff", std::string {from_off(checks)}},
      {"adapter", {
                    {"name", facts.adapter.name},
                    {"description", facts.adapter.description},
                    {"mac", facts.adapter.mac},
                    {"link", std::string {key(facts.adapter.link)}},
                  }},
      {"checks", list},
      {"lastPowerOn", std::string {key(facts.last_power_on)}},
      {"uptimeSeconds", facts.uptime_s},
      {"undoable", undoable},
      {"applying", facts.applying},
      {"canFix", can_fix},
      {"canUndo", can_undo},
      {"reason", std::string {reason}},
    };
    if (last) {
      json["lastResult"] = result_json(*last);
    }
    if ((can_fix || can_undo) && !token.empty()) {
      json["token"] = token;
    }
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }

  inline std::string accepted_json(action_e action) {
    return nlohmann::json {{"applying", true}, {"action", action == action_e::fix ? "fix" : "undo"}}.dump();
  }

  /**
   * @brief Reads POST /actions/wake's body: {"token": ..., "action": "fix"|"undo"}.
   * @param error Set to what's wrong when it isn't valid.
   */
  inline std::optional<request_t> parse_request(std::string_view body, std::string &error) {
    auto json = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      error = "the body isn't a JSON object";
      return std::nullopt;
    }
    auto token = json.find("token");
    if (token == json.end() || !token->is_string() || token->get_ref<const std::string &>().empty() ||
        token->get_ref<const std::string &>().size() > max_token) {
      error = "invalid token";
      return std::nullopt;
    }
    auto action = json.find("action");
    if (action == json.end() || !action->is_string() || (*action != "fix" && *action != "undo")) {
      error = "invalid action";
      return std::nullopt;
    }
    request_t request;
    request.token = token->get<std::string>();
    request.action = *action == "fix" ? action_e::fix : action_e::undo;
    return request;
  }
}  // namespace wake_setup_policy
