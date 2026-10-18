// Tests for src/fork/touchpad.h: LuzLunar's trackpad frames turned into synthetic touchpad
// contacts. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/touchpad_test.cpp

#include "fork/touchpad.h"

#include <cstdio>
#include <initializer_list>

using namespace fork_touchpad;
using luztouchpad::contact;
using luztouchpad::frame;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

constexpr std::uint32_t touching = flag_inrange | flag_incontact | flag_confidence;
constexpr std::int64_t ms = 1000;

static frame make(std::uint32_t time_ms, std::initializer_list<contact> contacts, std::uint8_t buttons = 0, std::uint8_t flags = 0) {
  frame f;
  f.time_ms = time_ms;
  f.buttons = buttons;
  f.flags = flags;
  f.width = 13000;
  f.height = 8000;
  for (auto &c : contacts) {
    f.contacts[f.count++] = c;
  }
  return f;
}

static host_t make_host() {
  host_t h;
  h.set_device_size(13000, 8000);
  return h;
}

static const entry_t *find(const injection_t &i, std::uint32_t id) {
  for (std::size_t n = 0; n < i.count; n++) {
    if (i.entries[n].pointer_id == id) {
      return &i.entries[n];
    }
  }
  return nullptr;
}

static void test_one_finger() {
  host_t h = make_host();
  CHECK(!h.active());

  auto down = h.apply(make(1000, {{0, luztouchpad::phase_down, 0, 65535}}), 50 * ms);
  CHECK(down.count == 1);
  CHECK(down.entries[0].pointer_id == 0);
  CHECK(down.entries[0].flags == touching);
  CHECK(down.entries[0].button_change == button_change_e::none);
  CHECK(down.entries[0].x == 0);
  CHECK(down.entries[0].y == 8000);
  CHECK(down.time_us == 50 * ms);  // the gesture's first frame anchors the clocks
  CHECK(h.active());

  // The client's 8 ms step is kept even though the frame took 10 ms to arrive
  auto move = h.apply(make(1008, {{0, luztouchpad::phase_move, 32768, 32768}}), 60 * ms);
  CHECK(move.count == 1);
  CHECK(move.entries[0].flags == touching);
  CHECK(move.entries[0].x == 6500);
  CHECK(move.entries[0].y == 4000);
  CHECK(move.time_us == 58 * ms);

  auto up = h.apply(make(1016, {{0, luztouchpad::phase_up, 32768, 32768}}), 68 * ms);
  CHECK(up.count == 1);
  CHECK(up.entries[0].flags == flag_confidence);  // lifted: only "confident"
  CHECK(!h.active());

  // Nothing is left to inject
  auto after = h.apply(make(1024, {}), 76 * ms);
  CHECK(after.count == 0);
}

static void test_missing_slot_lifts() {
  host_t h = make_host();
  h.apply(make(0, {{0, luztouchpad::phase_down, 100, 100}, {1, luztouchpad::phase_down, 200, 200}}), 0);
  auto i = h.apply(make(8, {{1, luztouchpad::phase_move, 300, 300}}), 8 * ms);
  CHECK(i.count == 2);
  auto zero = find(i, 0);
  auto one = find(i, 1);
  CHECK(zero != nullptr && zero->flags == flag_confidence);
  CHECK(one != nullptr && one->flags == touching);
  // Entries come in slot order
  CHECK(i.entries[0].pointer_id == 0 && i.entries[1].pointer_id == 1);

  // Slot 0 is free again and slot 1 still held
  auto next = h.apply(make(16, {{1, luztouchpad::phase_move, 300, 300}}), 16 * ms);
  CHECK(next.count == 1 && next.entries[0].pointer_id == 1);
}

static void test_cancel() {
  host_t h = make_host();
  h.apply(make(0, {{2, luztouchpad::phase_down, 100, 100}}), 0);
  auto i = h.apply(make(8, {{2, luztouchpad::phase_cancel, 100, 100}}), 8 * ms);
  CHECK(i.count == 1);
  CHECK(i.entries[0].flags == (flag_canceled | flag_confidence));
  CHECK(!h.active());

  // A cancel-all frame lifts everything and lets go of the button
  h.apply(make(16, {{0, luztouchpad::phase_down, 1, 1}, {3, luztouchpad::phase_down, 2, 2}}, luztouchpad::button_pressed), 16 * ms);
  auto all = h.apply(make(24, {}, 0, luztouchpad::flag_cancel_all), 24 * ms);
  CHECK(all.count == 2);
  for (std::size_t n = 0; n < all.count; n++) {
    CHECK(all.entries[n].flags == (flag_canceled | flag_confidence));
    CHECK(all.entries[n].button_change == button_change_e::up);
  }
  CHECK(!h.active());

  // cancel_all() with nothing held injects nothing
  CHECK(h.cancel_all(30 * ms).count == 0);

  // An up or cancel for a finger the host doesn't hold is ignored
  CHECK(h.apply(make(40, {{1, luztouchpad::phase_up, 0, 0}}), 40 * ms).count == 0);
  CHECK(h.apply(make(48, {{1, luztouchpad::phase_cancel, 0, 0}}), 48 * ms).count == 0);
}

