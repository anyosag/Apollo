// Smoke test for trackpad passthrough on Windows: makes a real synthetic touchpad and plays
// gestures on it through fork_touchpad::host_t, the code Apollo feeds LuzLunar's frames to.
//
// It moves the pointer, scrolls whatever is under it, and opens and closes Task View, so run it
// on a PC you're watching. Taps and clicks press whatever is under the pointer, so they only run
// with --clicks. Without the API, or when Windows won't make the device (CI runners), it says so
// and passes; with --strict, anything that doesn't work fails it.
//   g++ -std=c++17 -Isrc/fork tests/fork/touchpad_win_smoke.cpp -luser32 -o touchpad_win_smoke
//
// To try Apollo's own context, run it as SYSTEM in your session (Sysinternals PsExec):
//   psexec -s -i 1 touchpad_win_smoke.exe --strict

#include "touchpad.h"
#include "touchpad_win_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

using namespace fork_touchpad;
using luztouchpad::contact;
using luztouchpad::frame;

namespace {
  constexpr std::uint16_t k_width = 10000;  // 100 x 65 mm, about a laptop touchpad
  constexpr std::uint16_t k_height = 6500;
  constexpr int k_step_ms = 8;  // a 120 Hz trackpad

  enum class timing_e {
    tick_only,  ///< dwTime set, PerformanceCount zero, as Apollo does
    none,  ///< both zero
    both,  ///< both set
  };

  const char *name(timing_e t) {
    switch (t) {
      case timing_e::tick_only:
        return "dwTime only";
      case timing_e::none:
        return "no time";
      case timing_e::both:
        return "dwTime and PerformanceCount";
    }
    return "?";
  }

  LARGE_INTEGER s_frequency;
  LARGE_INTEGER s_base_counter;
  ULONGLONG s_base_tick;

