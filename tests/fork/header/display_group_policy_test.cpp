// Tests for src/fork/display_group_policy.h: the group LuzLunar declares for "Desktop (multiple
// displays)", where its displays go, which slot a joining stream takes, and the JSON of
// /actions/displaygroup and /actions/pairdevice. A standalone program, so CI builds it without the
// rest of Apollo:
//   g++ -std=c++23 -iquote src -Ithird-party tests/fork/header/display_group_policy_test.cpp

#include "fork/display_group_policy.h"

#include <cstdio>
#include <random>
#include <string>

using namespace display_group_policy;
using namespace std::literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static std::string base64url(std::string_view in) {
  static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (unsigned char c : in) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(alphabet[(buffer >> bits) & 0x3F]);
    }
  }
  if (bits > 0) {
    out.push_back(alphabet[(buffer << (6 - bits)) & 0x3F]);
  }
  return out;
}

// The contract's example
static const char *k_Example = R"j({"v":1,"id":"0123456789ABCDEF","slots":[
 {"slot":0,"x":0,"y":0},
 {"slot":1,"w":1920,"h":1080,"fps":60,"x":3024,"y":442,"external":true,"hdr":false,
  "screen":{"mm":"476x268","panel":"1920x1080","safe":"1920x1080","hz":"60","gamut":"srgb","hdr":"none","name":"DisplayExtend (iMac)"}}]})j";

static std::optional<declaration_t> parse_json(std::string_view json, std::string &error) {
  return parse(base64url(json), error);
}

static void test_base64url() {
  CHECK(base64url_decode("") == ""s);
  CHECK(base64url_decode("TQ") == "M"s);
  CHECK(base64url_decode("TWE") == "Ma"s);
  CHECK(base64url_decode("TWFu") == "Man"s);
  CHECK(base64url_decode("TWE=") == "Ma"s);
  // Both alphabets: 0xFB 0xFF is "-_8" in base64url, "+/8" in base64
  CHECK(base64url_decode("-_8") == "\xFB\xFF"s);
  CHECK(base64url_decode("+/8") == "\xFB\xFF"s);
  CHECK(!base64url_decode("T"));
  CHECK(!base64url_decode("TW!u"));
  CHECK(base64url_decode(base64url(k_Example)) == std::string {k_Example});
}

static void test_parse_example() {
  std::string error;
  auto d = parse_json(k_Example, error);
  CHECK(d);
  if (!d) {
    std::printf("  error: %s\n", error.c_str());
    return;
  }
  CHECK(d->id == "0123456789abcdef");
  CHECK(d->slots.size() == 2);
  CHECK(d->slots[0].slot == 0);
  CHECK(d->slots[0].width == 0);
  auto *s = d->find(1);
  CHECK(s);
  CHECK(s->width == 1920 && s->height == 1080);
  CHECK(s->fps_mhz == 60000);
  CHECK(s->x == 3024 && s->y == 442);
  CHECK(s->external);
  CHECK(!s->hdr);
  CHECK(s->screen_args.at("name") == "DisplayExtend (iMac)");
  CHECK(s->screen);
  CHECK(s->screen->panel_width == 1920 && s->screen->panel_height == 1080);
  CHECK(s->screen->width_mm == 476 && s->screen->height_mm == 268);
  CHECK(s->screen->max_hz == 60);
  CHECK(!s->screen->p3);
  CHECK(s->screen->hdr == client_display::hdr_e::none);
  CHECK(s->screen->name == "DisplayExtend (iMac)");
  CHECK(!d->find(2));
}

