// Tests for src/fork/mac_drive_policy.h: what LuzLunar may ask for when it shows its Mac as a
// network drive on this PC, where the drive points and which letter it takes, what Windows' errors
// come to, when the drive goes, and the lines between Apollo and its helper. A standalone program,
// so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/mac_drive_policy_test.cpp

#include "fork/mac_drive_policy.h"

#include <cstdio>
#include <string>

using namespace mac_drive_policy;
using namespace std::literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static bool rejected(const std::string &body) {
  std::string error;
  auto request = parse_map_request(body, error);
  return !request && !error.empty();
}

static void test_map_request() {
  std::string error;
  auto request = parse_map_request(R"({"action":"map","shares":["Macintosh HD","gonzalo"],"user":"gonzalo","password":"p4ss word","name":"Gonzalo's MacBook Pro"})", error);
  CHECK(request);
  if (request) {
    CHECK(request->shares.size() == 2);
    CHECK(request->shares[0] == "Macintosh HD");
    CHECK(request->shares[1] == "gonzalo");
    CHECK(request->user == "gonzalo");
    CHECK(request->password == "p4ss word");
    CHECK(request->label == "Gonzalo's MacBook Pro");
  }

  // Without a name, or with an empty one, Explorer calls it "Mac"
  request = parse_map_request(R"({"action":"map","shares":["Macintosh HD"],"user":"g","password":"x"})", error);
  CHECK(request && request->label == "Mac");
  request = parse_map_request(R"({"action":"map","shares":["Macintosh HD"],"user":"g","password":"x","name":""})", error);
  CHECK(request && request->label == "Mac");

  // Names and passwords outside ASCII are kept as they are
  request = parse_map_request(R"({"action":"map","shares":["Disco Duro"],"user":"g","password":"contraseña€😀","name":"MacBook de Begoña"})", error);
  CHECK(request && request->password == "contraseña€😀" && request->label == "MacBook de Begoña");

  CHECK(rejected("not json"));
  CHECK(rejected("[]"));
  CHECK(rejected(R"({"shares":["a"],"user":"g","password":"x"})"));
  CHECK(rejected(R"({"action":"unmap","shares":["a"],"user":"g","password":"x"})"));
  CHECK(rejected(R"({"action":"map","user":"g","password":"x"})"));
  CHECK(rejected(R"({"action":"map","shares":[],"user":"g","password":"x"})"));
  CHECK(rejected(R"({"action":"map","shares":["a","b","c","d","e"],"user":"g","password":"x"})"));
  CHECK(rejected(R"({"action":"map","shares":[1],"user":"g","password":"x"})"));
  CHECK(rejected(R"({"action":"map","shares":"a","user":"g","password":"x"})"));

  // No password, or an empty one: Windows would offer the PC user's own credentials instead
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"g"})"));
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"g","password":""})"));
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"g","password":"a\nb"})"));
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"g","password":")" + std::string(256, 'x') + R"("})"));
  CHECK(!rejected(R"({"action":"map","shares":["a"],"user":"g","password":")" + std::string(255, 'x') + R"("})"));

  CHECK(rejected(R"({"action":"map","shares":["a"],"password":"x"})"));
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"","password":"x"})"));
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"g\\h","password":"x"})"));
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"g","password":"x","name":"a\u0001b"})"));
  CHECK(rejected(R"({"action":"map","shares":["a"],"user":"g","password":"x","name":7})"));
}

