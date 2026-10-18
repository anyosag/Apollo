// Tests for src/fork/touch_emulation.h: stock Moonlight for iOS's Touchscreen mode, turned back into
// touch. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/touch_emulation_test.cpp
//
// The event sequences are what moonlight-ios's AbsoluteTouchHandler.m and RelativeTouchHandler.m send.

#include "fork/touch_emulation.h"

#include <cstdio>

using namespace touch_emulation;
using namespace std::chrono_literals;
using kind = step_t::kind_e;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static const clock::time_point t0 {};

static steps_t steps(std::initializer_list<step_t> list) {
  return steps_t(list);
}

static bool only_pass(const steps_t &s) {
  return s == steps({{kind::pass}});
}

// A finger in Touchscreen mode: position and left press together; left and right release at the finger up
static void tap(tracker_t &t, point_t p, clock::time_point at) {
  t.position(p, at);
  t.button(button_left, true, at + 1ms);
  t.button(button_left, false, at + 80ms);
  t.button(button_right, false, at + 81ms);
}

static void test_user_agent() {
  CHECK(is_moonlight_ios("Moonlight/1 CFNetwork/1568.200.51 Darwin/24.1.0"));
  CHECK(is_moonlight_ios("Moonlight/9.1.0 CFNetwork/3826.500.62 Darwin/25.0.0"));
  CHECK(!is_moonlight_ios("Moonlight%20TV/1 CFNetwork/1568.200.51 Darwin/24.1.0"));
  CHECK(!is_moonlight_ios("Moonlight TV/1 CFNetwork/1568.200.51 Darwin/24.1.0"));
  CHECK(!is_moonlight_ios("okhttp/4.12.0"));
  CHECK(!is_moonlight_ios("Moonlight/6.1.0"));
  CHECK(!is_moonlight_ios("Mozilla/5.0 Moonlight/1 CFNetwork/1 Darwin/1"));
  CHECK(!is_moonlight_ios(""));

  CHECK(printable("Moonlight/1 CFNetwork/1 Darwin/1") == "Moonlight/1 CFNetwork/1 Darwin/1");
  CHECK(printable("a\nb\x7f") == "a?b?");
  CHECK(printable(std::string(500, 'x')).size() == 160);
}

static void test_tap() {
  tracker_t t;
  point_t p {0.25f, 0.5f};

  // The first finger moves the mouse there too: nothing has shown fingers are in use yet
  CHECK(only_pass(t.position(p, t0)));
  CHECK(t.button(button_left, true, t0 + 2ms) == steps({{kind::touch_down, p}}));
  CHECK(t.button(button_left, false, t0 + 90ms).empty());
  CHECK(t.deadline() == t0 + 90ms + release_wait);
  CHECK(t.button(button_right, false, t0 + 91ms) == steps({{kind::touch_up, p}}));
  CHECK(!t.deadline());

  // The next one doesn't move the mouse first
  point_t q {0.75f, 0.1f};
  CHECK(t.position(q, t0 + 1s).empty());
  CHECK(t.button(button_left, true, t0 + 1s + 1ms) == steps({{kind::touch_down, q}}));
  CHECK(t.button(button_left, false, t0 + 1100ms).empty());
  CHECK(t.button(button_right, false, t0 + 1100ms) == steps({{kind::touch_up, q}}));

  CHECK(t.counts().contacts == 2);
  CHECK(t.counts().long_presses == 0);
  CHECK(t.counts().mouse_clicks == 0);
}

static void test_drag() {
  tracker_t t;
  t.position({0.1f, 0.1f}, t0);
  t.button(button_left, true, t0);
  CHECK(t.position({0.2f, 0.3f}, t0 + 16ms) == steps({{kind::touch_move, {0.2f, 0.3f}}}));
  CHECK(t.position({0.3f, 0.5f}, t0 + 32ms) == steps({{kind::touch_move, {0.3f, 0.5f}}}));
  CHECK(t.button(button_left, false, t0 + 40ms).empty());
  CHECK(t.button(button_right, false, t0 + 40ms) == steps({{kind::touch_up, {0.3f, 0.5f}}}));
}