static void test_parse_details() {
  std::string error;

  // Slots come out by number, whatever order they were declared in; a slot without a screen
  // gets none, and missing booleans are false
  auto d = parse_json(R"({"v":1,"id":"ffffffffffffffff","slots":[{"slot":2,"w":2560,"h":1440,"fps":59.94,"x":-2560,"y":0},{"slot":0},{"slot":1,"w":1920,"h":1080,"fps":120000,"x":3024,"y":0,"hdr":true,"screen":{"panel":"3840x2160","hz":120,"gamut":"p3","hdr":"1000,600,5"}}]})", error);
  CHECK(d);
  if (d) {
    CHECK(d->slots.size() == 3);
    CHECK(d->slots[0].slot == 0 && d->slots[1].slot == 1 && d->slots[2].slot == 2);
    CHECK(d->slots[2].fps_mhz == 59940);
    CHECK(!d->slots[2].external && !d->slots[2].hdr);
    CHECK(!d->slots[2].screen);
    CHECK(d->slots[2].x == -2560);
    // fps in millihertz, as Moonlight's mode may have it; hz as a number
    CHECK(d->slots[1].fps_mhz == 120000);
    CHECK(d->slots[1].hdr);
    CHECK(d->slots[1].screen && d->slots[1].screen->max_hz == 120 && d->slots[1].screen->p3);
    CHECK(d->slots[1].screen->hdr == client_display::hdr_e::known && d->slots[1].screen->peak_nits == 1000);
  }

  // Refused: each with what's wrong
  for (auto bad : {
         R"([])",
         R"({"v":2,"id":"0123456789abcdef","slots":[{"slot":0}]})",
         R"({"id":"0123456789abcdef","slots":[{"slot":0}]})",
         R"({"v":1,"id":"0123456789abcde","slots":[{"slot":0}]})",
         R"({"v":1,"id":"0123456789abcdeg","slots":[{"slot":0}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":1,"w":1920,"h":1080}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0,"x":10,"y":0}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":0}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":16,"w":1920,"h":1080}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":-1,"w":1920,"h":1080}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"h":1080}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":100,"h":1080}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":"1920","h":1080}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":1920,"h":1080,"fps":0}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":1920,"h":1080,"fps":"60"}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":1920,"h":1080,"fps":900}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":1920,"h":1080,"external":1}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":1920,"h":1080,"x":32000}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":1920,"h":1080,"screen":"x"}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},{"slot":1,"w":1920,"h":1080,"screen":{"name":7.5}}]})",
         R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0},"x"]})",
       }) {
    error.clear();
    CHECK(!parse_json(bad, error));
    CHECK(!error.empty());
  }

  // More slots than LuzVDA has displays
  std::string many = R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0})";
  for (int k = 1; k <= max_slots; ++k) {
    many += R"(,{"slot":)" + std::to_string(k) + R"(,"w":1920,"h":1080})";
  }
  many += "]}";
  CHECK(!parse_json(many, error));

  // Not base64url, or too long
  CHECK(!parse("!!!!", error));
  CHECK(!parse(std::string(max_declaration + 4, 'A'), error));
  CHECK(!parse("", error));
}

static void test_same_layout() {
  std::string error;
  auto a = parse_json(k_Example, error);
  std::string other_id = k_Example;
  other_id.replace(other_id.find("0123456789ABCDEF"), 16, "aaaaaaaaaaaaaaaa");
  auto b = parse_json(other_id, error);
  CHECK(a && b);
  if (!a || !b) {
    return;
  }
  CHECK(a->id != b->id);
  CHECK(same_layout(*a, *b));

  std::string moved = k_Example;
  moved.replace(moved.find("\"y\":442"), 7, "\"y\":440");
  auto c = parse_json(moved, error);
  CHECK(c && !same_layout(*a, *c));

  std::string renamed = k_Example;
  renamed.replace(renamed.find("DisplayExtend (iMac)"), 20, "Another display     ");
  auto d = parse_json(renamed, error);
  CHECK(d && !same_layout(*a, *d));

  // Slot 0 alone isn't the same as slot 0 and slot 1
  auto e = parse_json(R"({"v":1,"id":"0123456789abcdef","slots":[{"slot":0}]})", error);
  CHECK(e && !same_layout(*a, *e));
}