static void test_names() {
  CHECK(valid_user("gonzalo"));
  CHECK(valid_user("first.last_2-b"));
  CHECK(!valid_user(""));
  CHECK(!valid_user("-x"));
  CHECK(!valid_user(".x"));
  CHECK(!valid_user("a b"));
  CHECK(!valid_user("DOMAIN\\user"));
  CHECK(!valid_user("user@host"));
  CHECK(!valid_user("añ"));
  CHECK(valid_user(std::string(255, 'a')));
  CHECK(!valid_user(std::string(256, 'a')));

  CHECK(valid_share("Macintosh HD"));
  CHECK(valid_share("gonzalo's Public Folder"));
  CHECK(valid_share("Disco de Begoña"));
  CHECK(!valid_share(""));
  CHECK(!valid_share("."));
  CHECK(!valid_share(".."));
  CHECK(!valid_share("C$"));
  CHECK(!valid_share("IPC$"));
  CHECK(!valid_share(" lead"));
  CHECK(!valid_share("trail "));
  for (auto c : "\\/:*?\"<>|"sv) {
    CHECK(!valid_share("a"s + c + "b"));
  }
  CHECK(!valid_share("a\tb"));
  CHECK(valid_share(std::string(80, 'a')));
  CHECK(!valid_share(std::string(81, 'a')));

  CHECK(valid_host("192.168.1.23"));
  CHECK(valid_host("fe80--1s4.ipv6-literal.net"));
  CHECK(!valid_host(""));
  CHECK(!valid_host("a\\b"));
  CHECK(!valid_host("a b"));
}

static void test_unc_host() {
  CHECK(unc_host("192.168.1.23") == "192.168.1.23");
  CHECK(unc_host("10.0.0.1") == "10.0.0.1");
  CHECK(unc_host("0.0.0.0") == "0.0.0.0");
  CHECK(unc_host("192.168.01.23").empty());
  CHECK(unc_host("256.1.1.1").empty());
  CHECK(unc_host("1.2.3").empty());
  CHECK(unc_host("1.2.3.4.5").empty());
  CHECK(unc_host("1.2.3.4 ").empty());
  CHECK(unc_host("1..2.3").empty());
  CHECK(unc_host("").empty());
  CHECK(unc_host("mac.local").empty());
  CHECK(unc_host("\\\\evil\\share").empty());

  CHECK(unc_host("fd00::1") == "fd00--1.ipv6-literal.net");
  CHECK(unc_host("2001:db8::") == "2001-db8--0.ipv6-literal.net");
  CHECK(unc_host("::1") == "0--1.ipv6-literal.net");
  CHECK(unc_host("1:2:3:4:5:6:7:8") == "1-2-3-4-5-6-7-8.ipv6-literal.net");
  CHECK(unc_host("1:2:3:4:5:6:7").empty());
  CHECK(unc_host("1:2:3:4:5:6:7:8:9").empty());
  CHECK(unc_host("1::2::3").empty());
  CHECK(unc_host("12345::1").empty());
  CHECK(unc_host("::ffff:1.2.3.4").empty());

  // Link-local needs its zone, which the PC's socket reports as a number
  CHECK(unc_host("fe80::1c2b:3d4e:5f60:7a8b%12") == "fe80--1c2b-3d4e-5f60-7a8bs12.ipv6-literal.net");
  CHECK(unc_host("FEBF::1%3") == "FEBF--1s3.ipv6-literal.net");
  CHECK(unc_host("fe80::1").empty());
  CHECK(unc_host("fe80::1%").empty());
  CHECK(unc_host("fe80::1%en0").empty());
  CHECK(unc_host("fec0::1") == "fec0--1.ipv6-literal.net");

  CHECK(unc_path("192.168.1.23", "Macintosh HD") == "\\\\192.168.1.23\\Macintosh HD");
  CHECK(label_key("192.168.1.23", "Macintosh HD") == "##192.168.1.23#Macintosh HD");
}

static std::uint32_t bit(char letter) {
  return 1u << (letter - 'A');
}

static void test_letters() {
  CHECK(choose_letter(0) == 'M');
  CHECK(choose_letter(bit('A') | bit('C') | bit('D')) == 'M');
  CHECK(choose_letter(bit('M')) == 'Z');
  CHECK(choose_letter(bit('M') | bit('Z') | bit('Y')) == 'X');
  CHECK(choose_letter(0, 'Q') == 'Q');
  CHECK(choose_letter(0, 'C') == 'Z');
  CHECK(choose_letter(0, 'a') == 'Z');

  std::uint32_t all = 0;
  for (char letter = 'D'; letter <= 'Z'; letter++) {
    all |= bit(letter);
  }
  CHECK(choose_letter(all) == 0);
  CHECK(choose_letter(all & ~bit('E')) == 'E');
}

