// Tests for src/fork/vdisplay_mode.h: the mode Apollo gives a virtual display. A standalone
// program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/vdisplay_mode_test.cpp

#include "fork/vdisplay_mode.h"

#include <cstdio>

using namespace vdisplay_mode;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void test_scale() {
  CHECK(effective_scale(100, 100) == 100);
  CHECK(effective_scale(150, 100) == 150);

  // The app's own factor wins
  CHECK(effective_scale(150, 50) == 50);

  // Out of range from the client
  CHECK(effective_scale(0, 100) == 100);
  CHECK(effective_scale(-50, 100) == 100);
  CHECK(effective_scale(4294967246LL, 100) == 100);
  CHECK(effective_scale(401, 100) == 100);
  CHECK(effective_scale(20, 100) == 20);
  CHECK(effective_scale(400, 100) == 400);
}

static void test_mode() {
  // MacBook Pro 14": full panel and below the notch
  CHECK((for_client(3024, 1964, 120000, 100, 1) == display_mode_t {3024, 1964, 120000}));
  CHECK((for_client(3024, 1890, 60000, 100, 1) == display_mode_t {3024, 1890, 60000}));

  // Defaults
  CHECK((for_client(0, 0, 0, 100, 1) == display_mode_t {1920, 1080, 60000}));
  CHECK((for_client(1920, 1080, -5, 100, 1) == display_mode_t {1920, 1080, 60000}));

  // Hertz from old clients
  CHECK((for_client(1920, 1080, 60, 100, 1) == display_mode_t {1920, 1080, 60000}));

  // Scaled, rounded down to even
  CHECK((for_client(3024, 1964, 60000, 50, 1) == display_mode_t {1512, 982, 60000}));
  CHECK((for_client(1366, 768, 60000, 75, 1) == display_mode_t {1024, 576, 60000}));
  CHECK((for_client(1001, 1001, 60000, 150, 1) == display_mode_t {1500, 1500, 60000}));

  // Fractional rates and multiples
  CHECK((for_client(1920, 1080, 59940, 100, 2) == display_mode_t {1920, 1080, 119880}));
  CHECK((for_client(3024, 1964, 120000, 100, 4) == display_mode_t {3024, 1964, 480000}));
  CHECK((for_client(3840, 2160, 120000, 100, 4) == display_mode_t {3840, 2160, 480000}));
  CHECK((for_client(1920, 1080, 0, 100, 4) == display_mode_t {1920, 1080, 240000}));
  CHECK((for_client(1920, 1080, 120000, 100, 0) == display_mode_t {1920, 1080, 120000}));

  // Above 500 Hz, or 8 billion pixels a second, the multiple halves until the driver takes it
  CHECK((for_client(2560, 1440, 144000, 100, 4) == display_mode_t {2560, 1440, 288000}));
  CHECK((for_client(1920, 1080, 240000, 100, 4) == display_mode_t {1920, 1080, 480000}));
  CHECK((for_client(1920, 1080, 360000, 100, 4) == display_mode_t {1920, 1080, 360000}));
  CHECK((for_client(7680, 4320, 120000, 100, 4) == display_mode_t {7680, 4320, 240000}));
  CHECK((for_client(7680, 4320, 240000, 100, 2) == display_mode_t {7680, 4320, 240000}));
}

static void test_stream_rate() {
  CHECK(stream_mhz(120000) == 120000);
  CHECK(stream_mhz(59940) == 59940);
  CHECK(stream_mhz(60) == 60000);
  CHECK(stream_mhz(0) == 60000);
  CHECK(stream_mhz(-1) == 60000);

  CHECK(within_limits(1920, 1080, 500000));
  CHECK(!within_limits(1920, 1080, 500001));
  CHECK(within_limits(7680, 4320, 240000));
  CHECK(!within_limits(7680, 4320, 241200));
}

static void test_slower() {
  // 4x falls back to 2x, then 1x
  CHECK((slower_rates({3024, 1964, 480000}, 120000) == std::vector<uint32_t> {240000, 120000}));
  CHECK((slower_rates({3024, 1964, 240000}, 120000) == std::vector<uint32_t> {120000}));
  CHECK(slower_rates({3024, 1964, 120000}, 120000).empty());
  CHECK(slower_rates({3024, 1964, 480000}, 0).empty());
}

static void test_runs() {
  display_mode_t want {3024, 1964, 120000};
  CHECK(runs(want, 3024, 1964, 120000));
  CHECK(runs(want, 3024, 1964, 119999));
  CHECK(runs(want, 3024, 1964, 120400));
  CHECK(!runs(want, 3024, 1964, 60000));
  CHECK(!runs(want, 3024, 1890, 120000));
  CHECK(!runs(want, 1512, 1964, 120000));
}

int main() {
  test_scale();
  test_mode();
  test_stream_rate();
  test_slower();
  test_runs();

  if (s_Failures) {
    std::printf("%d virtual display mode checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All virtual display mode tests passed\n");
  return 0;
}