static void test_layout() {
  rect_t main {0, 0, 3024, 1890};
  rect_t right {3024, 442, 1920, 1080};
  rect_t above {500, -1080, 1920, 1080};
  rect_t left {-2560, 0, 2560, 1440};

  CHECK(touches(main, right));
  CHECK(touches(main, above));
  CHECK(touches(main, left));
  CHECK(!touches(right, above));
  // A corner alone isn't touching
  CHECK(!touches(main, {3024, 1890, 100, 100}));
  // Edges in line but apart
  CHECK(!touches(main, {3024, 1890, 1920, 1080}));
  CHECK(!touches(main, {3025, 0, 1920, 1080}));

  CHECK(!overlaps(main, right));
  CHECK(overlaps(main, {3000, 0, 1920, 1080}));
  CHECK(!overlaps(main, {3024, 0, 1920, 1080}));

  CHECK(valid_layout({main}));
  CHECK(valid_layout({main, right}));
  CHECK(valid_layout({main, right, above, left}));
  // A display reached only through another
  CHECK(valid_layout({main, right, {4944, 442, 1920, 1080}}));
  // Slot 0 isn't at the origin
  CHECK(!valid_layout({{10, 0, 1920, 1080}, right}));
  // Apart from the rest
  CHECK(!valid_layout({main, {3100, 442, 1920, 1080}}));
  // Overlapping
  CHECK(!valid_layout({main, {3000, 442, 1920, 1080}}));
  CHECK(!valid_layout({main, right, {3024, 1000, 1920, 1080}}));
  CHECK(!valid_layout({main, {3024, 0, 0, 1080}}));
  CHECK(!valid_layout({}));

  bool declared = false;
  auto kept = arrange({main, right, above}, declared);
  CHECK(declared);
  CHECK(kept == (std::vector<rect_t> {main, right, above}));

  // Overlapping: a row from the origin, in slot order
  auto row = arrange({main, {3000, 442, 1920, 1080}, {0, 0, 2560, 1440}}, declared);
  CHECK(!declared);
  CHECK(row.size() == 3);
  CHECK(row[0] == (rect_t {0, 0, 3024, 1890}));
  CHECK(row[1] == (rect_t {3024, 0, 1920, 1080}));
  CHECK(row[2] == (rect_t {4944, 0, 2560, 1440}));
  CHECK(valid_layout(row));

  // Any desktop comes out valid
  std::mt19937 random {12345};
  std::uniform_int_distribution<int> size {320, 4000};
  std::uniform_int_distribution<int> place {-6000, 6000};
  std::uniform_int_distribution<int> count {1, max_slots};
  for (int i = 0; i < 2000; ++i) {
    std::vector<rect_t> displays {{0, 0, size(random), size(random)}};
    int n = count(random);
    for (int k = 1; k < n; ++k) {
      displays.push_back({place(random), place(random), size(random), size(random)});
    }
    auto out = arrange(displays, declared);
    CHECK(valid_layout(out));
    CHECK(out.size() == displays.size());
    for (std::size_t k = 0; k < out.size(); ++k) {
      CHECK(out[k].width == displays[k].width && out[k].height == displays[k].height);
    }
  }
}

static void test_slot_guid() {
  std::array<uint8_t, 16> base {};
  for (int i = 0; i < 16; ++i) {
    base[i] = (uint8_t) (0x10 + i);
  }
  CHECK(slot_guid(base, 0) == base);
  auto one = slot_guid(base, 1);
  CHECK(one != base);
  CHECK(std::equal(base.begin(), base.begin() + 15, one.begin()));
  CHECK(one[15] == (base[15] ^ 1));
  // Every slot its own
  for (int a = 0; a < max_slots; ++a) {
    for (int b = a + 1; b < max_slots; ++b) {
      CHECK(slot_guid(base, a) != slot_guid(base, b));
    }
  }
}

