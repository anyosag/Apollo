// Tests for src/fork/luzvda_desc.h: the LuzVDA driver's requests built from a client's screen.
// A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src -Ithird-party tests/fork/header/luzvda_desc_test.cpp

#include "fork/luzvda_desc.h"

#include <cstdio>

using namespace luzvda_desc;
using client_display::description_t;
using vdisplay_mode::display_mode_t;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static const guid_bytes_t k_Guid {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static description_t macbook(display_mode_t render) {
  client_display::screen_t s;
  s.width_mm = 302;
  s.height_mm = 196;
  s.panel_width = 3024;
  s.panel_height = 1964;
  s.safe_width = 3024;
  s.safe_height = 1890;
  s.max_hz = 120;
  s.p3 = true;
  s.hdr = client_display::hdr_e::known;
  s.peak_nits = 1600;
  s.full_frame_nits = 1000;
  s.min_luminance = 50;
  return client_display::describe(s, render);
}

static void test_display() {
  auto d = display(macbook({3024, 1890, 120000}), "MacBook Pro", k_Guid);
  CHECK(d.struct_size == sizeof(LUZVDA_DISPLAY_DESC));
  CHECK(d.flags == LUZVDA_DISPLAY_HDR);
  CHECK(std::memcmp(d.monitor_guid.bytes, k_Guid.data(), 16) == 0);
  CHECK(std::string(d.name) == "MacBook Pro");
  CHECK(d.width_mm == 302 && d.height_mm == 196);
  CHECK(d.primaries[0] == client_display::p3_primaries[0] && d.primaries[7] == client_display::p3_primaries[7]);
  CHECK(d.max_luminance == 1600 && d.max_full_frame_luminance == 1000 && d.min_luminance == 50);
  CHECK(d.mode_count == 4);
  CHECK(d.modes[d.preferred_mode].width == 3024 && d.modes[d.preferred_mode].height == 1890 && d.modes[d.preferred_mode].refresh_mhz == 120000);
  CHECK(offers(d, {3024, 1964, 60000}));
  CHECK(!offers(d, {1920, 1080, 60000}));

  // SDR: no luminance, which the driver would refuse without the HDR flag anyway
  client_display::screen_t sdr;
  sdr.panel_width = 1920;
  sdr.panel_height = 1080;
  sdr.safe_width = 1920;
  sdr.safe_height = 1080;
  d = display(client_display::describe(sdr, {1920, 1080, 60000}), "TV", k_Guid);
  CHECK(d.flags == 0 && d.max_luminance == 0 && d.min_luminance == 0);
}

static void test_names() {
  // 63 bytes and a NUL, cut at a character boundary
  std::string long_name(70, 'a');
  auto d = display(macbook({3024, 1964, 120000}), long_name, k_Guid);
  CHECK(std::string(d.name) == std::string(63, 'a'));
  CHECK(d.name[63] == '\0');

  // "é" is two bytes; the 62 before it leave room for only one
  std::string accented = std::string(62, 'a') + "\xc3\xa9";
  d = display(macbook({3024, 1964, 120000}), accented, k_Guid);
  CHECK(std::string(d.name) == std::string(62, 'a'));

  // A NUL inside ends the name
  d = display(macbook({3024, 1964, 120000}), std::string("ab\0cd", 5), k_Guid);
  CHECK(std::string(d.name) == "ab");
  CHECK(d.name[3] == '\0');
}

static void test_modes() {
  // More modes than the protocol carries are cut to 32, keeping the preferred one valid
  description_t many;
  for (uint32_t i = 0; i < 40; ++i) {
    many.modes.push_back({1920 + i * 8, 1080, 60000});
  }
  many.preferred = 35;
  auto d = display(many, "x", k_Guid);
  CHECK(d.mode_count == LUZVDA_MAX_MODES);
  CHECK(d.preferred_mode == 0);

  // From a display's description: the same modes, preferred included
  auto described = display(macbook({1512, 982, 60000}), "MacBook Pro", k_Guid);
  auto from_desc = update(described);
  CHECK(from_desc.struct_size == sizeof(LUZVDA_UPDATE_MODES));
  CHECK(std::memcmp(from_desc.monitor_guid.bytes, k_Guid.data(), 16) == 0);
  CHECK(from_desc.mode_count == described.mode_count && from_desc.preferred_mode == described.preferred_mode);
  CHECK(std::memcmp(from_desc.modes, described.modes, sizeof(LUZVDA_MODE) * described.mode_count) == 0);

  auto u = update(macbook({1512, 982, 60000}), k_Guid);
  CHECK(u.struct_size == sizeof(LUZVDA_UPDATE_MODES));
  CHECK(std::memcmp(u.monitor_guid.bytes, k_Guid.data(), 16) == 0);
  CHECK(u.mode_count == 6);
  CHECK(u.modes[u.preferred_mode].width == 1512 && u.modes[u.preferred_mode].refresh_mhz == 60000);
}

static void test_same_screen() {
  auto a = display(macbook({3024, 1964, 120000}), "MacBook Pro", k_Guid);

  // Another stream mode on the same screen keeps the monitor
  auto b = display(macbook({1512, 982, 60000}), "MacBook Pro", k_Guid);
  CHECK(same_screen(a, b));

  // A different brightness, gamut, size or name is another screen to Windows
  auto c = a;
  c.max_luminance = 1000;
  CHECK(!same_screen(a, c));
  c = a;
  c.primaries[0] = 0;
  CHECK(!same_screen(a, c));
  c = a;
  c.width_mm = 600;
  CHECK(!same_screen(a, c));
  c = a;
  c.name[0] = 'X';
  CHECK(!same_screen(a, c));
  c = a;
  c.flags = 0;
  CHECK(!same_screen(a, c));
}

int main() {
  test_display();
  test_names();
  test_modes();
  test_same_screen();

  if (s_Failures) {
    std::printf("%d LuzVDA descriptor checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All LuzVDA descriptor tests passed\n");
  return 0;
}