static void test_errors() {
  CHECK(classify(0) == failure_e::none);
  CHECK(classify(1326) == failure_e::logon);
  CHECK(classify(86) == failure_e::logon);
  CHECK(classify(1909) == failure_e::logon);
  CHECK(classify(1937) == failure_e::ntlm_blocked);
  CHECK(classify(5) == failure_e::denied);
  CHECK(classify(67) == failure_e::no_share);
  CHECK(classify(53) == failure_e::unreachable);
  CHECK(classify(1231) == failure_e::unreachable);
  CHECK(classify(1219) == failure_e::conflict);
  CHECK(classify(85) == failure_e::letter_taken);
  CHECK(classify(1202) == failure_e::letter_taken);
  CHECK(classify(1248) == failure_e::no_letter);
  CHECK(classify(2401) == failure_e::in_use);
  CHECK(classify(2404) == failure_e::in_use);
  CHECK(classify(2250) == failure_e::gone);
  CHECK(classify(12345) == failure_e::other);

  CHECK(key(classify(1326)) == "logon");
  CHECK(key(classify(67)) == "no_share");
  CHECK(key(classify(12345)) == "other");
  CHECK(describe(1326) == "the Mac refused the account or its password (error 1326)");
  CHECK(describe(1219).find("another account") != std::string::npos);
}

static void test_removal() {
  using std::chrono::seconds;
  using std::chrono::minutes;
  CHECK(removal(std::nullopt, std::nullopt) == removal_e::keep);
  CHECK(removal(std::nullopt, seconds(100)) == removal_e::keep);
  CHECK(removal(seconds(0), std::nullopt) == removal_e::keep);
  CHECK(removal(seconds(59), std::nullopt) == removal_e::keep);
  CHECK(removal(seconds(60), std::nullopt) == removal_e::unmap);

  // Files were open: tried again every 30 s, and forced 3 minutes after the stream ended
  CHECK(removal(seconds(75), seconds(15)) == removal_e::keep);
  CHECK(removal(seconds(90), seconds(30)) == removal_e::unmap);
  CHECK(removal(minutes(3) - seconds(1), seconds(29)) == removal_e::keep);
  CHECK(removal(minutes(3), seconds(1)) == removal_e::unmap_force);
  CHECK(removal(minutes(10), std::nullopt) == removal_e::unmap_force);
}

static void test_commands() {
  map_request_t request;
  request.shares = {"Macintosh HD", "gonzalo"};
  request.user = "gonzalo";
  request.password = R"(sp ace "quote" \back% ñ€😀)";
  request.label = "Gonzalo's MacBook Pro";

  auto line = map_command('M', "192.168.1.23", request);
  CHECK(line.find('\n') == std::string::npos);
  CHECK(line.find('\r') == std::string::npos);
  auto command = parse_command(line);
  CHECK(command.kind == command_t::kind_e::map);
  CHECK(command.letter == 'M');
  CHECK(command.host == "192.168.1.23");
  CHECK(command.request.shares == request.shares);
  CHECK(command.request.user == "gonzalo");
  CHECK(command.request.password == request.password);
  CHECK(command.request.label == "Gonzalo's MacBook Pro");

  CHECK(parse_command(unmap_command(false)).kind == command_t::kind_e::unmap);
  CHECK(!parse_command(unmap_command(false)).force);
  CHECK(parse_command(unmap_command(true)).kind == command_t::kind_e::unmap);
  CHECK(parse_command(unmap_command(true)).force);

  CHECK(parse_command("").kind == command_t::kind_e::bad);
  CHECK(parse_command("map").kind == command_t::kind_e::bad);
  CHECK(parse_command("{}").kind == command_t::kind_e::bad);
  CHECK(parse_command(R"({"cmd":"format"})").kind == command_t::kind_e::bad);

  // A map line the helper won't take: a letter it may not use, a host that isn't one, a bad share
  auto changed = [&](const char *key, nlohmann::json value) {
    auto json = nlohmann::json::parse(line);
    json[key] = std::move(value);
    return parse_command(json.dump()).kind;
  };
  CHECK(changed("letter", "C") == command_t::kind_e::bad);
  CHECK(changed("letter", "MM") == command_t::kind_e::bad);
  CHECK(changed("host", "evil\\share") == command_t::kind_e::bad);
  CHECK(changed("host", "") == command_t::kind_e::bad);
  CHECK(changed("shares", nlohmann::json::array({"ok", "C$"})) == command_t::kind_e::bad);
  CHECK(changed("shares", nlohmann::json::array()) == command_t::kind_e::bad);
  CHECK(changed("password", "") == command_t::kind_e::bad);
  CHECK(changed("user", "a b") == command_t::kind_e::bad);
  CHECK(changed("label", "a\nb") == command_t::kind_e::bad);
  CHECK(changed("label", "fine") == command_t::kind_e::map);
}