static lookup_t args(std::map<std::string, std::string> values) {
  return [values](std::string_view key) -> std::optional<std::string> {
    auto it = values.find(std::string {key});
    if (it == values.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

static void test_requests() {
  auto none = parse_request(args({{"mode", "1920x1080x60"}}));
  CHECK(none.kind == request_t::kind_e::none);
  CHECK(!none.bad());

  auto lead = parse_request(args({{"luzGroup", base64url(k_Example)}, {"luzGroupId", "aaaaaaaaaaaaaaaa"}}));
  CHECK(lead.kind == request_t::kind_e::lead);
  CHECK(lead.declaration && lead.declaration->id == "0123456789abcdef");
  CHECK(!lead.bad());

  auto bad = parse_request(args({{"luzGroup", "nonsense!"}}));
  CHECK(bad.kind == request_t::kind_e::lead);
  CHECK(bad.bad());
  CHECK(!bad.error.empty());

  auto join = parse_request(args({{"luzGroupId", "0123456789ABCDEF"}, {"luzSlot", "2"}}));
  CHECK(join.kind == request_t::kind_e::join);
  CHECK(join.id == "0123456789abcdef");
  CHECK(join.slot == 2);

  auto odd = parse_request(args({{"luzGroupId", "nope"}, {"luzSlot", "x"}}));
  CHECK(odd.kind == request_t::kind_e::join);
  CHECK(odd.id.empty());
  CHECK(odd.slot == -1);
  CHECK(parse_request(args({{"luzGroupId", "0123456789abcdef"}, {"luzSlot", "16"}})).slot == -1);
}

static void test_check_join() {
  group_view_t group {"0123456789abcdef", {{1, false, {}}, {2, true, {}}, {3, true, "mac"}}};

  auto join = [](std::string id, int slot) {
    request_t r;
    r.kind = request_t::kind_e::join;
    r.id = id;
    r.slot = slot;
    return r;
  };

  // LuzLunar's helpers name their slot
  auto ok = check_join(&group, join("0123456789abcdef", 1), "mac");
  CHECK(ok.status == 200 && ok.slot == 1);
  // The same device takes its own slot back
  auto again = check_join(&group, join("0123456789abcdef", 3), "mac");
  CHECK(again.status == 200 && again.slot == 3);
  // Another device's slot
  CHECK(check_join(&group, join("0123456789abcdef", 3), "imac").status == 409);
  // Unknown slots, slot 0 included
  CHECK(check_join(&group, join("0123456789abcdef", 4), "mac").status == 409);
  CHECK(check_join(&group, join("0123456789abcdef", 0), "mac").status == 409);
  CHECK(check_join(&group, join("0123456789abcdef", -1), "mac").status == 409);
  // No such group
  CHECK(check_join(&group, join("aaaaaaaaaaaaaaaa", 1), "mac").status == 410);
  CHECK(check_join(&group, join("", 1), "mac").status == 410);
  CHECK(check_join(nullptr, join("0123456789abcdef", 1), "mac").status == 410);

  // Stock Moonlight: the first free external slot
  request_t stock;
  auto first = check_join(&group, stock, "imac");
  CHECK(first.status == 200 && first.slot == 2);
  // An external slot it holds already, before a free one
  group.slots[1].claimed_by = "imac2";
  auto own = check_join(&group, stock, "mac");
  CHECK(own.status == 200 && own.slot == 3);
  // None free
  auto full = check_join(&group, stock, "imac");
  CHECK(full.status == 409);
  CHECK(!full.why.empty());
  // A non-external slot is never taken that way
  group_view_t internal {"0123456789abcdef", {{1, false, {}}}};
  CHECK(check_join(&internal, stock, "imac").status == 409);
  // A group of slot 0 alone resumes as any app does
  group_view_t alone {"0123456789abcdef", {}};
  auto plain = check_join(&alone, stock, "imac");
  CHECK(plain.status == 200 && plain.slot == 0);
}

static void test_lead_action() {
  std::string error;
  auto a = parse_json(k_Example, error);
  std::string other_id = k_Example;
  other_id.replace(other_id.find("0123456789ABCDEF"), 16, "bbbbbbbbbbbbbbbb");
  auto b = parse_json(other_id, error);
  auto c = parse_json(R"({"v":1,"id":"cccccccccccccccc","slots":[{"slot":0}]})", error);
  CHECK(a && b && c);
  if (!a || !b || !c) {
    return;
  }
  CHECK(lead_action(&*a, *b, true) == lead_e::adopt);
  CHECK(lead_action(&*a, *b, false) == lead_e::adopt);
  CHECK(lead_action(&*a, *c, false) == lead_e::rebuild);
  CHECK(lead_action(&*a, *c, true) == lead_e::conflict);
  CHECK(lead_action(nullptr, *a, false) == lead_e::rebuild);
  CHECK(lead_action(nullptr, *a, true) == lead_e::conflict);
}

static void test_status_json() {
  auto json = nlohmann::json::parse(status_json(true, "0123456789abcdef", {{1, "\\\\.\\DISPLAY7", 1920, 1080, 3024, 442, true, true, "iMac (DisplayExtend)"}}));
  CHECK(json["active"] == true);
  CHECK(json["id"] == "0123456789abcdef");
  CHECK(json["slots"].size() == 1);
  auto &s = json["slots"][0];
  CHECK(s["slot"] == 1);
  CHECK(s["display"] == "\\\\.\\DISPLAY7");
  CHECK(s["w"] == 1920 && s["h"] == 1080);
  CHECK(s["x"] == 3024 && s["y"] == 442);
  CHECK(s["external"] == true && s["claimed"] == true);
  CHECK(s["client"] == "iMac (DisplayExtend)");

  auto idle = nlohmann::json::parse(status_json(false, "0123456789abcdef", {}));
  CHECK(idle["active"] == false);
  CHECK(idle["id"] == "");
  CHECK(idle["slots"].is_array() && idle["slots"].empty());

  // A name that isn't UTF-8 still makes JSON
  auto odd = nlohmann::json::parse(status_json(true, "0123456789abcdef", {{1, "x", 1, 1, 0, 0, false, true, "bad \xFF name"}}));
  CHECK(odd["slots"][0]["client"].is_string());
}

static void test_pair_request() {
  std::string error;
  auto ok = parse_pair_request(R"j({"pin":"4821","name":"iMac (DisplayExtend)","address":"192.168.1.31"})j", error);
  CHECK(ok);
  if (ok) {
    CHECK(ok->pin == "4821");
    CHECK(ok->name == "iMac (DisplayExtend)");
    CHECK(ok->address == "192.168.1.31");
  }
  auto nameless = parse_pair_request(R"({"pin":"0000","address":"fe80::1"})", error);
  CHECK(nameless && nameless->name.empty());
  auto long_name = parse_pair_request(R"({"pin":"0000","address":"10.0.0.2","name":")" + std::string(100, 'n') + R"("})", error);
  CHECK(long_name && long_name->name.size() == max_pair_name);

  for (auto bad : {
         R"(nonsense)",
         R"([])",
         R"({"address":"192.168.1.31"})",
         R"({"pin":4821,"address":"192.168.1.31"})",
         R"({"pin":"482","address":"192.168.1.31"})",
         R"({"pin":"48211","address":"192.168.1.31"})",
         R"({"pin":"48a1","address":"192.168.1.31"})",
         R"({"pin":"4821"})",
         R"({"pin":"4821","address":""})",
         R"({"pin":"4821","address":7})",
         R"({"pin":"4821","address":"192.168.1.31","name":5})",
       }) {
    error.clear();
    CHECK(!parse_pair_request(bad, error));
    CHECK(!error.empty());
  }
}

static void test_waiting() {
  std::vector<std::string> waiting {"192.168.1.31", "192.168.1.40", "192.168.1.40"};
  auto one = waiting_at(waiting, "192.168.1.31");
  CHECK(one.index == std::optional<std::size_t> {0});
  CHECK(one.count == 1);
  auto two = waiting_at(waiting, "192.168.1.40");
  CHECK(!two.index);
  CHECK(two.count == 2);
  auto none = waiting_at(waiting, "10.0.0.1");
  CHECK(!none.index && none.count == 0);
  CHECK(!waiting_at({}, "10.0.0.1").index);

  auto json = nlohmann::json::parse(not_waiting_json(waiting, 0));
  CHECK(json["error"] == "no device is waiting");
  CHECK(json["waiting"].size() == 3);
  CHECK(json["waiting"][0] == "192.168.1.31");
  CHECK(nlohmann::json::parse(not_waiting_json(waiting, 2))["error"] == "several devices are waiting at that address");
  CHECK(nlohmann::json::parse(not_waiting_json({}, 0))["waiting"].empty());
}

int main() {
  test_base64url();
  test_parse_example();
  test_parse_details();
  test_same_layout();
  test_layout();
  test_slot_guid();
  test_requests();
  test_check_join();
  test_lead_action();
  test_status_json();
  test_pair_request();
  test_waiting();

  if (s_Failures) {
    std::printf("%d display group policy test(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All display group policy tests passed\n");
  return 0;
}