static void test_adopted() {
  host_t h = make_host();
  // The down was lost; the move is taken as the finger's start
  auto i = h.apply(make(0, {{4, luztouchpad::phase_move, 10, 10}}), 0);
  CHECK(i.count == 1 && i.entries[0].pointer_id == 4 && i.entries[0].flags == touching);
  CHECK(h.adopted() == 1);
  // A down for a finger already held is just a move
  i = h.apply(make(8, {{4, luztouchpad::phase_down, 20, 20}}), 8 * ms);
  CHECK(i.count == 1 && i.entries[0].flags == touching);
  CHECK(h.adopted() == 1);
}

static void test_button() {
  host_t h = make_host();
  auto press = h.apply(make(0, {{0, luztouchpad::phase_down, 10, 10}}, luztouchpad::button_pressed), 0);
  CHECK(press.entries[0].flags == (touching | flag_firstbutton));
  CHECK(press.entries[0].button_change == button_change_e::down);

  auto hold = h.apply(make(8, {{0, luztouchpad::phase_move, 20, 20}}, luztouchpad::button_pressed), 8 * ms);
  CHECK(hold.entries[0].flags == (touching | flag_firstbutton));
  CHECK(hold.entries[0].button_change == button_change_e::none);

  // A second finger joins while pressed: both carry the button
  auto two = h.apply(make(16, {{0, luztouchpad::phase_move, 20, 20}, {1, luztouchpad::phase_down, 30, 30}}, luztouchpad::button_pressed), 16 * ms);
  CHECK(two.count == 2);
  CHECK(two.entries[1].flags == (touching | flag_firstbutton));

  auto release = h.apply(make(24, {{0, luztouchpad::phase_move, 20, 20}, {1, luztouchpad::phase_move, 30, 30}}), 24 * ms);
  CHECK(release.entries[0].flags == touching);
  CHECK(release.entries[0].button_change == button_change_e::up);
  CHECK(release.entries[1].button_change == button_change_e::up);
}

static void test_button_released_with_last_finger() {
  host_t h = make_host();
  h.apply(make(0, {{0, luztouchpad::phase_down, 10, 10}}, luztouchpad::button_pressed), 0);
  // The Mac reports the lift before the click's release
  auto lift = h.apply(make(8, {{0, luztouchpad::phase_up, 10, 10}}, luztouchpad::button_pressed), 8 * ms);
  CHECK(lift.count == 1);
  CHECK(lift.entries[0].flags == flag_confidence);
  CHECK(lift.entries[0].button_change == button_change_e::up);
  CHECK(!h.active());

  // The late release changes nothing
  CHECK(h.apply(make(12, {}), 12 * ms).count == 0);
  CHECK(!h.active());

  // A press with no finger on the pad doesn't hold anything either
  CHECK(h.apply(make(20, {}, luztouchpad::button_pressed), 20 * ms).count == 0);
  CHECK(!h.active());
}

static void test_scaling() {
  host_t h = make_host();
  auto i = h.apply(make(0, {{0, luztouchpad::phase_down, 65535, 0}, {1, luztouchpad::phase_down, 1, 65534}}), 0);
  CHECK(i.entries[0].x == 13000 && i.entries[0].y == 0);
  CHECK(i.entries[1].x == 0 && i.entries[1].y == 8000);

  // Without a device size there's nothing to scale to
  host_t none;
  auto j = none.apply(make(0, {{0, luztouchpad::phase_down, 65535, 65535}}), 0);
  CHECK(j.count == 1 && j.entries[0].x == 0 && j.entries[0].y == 0);
}

