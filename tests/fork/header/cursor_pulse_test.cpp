// Tests for src/fork/cursor_pulse.h: LuzLunar's cursor pulse on the PC's own cursor in the stream.
// The look over a pulse, settling back, the viewport scaled around the hotspot as the capture
// rotates it, the tint, the request and the state. A standalone program, so CI builds it without
// the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/cursor_pulse_test.cpp

#include "fork/cursor_pulse.h"

#include <cstdio>

using namespace fork_tweaks::cursor_pulse;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static bool near(float a, float b, float eps = 0.01f) {
  return std::fabs(a - b) <= eps;
}

static void test_pulses() {
  plan_t two {2, true, false, {}};
  CHECK(near(static_cast<float>(length_ms(two)), 340));
  // At rest at the start, at the height of each pulse half way through it, at rest between them
  CHECK(look_at(two, 0).rest());
  look_t peak = look_at(two, pulse_ms / 2);
  CHECK(near(peak.scale, peak_scale) && near(peak.tint, 1));
  CHECK(near(look_at(two, pulse_ms).scale, 1) && near(look_at(two, pulse_ms).tint, 0));
  look_t second = look_at(two, pulse_ms * 1.5);
  CHECK(near(second.scale, peak_scale) && near(second.tint, 1));
  // A quarter of the way in, half way up
  CHECK(near(look_at(two, pulse_ms / 4).tint, 0.5f));
  // Over: at rest
  CHECK(look_at(two, 340).rest() && look_at(two, 1000).rest() && look_at(two, -5).rest());

  // Reduce Motion: the tint only
  plan_t still {2, false, false, {}};
  look_t tinted = look_at(still, pulse_ms / 2);
  CHECK(near(tinted.scale, 1) && near(tinted.tint, 1));
}

static void test_settle() {
  plan_t settle {0, true, true, {1.3f, 0.8f}};
  CHECK(near(static_cast<float>(length_ms(settle)), static_cast<float>(settle_ms)));
  CHECK(near(look_at(settle, 0).scale, 1.3f) && near(look_at(settle, 0).tint, 0.8f));
  // Eased out: more than half way back at half the time
  look_t half = look_at(settle, settle_ms / 2);
  CHECK(half.scale < 1.15f && half.tint < 0.4f);
  CHECK(look_at(settle, settle_ms).rest());
}

static void test_view() {
  // Scaled around the hotspot: the hotspot stays put
  view_t view {100, 200, 32, 32};
  auto [px, py] = hotspot_in_view(0, 4, 2, 32, 32);
  view_t big = scaled(view, px, py, 1.5f);
  CHECK(near(big.x + px * 1.5f, view.x + px) && near(big.y + py * 1.5f, view.y + py));
  CHECK(near(big.w, 48) && near(big.h, 48));
  // Rotated as gpu_cursor_t::update_viewport() rotates the cursor
  CHECK((hotspot_in_view(1, 4, 2, 32, 20) == std::pair<float, float> {2, 28}));
  CHECK((hotspot_in_view(2, 4, 2, 32, 20) == std::pair<float, float> {28, 18}));
  CHECK((hotspot_in_view(3, 4, 2, 32, 20) == std::pair<float, float> {18, 4}));
}

static void test_tint() {
  std::uint8_t pixels[] = {255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 0};
  tint_bgra(pixels, 3, 1);
  // White becomes the accent (BGRA), black stays, alpha is kept
  CHECK(pixels[0] == 255 && pixels[1] == 180 && pixels[2] == 159 && pixels[3] == 255);
  CHECK(pixels[4] == 0 && pixels[5] == 0 && pixels[6] == 0);
  CHECK(pixels[11] == 0);
  std::uint8_t none[] = {200, 100, 50, 255};
  tint_bgra(none, 1, 0);
  CHECK(none[0] == 200 && none[1] == 100 && none[2] == 50);
}

static void test_request() {
  auto pulse = parse_request(R"({"pulse": 2, "ifAtTop": true})");
  CHECK(pulse && pulse->pulses == 2 && pulse->if_at_top && !pulse->reduce_motion);
  auto settle = parse_request(R"({"pulse": 0})");
  CHECK(settle && settle->pulses == 0 && !settle->if_at_top);
  auto many = parse_request(R"({"pulse": 40, "reduceMotion": true})");
  CHECK(many && many->pulses == max_pulses && many->reduce_motion);
  CHECK(!parse_request("") && !parse_request("[]") && !parse_request(R"({"pulse": "2"})") && !parse_request("{"));
}

static void test_state() {
  using namespace std::chrono_literals;
  state_t state;
  auto t0 = state_t::clock::time_point {} + 10s;
  CHECK(!state.animating(t0) && state.look(t0).rest());

  state.start({2, true, false}, t0);
  CHECK(state.animating(t0 + 100ms));
  CHECK(near(state.look(t0 + 85ms).scale, peak_scale));
  // Animating a little past the end, so the frame at rest goes out
  CHECK(state.animating(t0 + 360ms) && state.look(t0 + 360ms).rest());
  CHECK(!state.animating(t0 + 400ms));

  // Cut short half way up a pulse: settles back from there
  state.start({2, true, false}, t0);
  state.start({0, false, false}, t0 + 40ms);
  look_t from = state.look(t0 + 40ms);
  CHECK(from.scale > 1.1f && from.tint > 0.4f);
  CHECK(state.look(t0 + 40ms + 150ms).rest());
  // Settling at rest does nothing
  state.start({0, false, false}, t0 + 5s);
  CHECK(!state.animating(t0 + 5s));
}

int main() {
  test_pulses();
  test_settle();
  test_view();
  test_tint();
  test_request();
  test_state();

  if (s_Failures == 0) {
    std::printf("cursor_pulse: all tests passed\n");
    return 0;
  }
  std::printf("cursor_pulse: %d failure(s)\n", s_Failures);
  return 1;
}
