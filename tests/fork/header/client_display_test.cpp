// Tests for src/fork/client_display.h: the client's screen and the virtual display built from it.
// A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/client_display_test.cpp

#include "fork/client_display.h"

#include <cstdio>
#include <map>

using namespace client_display;
using vdisplay_mode::display_mode_t;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static lookup_t args(std::map<std::string, std::string> m) {
  return [m](std::string_view key) -> std::optional<std::string> {
    auto it = m.find(std::string(key));
    if (it == m.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

// What LuzLunar sends for a 14" MacBook Pro
static std::map<std::string, std::string> macbook() {
  return {
    {"luzDisplay", "1"},
    {"luzMm", "302x196"},
    {"luzPanel", "3024x1964"},
    {"luzSafe", "3024x1890"},
    {"luzHz", "120"},
    {"luzGamut", "p3"},
    {"luzHdr", "1600,1000,50"},
    {"luzEdr", "1600"},
    {"luzName", "Built-in Liquid Retina XDR Display"},
  };
}

static void test_parse() {
  auto s = parse(args(macbook()));
  CHECK(s.has_value());
  if (!s) {
    return;
  }
  CHECK(s->width_mm == 302 && s->height_mm == 196);
  CHECK(s->panel_width == 3024 && s->panel_height == 1964);
  CHECK(s->safe_width == 3024 && s->safe_height == 1890);
  CHECK(s->max_hz == 120);
  CHECK(s->p3);
  CHECK(s->hdr == hdr_e::known && s->peak_nits == 1600 && s->full_frame_nits == 1000 && s->min_luminance == 50);
  CHECK(s->edr_x100 == 1600);
  CHECK(s->name == "Built-in Liquid Retina XDR Display");
}

static void test_parse_rejects() {
  // Not sent, or another version
  CHECK(!parse(args({})));
  auto m = macbook();
  m["luzDisplay"] = "2";
  CHECK(!parse(args(m)));

  // No usable panel size
  m = macbook();
  m.erase("luzPanel");
  CHECK(!parse(args(m)));
  m["luzPanel"] = "100x100";
  CHECK(!parse(args(m)));
  m["luzPanel"] = "3024x";
  CHECK(!parse(args(m)));
  m["luzPanel"] = "-3024x1964";
  CHECK(!parse(args(m)));

  // Bad values are dropped one by one
  m = macbook();
  m["luzSafe"] = "4000x4000";  // larger than the panel
  m["luzMm"] = "99999x10";
  m["luzHz"] = "100000";
  m["luzHdr"] = "20000,1000,50";  // brighter than HDR10 goes
  auto s = parse(args(m));
  CHECK(s && s->safe_width == 3024 && s->safe_height == 1964);
  CHECK(s && s->width_mm == 0 && s->max_hz == 0);
  CHECK(s && s->hdr == hdr_e::unknown);

  m["luzHdr"] = "none";
  CHECK(parse(args(m))->hdr == hdr_e::none);
  m["luzHdr"] = "unknown";
  CHECK(parse(args(m))->hdr == hdr_e::unknown);
  m["luzHdr"] = "1000,2000,0";  // full screen brighter than peak
  CHECK(parse(args(m))->hdr == hdr_e::unknown);
  m["luzHdr"] = "1000,0,0";  // full screen not given
  s = parse(args(m));
  CHECK(s->hdr == hdr_e::known && s->full_frame_nits == 1000);

  m["luzName"] = std::string(62, 'a') + "\xc3\xa9";
  CHECK(parse(args(m))->name == std::string(62, 'a'));
}

static bool has(const description_t &d, display_mode_t m) {
  return std::find(d.modes.begin(), d.modes.end(), m) != d.modes.end();
}

static void test_describe_macbook() {
  auto s = parse(args(macbook()));
  auto d = describe(s, {3024, 1964, 120000});
  CHECK(d.width_mm == 302 && d.height_mm == 196);
  CHECK(d.primaries == p3_primaries);
  CHECK(d.hdr && d.max_luminance == 1600 && d.max_full_frame_luminance == 1000 && d.min_luminance == 50);

  // The stream's mode first, then the other shape and rates
  CHECK((d.modes[d.preferred] == display_mode_t {3024, 1964, 120000}));
  CHECK(has(d, {3024, 1890, 120000}));
  CHECK(has(d, {3024, 1890, 60000}));
  CHECK(has(d, {3024, 1964, 60000}));
  CHECK(d.modes.size() == 4);

  // Half size at 60 Hz: its own shape plus the screen's
  d = describe(s, {1512, 982, 60000});
  CHECK((d.modes[0] == display_mode_t {1512, 982, 60000}));
  CHECK(has(d, {1512, 982, 120000}));
  CHECK(has(d, {3024, 1890, 120000}));
  CHECK(d.modes.size() == 6);
}

static void test_describe_defaults() {
  // Moonlight: SudoVDA's description, but only the stream's modes
  auto d = describe(std::nullopt, {1920, 1080, 59940});
  CHECK(d.width_mm == default_width_mm && d.height_mm == default_height_mm);
  CHECK(d.hdr && d.max_luminance == default_nits);
  CHECK((d.primaries == std::array<uint16_t, 8> {}));
  CHECK(d.modes.size() == 2);
  CHECK((d.modes[0] == display_mode_t {1920, 1080, 59940}));
  CHECK(has(d, {1920, 1080, 60000}));

  // HDR of unknown brightness, and none
  auto m = macbook();
  m["luzHdr"] = "unknown";
  m["luzGamut"] = "srgb";
  d = describe(parse(args(m)), {3024, 1964, 120000});
  CHECK(d.hdr && d.max_luminance == unknown_peak_nits && d.min_luminance == unknown_min_luminance);
  CHECK((d.primaries == std::array<uint16_t, 8> {}));
  m["luzHdr"] = "none";
  d = describe(parse(args(m)), {3024, 1964, 120000});
  CHECK(!d.hdr && d.max_luminance == 0);

  // A stream mode out of range leaves 1080p60
  d = describe(std::nullopt, {100, 100, 60000});
  CHECK((d.modes.size() == 1 && d.modes[0] == display_mode_t {1920, 1080, 60000}));

  // Modes the driver would refuse, failing the whole display, are left out: over its pixel rate,
  // and over 500 Hz
  d = describe(std::nullopt, {7680, 4320, 480000});
  CHECK((d.modes.size() == 1 && d.modes[0] == display_mode_t {7680, 4320, 60000}));
  d = describe(std::nullopt, {1920, 1080, 1000000});
  CHECK((d.modes.size() == 1 && d.modes[0] == display_mode_t {1920, 1080, 60000}));
  for (const auto &mode : describe(parse(args(macbook())), {3024, 1964, 120000}).modes) {
    CHECK((uint64_t) mode.width * mode.height * mode.refresh_mhz / 1000 <= max_pixels_per_second);
  }
}

static void test_summary() {
  auto s = parse(args(macbook()));
  CHECK(summary(*s) == "3024x1964 (3024x1890 below the notch), 302x196 mm, 120 Hz, P3, HDR 1600/1000 nits, EDR headroom 16.00");

  auto m = macbook();
  m.erase("luzSafe");
  m.erase("luzMm");
  m.erase("luzHz");
  m["luzHdr"] = "none";
  m["luzGamut"] = "srgb";
  m["luzEdr"] = "105";
  CHECK(summary(*parse(args(m))) == "3024x1964, sRGB, SDR, EDR headroom 1.05");
}

int main() {
  test_summary();
  test_parse();
  test_parse_rejects();
  test_describe_macbook();
  test_describe_defaults();

  if (s_Failures) {
    std::printf("%d client display checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All client display tests passed\n");
  return 0;
}