static void test_time() {
  // The client's clock runs ahead of the host's: never inject from the future
  {
    host_t h = make_host();
    h.apply(make(0, {{0, luztouchpad::phase_down, 0, 0}}), 100 * ms);
    auto i = h.apply(make(50, {{0, luztouchpad::phase_move, 0, 0}}), 120 * ms);
    CHECK(i.time_us == 120 * ms);
    // The anchor moved, so the next step follows the client again
    i = h.apply(make(58, {{0, luztouchpad::phase_move, 0, 0}}), 130 * ms);
    CHECK(i.time_us == 128 * ms);
  }

  // Frames held up by the network: no more than 300 ms behind now
  {
    host_t h = make_host();
    h.apply(make(0, {{0, luztouchpad::phase_down, 0, 0}}), 0);
    auto i = h.apply(make(10, {{0, luztouchpad::phase_move, 0, 0}}), 1000 * ms);
    CHECK(i.time_us == 700 * ms);
    i = h.apply(make(18, {{0, luztouchpad::phase_move, 0, 0}}), 1001 * ms);
    CHECK(i.time_us == 708 * ms);
  }

  // Two frames with the same client time still get increasing times
  {
    host_t h = make_host();
    auto a = h.apply(make(500, {{0, luztouchpad::phase_down, 0, 0}}), 10 * ms);
    auto b = h.apply(make(500, {{0, luztouchpad::phase_move, 5, 5}}), 11 * ms);
    CHECK(b.time_us == a.time_us + min_step_us);
  }

  // The client's clock wraps
  {
    host_t h = make_host();
    h.apply(make(0xFFFFFFF0u, {{0, luztouchpad::phase_down, 0, 0}}), 100 * ms);
    auto i = h.apply(make(0x00000008u, {{0, luztouchpad::phase_move, 0, 0}}), 200 * ms);
    CHECK(i.time_us == 124 * ms);
  }

  // A new gesture anchors again
  {
    host_t h = make_host();
    h.apply(make(0, {{0, luztouchpad::phase_down, 0, 0}}), 0);
    h.apply(make(8, {{0, luztouchpad::phase_up, 0, 0}}), 8 * ms);
    auto i = h.apply(make(5000, {{0, luztouchpad::phase_down, 0, 0}}), 9000 * ms);
    CHECK(i.time_us == 9000 * ms);
  }
}

static void test_tick() {
  host_t h = make_host();
  CHECK(h.tick(0) == tick_e::idle);

  h.apply(make(0, {{0, luztouchpad::phase_down, 10, 10}}), 0);
  h.injected(0);
  CHECK(h.tick(100 * ms) == tick_e::wait);
  CHECK(h.tick(repeat_after_us) == tick_e::repeat);

  auto again = h.repeat(repeat_after_us);
  CHECK(again.count == 1 && again.entries[0].flags == touching);
  CHECK(again.entries[0].x == h.apply(make(1, {{0, luztouchpad::phase_move, 10, 10}}), repeat_after_us + ms).entries[0].x);

  // The client spoke at 251 ms, so silence counts from there
  h.injected(repeat_after_us + ms);
  CHECK(h.tick(repeat_after_us + 100 * ms) == tick_e::wait);
  CHECK(h.tick(2 * repeat_after_us + ms) == tick_e::repeat);
  h.injected(2 * repeat_after_us + ms);
  CHECK(h.tick(2 * repeat_after_us + 100 * ms) == tick_e::wait);  // just repeated

  CHECK(h.tick(repeat_after_us + ms + cancel_after_us) == tick_e::cancel);
  h.cancel_all(repeat_after_us + ms + cancel_after_us);
  CHECK(h.tick(repeat_after_us + ms + cancel_after_us + ms) == tick_e::idle);
}

static void test_repeat_time_increases() {
  host_t h = make_host();
  auto first = h.apply(make(0, {{0, luztouchpad::phase_down, 10, 10}}), 100 * ms);
  auto again = h.repeat(100 * ms);
  CHECK(again.time_us == first.time_us + min_step_us);
  CHECK(h.repeat(500 * ms).time_us == 500 * ms);
}

static void test_recreate() {
  host_t h = make_host();
  CHECK(!h.needs_recreate(13000, 8000));
  CHECK(h.needs_recreate(16000, 10000));
  h.apply(make(0, {{0, luztouchpad::phase_down, 10, 10}}), 0);
  // Never while a finger is down
  CHECK(!h.needs_recreate(16000, 10000));
  h.apply(make(8, {}), 8 * ms);
  CHECK(h.needs_recreate(16000, 10000));
  h.set_device_size(16000, 10000);
  CHECK(!h.needs_recreate(16000, 10000));
  CHECK(h.width() == 16000 && h.height() == 10000);
}

int main() {
  test_one_finger();
  test_missing_slot_lifts();
  test_cancel();
  test_adopted();
  test_button();
  test_button_released_with_last_finger();
  test_scaling();
  test_time();
  test_tick();
  test_repeat_time_increases();
  test_recreate();

  if (s_Failures == 0) {
    std::printf("touchpad: all tests passed\n");
    return 0;
  }
  std::printf("touchpad: %d failure(s)\n", s_Failures);
  return 1;
}