  std::int64_t now_us() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    std::int64_t elapsed = counter.QuadPart - s_base_counter.QuadPart;
    return elapsed / s_frequency.QuadPart * 1'000'000 + elapsed % s_frequency.QuadPart * 1'000'000 / s_frequency.QuadPart;
  }

  /// One test on its own touchpad, so one test's times can't run backwards into the next's
  struct player_t {
    HSYNTHETICPOINTERDEVICE device = nullptr;
    host_t host;
    timing_e timing = timing_e::tick_only;
    std::uint32_t client_ms = 0;
    int injections = 0;
    int failures = 0;
    DWORD last_error = 0;

    explicit player_t(timing_e t = timing_e::tick_only):
        timing(t) {
      auto params = win::touchpad_params(k_width, k_height);
      device = win::api().create(&params);
      host.set_device_size(k_width, k_height);
    }

    ~player_t() {
      if (device) {
        win::api().destroy(device);
      }
    }

    player_t(const player_t &) = delete;
    player_t &operator=(const player_t &) = delete;

    void inject(const injection_t &i) {
      if (i.count == 0) {
        return;
      }
      DWORD time = 0;
      UINT64 counter = 0;
      if (timing != timing_e::none) {
        time = (DWORD) (s_base_tick + (ULONGLONG) (i.time_us / 1000));
      }
      if (timing == timing_e::both) {
        counter = (UINT64) (s_base_counter.QuadPart + i.time_us * s_frequency.QuadPart / 1'000'000);
      }
      POINTER_TYPE_INFO info[luztouchpad::k_max_contacts];
      for (std::size_t n = 0; n < i.count; n++) {
        const auto &e = i.entries[n];
        info[n] = win::touchpad_contact(e.pointer_id, e.flags, (int) e.button_change, e.x, e.y, time, counter);
      }
      injections++;
      if (!win::api().inject(device, info, (UINT32) i.count)) {
        failures++;
        last_error = GetLastError();
      }
      host.injected(now_us());
    }

    /// Sends a frame of fingers at 0-65535 positions, then waits one trackpad step
    void send(std::initializer_list<contact> contacts, std::uint8_t buttons = 0) {
      frame f;
      f.width = k_width;
      f.height = k_height;
      f.time_ms = client_ms;
      f.buttons = buttons;
      for (auto &c : contacts) {
        f.contacts[f.count++] = c;
      }
      inject(host.apply(f, now_us()));
      client_ms += k_step_ms;
      std::this_thread::sleep_for(std::chrono::milliseconds(k_step_ms));
    }

    void pause(int ms) {
      client_ms += ms;
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
  };

  std::uint16_t lerp(int from, int to, int step, int steps) {
    return (std::uint16_t) (from + (to - from) * step / steps);
  }

  /**
   * Puts the pointer in the middle of the main screen before a measured move. Every move goes
   * right, so without this the pointer ends up against the screen's edge and stops counting.
   * The pause also lets the clock pass the previous test's last time: a test's first injection
   * failed with error 87 right after the "no time" test, whose times Windows set itself.
   */
  void center_pointer() {
    SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  /// One finger across the middle of the pad. Returns how far the pointer moved, in pixels.
  long one_finger_move(player_t &p, int steps = 25) {
    POINT before {};
    GetCursorPos(&before);
    p.send({{0, luztouchpad::phase_down, 20000, 32768}});
    for (int s = 1; s <= steps; s++) {
      p.send({{0, luztouchpad::phase_move, lerp(20000, 45000, s, steps), 32768}});
    }
    p.send({{0, luztouchpad::phase_up, 45000, 32768}});
    p.pause(300);
    POINT after {};
    GetCursorPos(&after);
    return std::labs(after.x - before.x) + std::labs(after.y - before.y);
  }

  /// Two fingers moving up the pad, lifted while moving, so Windows scrolls on with inertia
  void two_finger_scroll(player_t &p) {
    p.send({{0, luztouchpad::phase_down, 26000, 50000}, {1, luztouchpad::phase_down, 39000, 50000}});
    for (int s = 1; s <= 20; s++) {
      std::uint16_t y = lerp(50000, 20000, s, 20);
      p.send({{0, luztouchpad::phase_move, 26000, y}, {1, luztouchpad::phase_move, 39000, y}});
    }
    p.send({{0, luztouchpad::phase_up, 26000, 20000}, {1, luztouchpad::phase_up, 39000, 20000}});
    p.pause(1000);
  }

  /// Three fingers up (Task View), then down again (closes it)
  void three_finger_swipes(player_t &p) {
    for (int direction : {-1, 1}) {
      int from = direction < 0 ? 52000 : 15000;
      int to = direction < 0 ? 15000 : 52000;
      p.send({{0, luztouchpad::phase_down, 20000, (std::uint16_t) from}, {1, luztouchpad::phase_down, 32768, (std::uint16_t) from}, {2, luztouchpad::phase_down, 45000, (std::uint16_t) from}});
      for (int s = 1; s <= 15; s++) {
        std::uint16_t y = lerp(from, to, s, 15);
        p.send({{0, luztouchpad::phase_move, 20000, y}, {1, luztouchpad::phase_move, 32768, y}, {2, luztouchpad::phase_move, 45000, y}});
      }
      p.send({{0, luztouchpad::phase_up, 20000, (std::uint16_t) to}, {1, luztouchpad::phase_up, 32768, (std::uint16_t) to}, {2, luztouchpad::phase_up, 45000, (std::uint16_t) to}});
      p.pause(1500);
    }
  }

  /**
   * A finger held still, then moved. Returns how far the move after the hold took the pointer.
   * With keepalive, the still finger is sent again every 100 ms during the hold, as LuzLunar does.
   */
  long held_then_moved(player_t &p, int hold_ms, bool keepalive) {
    p.send({{0, luztouchpad::phase_down, 32768, 32768}});
    if (keepalive) {
      for (int held = 0; held < hold_ms; held += static_cast<int>(luztouchpad::k_keepalive_ms)) {
        p.pause(static_cast<int>(luztouchpad::k_keepalive_ms) - k_step_ms);
        p.send({{0, luztouchpad::phase_move, 32768, 32768}});
      }
    } else {
      p.pause(hold_ms);
    }
    POINT before {};
    GetCursorPos(&before);
    for (int s = 1; s <= 15; s++) {
      p.send({{0, luztouchpad::phase_move, lerp(32768, 50000, s, 15), 32768}});
    }
    p.send({{0, luztouchpad::phase_up, 50000, 32768}});
    p.pause(300);
    POINT after {};
    GetCursorPos(&after);
    return std::labs(after.x - before.x) + std::labs(after.y - before.y);
  }

  void tap(player_t &p) {
    p.send({{0, luztouchpad::phase_down, 32768, 32768}});
    p.pause(40);
    p.send({{0, luztouchpad::phase_up, 32768, 32768}});
    p.pause(500);
  }

  void click(player_t &p) {
    p.send({{0, luztouchpad::phase_down, 32768, 45000}});
    p.send({{0, luztouchpad::phase_move, 32768, 45000}}, luztouchpad::button_pressed);
    p.pause(60);
    p.send({{0, luztouchpad::phase_move, 32768, 45000}});
    p.send({{0, luztouchpad::phase_up, 32768, 45000}});
    p.pause(500);
  }
}  // namespace

int main(int argc, char **argv) {
  bool strict = false;
  bool clicks = false;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--strict") == 0) {
      strict = true;
    } else if (std::strcmp(argv[i], "--clicks") == 0) {
      clicks = true;
    }
  }

  QueryPerformanceFrequency(&s_frequency);
  QueryPerformanceCounter(&s_base_counter);
  s_base_tick = GetTickCount64();

  auto &api = win::api();
  if (!api) {
    std::printf("touchpad_win_smoke: skipped, this Windows has no CreateSyntheticPointerDevice2\n");
    return strict ? 1 : 0;
  }

  {
    auto params = win::touchpad_params(k_width, k_height);
    HSYNTHETICPOINTERDEVICE device = api.create(&params);
    if (!device) {
      std::printf("touchpad_win_smoke: skipped, Windows wouldn't make a synthetic touchpad (error %lu)\n", GetLastError());
      return strict ? 1 : 0;
    }
    api.destroy(device);
  }
  std::printf("Windows makes %.0f x %.0f mm synthetic touchpads; each test below gets its own\n", k_width / 100.0, k_height / 100.0);

  int problems = 0;
  auto report = [&](const char *what, const player_t &p, long moved = -1) {
    std::printf("  %-40s %d injections, %d failed", what, p.injections, p.failures);
    if (p.failures > 0) {
      std::printf(" (last error %lu)", p.last_error);
    }
    if (moved >= 0) {
      std::printf(", pointer moved %ld px", moved);
    }
    std::printf("\n");
  };

  std::printf("One-finger moves, by timing:\n");
  for (timing_e t : {timing_e::tick_only, timing_e::none, timing_e::both}) {
    center_pointer();
    player_t p(t);
    long moved = one_finger_move(p);
    report(name(t), p, moved);
    if (t == timing_e::tick_only && (p.failures > 0 || moved == 0)) {
      problems++;
    }
  }

  std::printf("Held fingers, then moved:\n");
  for (bool keepalive : {false, true}) {
    for (int hold : {500, 1200, 2500}) {
      center_pointer();
      player_t p;
      long moved = held_then_moved(p, hold, keepalive);
      char what[64];
      std::snprintf(what, sizeof(what), "held %d ms, %s", hold, keepalive ? "sent every 100 ms" : "not sent again");
      report(what, p, moved);
      // LuzLunar sends a held finger every 100 ms, so that must keep it moving the pointer
      if (keepalive && (p.failures > 0 || moved == 0)) {
        problems++;
      }
    }
  }

  {
    center_pointer();
    player_t p;
    std::printf("Gestures (watch the screen):\n");
    two_finger_scroll(p);
    report("two-finger scroll with inertia", p);
    three_finger_swipes(p);
    report("three-finger swipes up and down", p);
    if (p.failures > 0) {
      problems++;
    }

    if (clicks) {
      tap(p);
      click(p);
      report("tap and click", p);
    }
  }

  if (problems > 0) {
    std::printf("touchpad_win_smoke: %d problem(s)\n", problems);
    return strict ? 1 : 0;
  }
  std::printf("touchpad_win_smoke: done\n");
  return 0;
}