static void test_results() {
  auto mapped = parse_result(mapped_result('M', "Macintosh HD"));
  CHECK(mapped.kind == result_t::kind_e::mapped);
  CHECK(mapped.letter == 'M');
  CHECK(mapped.share == "Macintosh HD");

  auto failed = parse_result(failed_result(1326, "gonzalo"));
  CHECK(failed.kind == result_t::kind_e::failed);
  CHECK(failed.code == 1326);
  CHECK(failed.share == "gonzalo");

  auto in_use = parse_result(in_use_result(2401));
  CHECK(in_use.kind == result_t::kind_e::in_use);
  CHECK(in_use.code == 2401);

  CHECK(parse_result(unmapped_result()).kind == result_t::kind_e::unmapped);

  // Log lines and anything malformed aren't answers
  CHECK(parse_result("info: mapped M:").kind == result_t::kind_e::none);
  CHECK(parse_result("").kind == result_t::kind_e::none);
  CHECK(parse_result("{not json").kind == result_t::kind_e::none);
  CHECK(parse_result(R"({"result":"mapped","letter":"C","share":"x"})").kind == result_t::kind_e::none);
  CHECK(parse_result(R"({"result":"mapped","letter":"M"})").kind == result_t::kind_e::none);
  CHECK(parse_result(R"({"result":"exploded"})").kind == result_t::kind_e::none);
}

static void test_status() {
  auto json = nlohmann::json::parse(status_json({}));
  CHECK(json["state"] == "none");

  status_t mapping;
  mapping.state = state_e::mapping;
  CHECK(nlohmann::json::parse(status_json(mapping))["state"] == "mapping");

  status_t mapped;
  mapped.state = state_e::mapped;
  mapped.letter = 'M';
  mapped.share = "Macintosh HD";
  mapped.added = true;
  json = nlohmann::json::parse(status_json(mapped));
  CHECK(json["state"] == "mapped");
  CHECK(json["letter"] == "M");
  CHECK(json["share"] == "Macintosh HD");
  CHECK(json["added"] == true);

  status_t failed;
  failed.state = state_e::failed;
  failed.error = "logon";
  failed.code = 1326;
  json = nlohmann::json::parse(status_json(failed));
  CHECK(json["state"] == "failed");
  CHECK(json["error"] == "logon");
  CHECK(json["code"] == 1326);

  status_t nobody;
  nobody.state = state_e::failed;
  nobody.error = "no_user";
  json = nlohmann::json::parse(status_json(nobody));
  CHECK(json["error"] == "no_user");
  CHECK(!json.contains("code"));
}

int main() {
  test_map_request();
  test_names();
  test_unc_host();
  test_letters();
  test_errors();
  test_removal();
  test_commands();
  test_results();
  test_status();

  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("mac_drive_policy: all checks passed\n");
  return 0;
}