static void test_long_press() {
  tracker_t t;
  point_t p {0.5f, 0.5f};
  t.position(p, t0);
  t.button(button_left, true, t0);

  // At 650 ms the client releases left and presses right, with the finger still down
  CHECK(t.button(button_left, false, t0 + 650ms).empty());
  CHECK(t.button(button_right, true, t0 + 651ms) == steps({{kind::touch_cancel, p}, {kind::mouse_move, p}, {kind::pass}}));
  CHECK(!t.deadline());

  // Moving now drags with the right button, as upstream would
  CHECK(only_pass(t.position({0.6f, 0.5f}, t0 + 700ms)));

  // At the finger up: left release (its press never reached Windows) and right release
  CHECK(t.button(button_left, false, t0 + 900ms).empty());
  CHECK(only_pass(t.button(button_right, false, t0 + 900ms)));

  CHECK(t.counts().contacts == 1);
  CHECK(t.counts().long_presses == 1);

  // Still a finger afterwards
  CHECK(t.position(p, t0 + 2s).empty());
  CHECK(t.button(button_left, true, t0 + 2s) == steps({{kind::touch_down, p}}));
}

static void test_double_tap() {
  tracker_t t;
  point_t p {0.4f, 0.4f};
  tap(t, p, t0);

  // Within 250 ms and close by, the client sends no position for the second tap
  CHECK(t.button(button_left, true, t0 + 200ms) == steps({{kind::touch_down, p}}));
  t.button(button_left, false, t0 + 260ms);
  t.button(button_right, false, t0 + 260ms);

  // Long after, a press without a position is a mouse
  CHECK(only_pass(t.button(button_left, true, t0 + 900ms)));
  CHECK(t.counts().contacts == 2);
  CHECK(t.counts().mouse_clicks == 1);
}

static void test_touchpad_mode() {
  tracker_t t;

  // Relative motion, then a tap to click: no positions at all
  CHECK(only_pass(t.pointer(t0)));
  CHECK(only_pass(t.pointer(t0 + 8ms)));
  CHECK(only_pass(t.button(button_left, true, t0 + 100ms)));
  CHECK(only_pass(t.button(button_left, false, t0 + 150ms)));

  // A tap without motion, right after
  CHECK(only_pass(t.button(button_left, true, t0 + 250ms)));
  CHECK(only_pass(t.button(button_left, false, t0 + 300ms)));

  // Two-finger tap: a right click
  CHECK(only_pass(t.button(button_right, true, t0 + 400ms)));
  CHECK(only_pass(t.button(button_right, false, t0 + 450ms)));

  CHECK(t.counts().contacts == 0);
  CHECK(t.counts().mouse_clicks == 2);
}

static void test_pointer_hover() {
  tracker_t t;

  // An iPad trackpad hovering, then clicking: many positions before the press
  CHECK(only_pass(t.position({0.1f, 0.1f}, t0)));
  CHECK(only_pass(t.position({0.2f, 0.1f}, t0 + 8ms)));
  CHECK(only_pass(t.position({0.3f, 0.1f}, t0 + 16ms)));
  CHECK(only_pass(t.button(button_left, true, t0 + 20ms)));
  CHECK(only_pass(t.position({0.4f, 0.1f}, t0 + 28ms)));
  CHECK(only_pass(t.button(button_left, false, t0 + 40ms)));

  // One position, then a click much later: the mouse moved once and stopped
  CHECK(only_pass(t.position({0.5f, 0.1f}, t0 + 1s)));
  CHECK(only_pass(t.button(button_left, true, t0 + 2s)));
  CHECK(t.counts().contacts == 0);
  CHECK(t.counts().mouse_clicks == 2);
}

static void test_hover_after_fingers() {
  tracker_t t;
  tap(t, {0.5f, 0.5f}, t0);

  // A trackpad's first position is held back, the second shows it's a pointer
  point_t a {0.1f, 0.2f};
  point_t b {0.15f, 0.2f};
  CHECK(t.position(a, t0 + 1s).empty());
  CHECK(only_pass(t.position(b, t0 + 1s + 8ms)));
  CHECK(only_pass(t.button(button_left, true, t0 + 1s + 10ms)));
  CHECK(t.counts().mouse_clicks == 1);
}

static void test_held_position_then_late_press() {
  tracker_t t;
  tap(t, {0.5f, 0.5f}, t0);

  // A single position, and a press well after: the mouse goes there first, then clicks
  point_t a {0.3f, 0.3f};
  CHECK(t.position(a, t0 + 1s).empty());
  CHECK(t.button(button_left, true, t0 + 1s + 200ms) == steps({{kind::mouse_move, a}, {kind::pass}}));
}

static void test_held_position_then_scroll() {
  tracker_t t;
  tap(t, {0.5f, 0.5f}, t0);

  point_t a {0.3f, 0.3f};
  CHECK(t.position(a, t0 + 1s).empty());
  CHECK(t.pointer(t0 + 1s + 5ms) == steps({{kind::mouse_move, a}, {kind::pass}}));
  CHECK(only_pass(t.position({0.35f, 0.3f}, t0 + 1s + 10ms)));
}

static void test_lift_without_right_release() {
  tracker_t t;
  point_t p {0.2f, 0.2f};
  t.position(p, t0);
  t.button(button_left, true, t0);
  t.button(button_left, false, t0 + 50ms);

  // A client that doesn't send the right release: the finger goes up at the deadline
  CHECK(t.expire(t0 + 50ms + release_wait - 1ms).empty());
  CHECK(t.expire(t0 + 50ms + release_wait) == steps({{kind::touch_up, p}}));
  CHECK(!t.deadline());
  CHECK(t.expire(t0 + 1s).empty());

  // Or the next finger arrives before the deadline: the first goes up, without dragging to it
  point_t q {0.8f, 0.8f};
  tracker_t u;
  u.position(p, t0);
  u.button(button_left, true, t0);
  u.button(button_left, false, t0 + 50ms);
  CHECK(u.position(q, t0 + 70ms) == steps({{kind::touch_up, p}}));
  CHECK(u.button(button_left, true, t0 + 71ms) == steps({{kind::touch_down, q}}));

  // Or presses again with no position (a double tap)
  tracker_t v;
  v.position(p, t0);
  v.button(button_left, true, t0);
  v.button(button_left, false, t0 + 50ms);
  CHECK(v.button(button_left, true, t0 + 90ms) == steps({{kind::touch_up, p}, {kind::touch_down, p}}));
}

static void test_mouse_during_finger() {
  tracker_t t;
  point_t p {0.2f, 0.2f};
  t.position(p, t0);
  t.button(button_left, true, t0);

  // A scroll from a trackpad while the finger is down cancels the finger
  CHECK(t.pointer(t0 + 30ms) == steps({{kind::touch_cancel, p}, {kind::pass}}));

  // The finger's releases then go to upstream, which ignores a button that isn't down
  CHECK(only_pass(t.button(button_left, false, t0 + 60ms)));
  CHECK(only_pass(t.button(button_right, false, t0 + 60ms)));

  // The middle button is a mouse
  tap(t, p, t0 + 1s);
  CHECK(only_pass(t.button(2, true, t0 + 2s)));
  CHECK(only_pass(t.position(p, t0 + 2s + 10ms)));
}

static void test_end() {
  tracker_t t;
  point_t p {0.2f, 0.2f};
  t.position(p, t0);
  t.button(button_left, true, t0);
  CHECK(t.end() == steps({{kind::touch_cancel, p}}));
  CHECK(t.end().empty());

  // After a long press, the right button is upstream's to release
  tracker_t u;
  u.position(p, t0);
  u.button(button_left, true, t0);
  u.button(button_left, false, t0 + 650ms);
  u.button(button_right, true, t0 + 650ms);
  CHECK(u.end().empty());
}

int main() {
  test_user_agent();
  test_tap();
  test_drag();
  test_long_press();
  test_double_tap();
  test_touchpad_mode();
  test_pointer_hover();
  test_hover_after_fingers();
  test_held_position_then_late_press();
  test_held_position_then_scroll();
  test_lift_without_right_release();
  test_mouse_during_finger();
  test_end();

  if (s_Failures) {
    std::printf("%d failure(s)\n", s_Failures);
    return 1;
  }
  std::printf("touch_emulation: all tests passed\n");
  return 0;
}
