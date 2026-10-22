// Tests for src/fork/vdisplay_recovery_policy.h: when the recovery monitor switches a stream's
// virtual display on again, re-creates it, backs off or gives up, and when it restarts capture so
// capture goes back to the display. A standalone program, so CI builds it without the rest of
// Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/vdisplay_recovery_policy_test.cpp

#include "fork/vdisplay_recovery_policy.h"

#include <cstdio>
#include <vector>

using namespace vdisplay_recovery_policy;
using namespace std::chrono_literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static const clock::time_point t0 {};

// Feeds the monitor one look every 250 ms from `from` until `to` (excluded) and returns the
// actions with when they came
struct seen_t {
  clock::duration at;
  action_e action;
};

static std::vector<seen_t> run(monitor_t &monitor, clock::duration from, clock::duration to, presence_e presence, bool streaming = true) {
  std::vector<seen_t> seen;
  for (auto t = from; t < to; t += 250ms) {
    auto action = monitor.step(t0 + t, presence, streaming);
    if (action != action_e::none) {
      seen.push_back({t, action});
    }
  }
  return seen;
}

static void test_settle() {
  // Set up at 0: the launch's own display changes come first, for 6 s
  monitor_t monitor(t0);
  CHECK(run(monitor, 0s, 6s, presence_e::inactive).empty());

  // Then off from 6 s: switched on at 6.75, 7.5 and 8.25 s, re-created at 18 s
  auto seen = run(monitor, 6s, 19s, presence_e::inactive);
  CHECK(seen.size() == 4);
  if (seen.size() == 4) {
    CHECK(seen[0].at == 6750ms && seen[0].action == action_e::reactivate);
    CHECK(seen[1].at == 7500ms && seen[1].action == action_e::reactivate);
    CHECK(seen[2].at == 8250ms && seen[2].action == action_e::reactivate);
    CHECK(seen[3].at == 18s && seen[3].action == action_e::recreate);
  }
  CHECK(monitor.recreations() == 1);

  // A resume set the display up again: quiet again for 6 s, from scratch
  monitor.settle(t0 + 19s);
  CHECK(monitor.recreations() == 0);
  CHECK(run(monitor, 19s, 25s, presence_e::inactive).empty());
}

static void test_not_streaming() {
  monitor_t monitor(t0);

  // Between streams the display is the PC user's business, however long it's off
  CHECK(run(monitor, 6s, 60s, presence_e::inactive, false).empty());
  CHECK(run(monitor, 6s, 60s, presence_e::missing, false).empty());

  // Time off between streams doesn't count once someone streams again
  auto seen = run(monitor, 60s, 61s, presence_e::inactive);
  CHECK(seen.size() == 1);
  if (!seen.empty()) {
    CHECK(seen[0].at == 60750ms && seen[0].action == action_e::reactivate);
  }

  // Unknown (the driver couldn't be asked) does nothing and restarts the clock; the second attempt
  // then comes 1.5 s into the new episode
  CHECK(run(monitor, 61s, 70s, presence_e::unknown).empty());
  seen = run(monitor, 70s, 72s, presence_e::inactive);
  CHECK(seen.size() == 1);
  if (!seen.empty()) {
    CHECK(seen[0].at == 71500ms && seen[0].action == action_e::reactivate);
  }
}

static void test_back_on() {
  monitor_t monitor(t0);

  // Off for a second, switched on once at 6.75 s, on again from 7 s
  auto seen = run(monitor, 6s, 7s, presence_e::inactive);
  CHECK(seen.size() == 1);
  CHECK(run(monitor, 7s, 10s, presence_e::active).empty());
  CHECK(monitor.reactivations() == 1);

  // Off again 3 s later: the attempts so far still count, so the next comes at 1.5 s
  seen = run(monitor, 10s, 12s, presence_e::inactive);
  CHECK(seen.size() == 1);
  if (!seen.empty()) {
    CHECK(seen[0].at == 11500ms);
  }
  CHECK(monitor.reactivations() == 2);

  // On for 10 s: forgotten, so a later episode gets all three attempts again
  CHECK(run(monitor, 12s, 23s, presence_e::active).empty());
  CHECK(monitor.reactivations() == 0);
  seen = run(monitor, 23s, 26s, presence_e::inactive);
  CHECK(seen.size() == 3);
}

static void test_missing() {
  monitor_t monitor(t0);

  // Gone from the driver: re-created after half a second, then left alone for 3 s
  auto seen = run(monitor, 6s, 9s, presence_e::missing);
  CHECK(seen.size() == 1);
  if (!seen.empty()) {
    CHECK(seen[0].at == 6500ms && seen[0].action == action_e::recreate);
  }

  // Still gone after the quiet: re-created again half a second later
  seen = run(monitor, 9s, 11s, presence_e::missing);
  CHECK(seen.size() == 1);
  if (!seen.empty()) {
    CHECK(seen[0].at == 10s && seen[0].action == action_e::recreate);
  }

  // Off rather than gone: the clock starts again, with switching on first
  seen = run(monitor, 13s, 14s, presence_e::inactive);
  CHECK(seen.size() == 1);
  if (!seen.empty()) {
    CHECK(seen[0].action == action_e::reactivate);
  }
}

static void test_back_off() {
  monitor_t monitor(t0);

  // Never back: 3 re-creates, then back-offs of 5, 10, 20, 40 and 60 s, then it gives up
  std::vector<seen_t> seen = run(monitor, 6s, 600s, presence_e::missing);
  std::vector<action_e> actions;
  for (const auto &s : seen) {
    actions.push_back(s.action);
  }
  int recreates = 0;
  int back_offs = 0;
  int give_ups = 0;
  for (auto a : actions) {
    recreates += a == action_e::recreate;
    back_offs += a == action_e::back_off;
    give_ups += a == action_e::give_up;
  }
  CHECK(recreates == 18);  // 3 before each of the 5 back-offs and the last round
  CHECK(back_offs == 5);
  CHECK(give_ups == 1);
  CHECK(monitor.gave_up());
  CHECK(!actions.empty() && actions.back() == action_e::give_up);

  // The waits double up to a minute
  monitor_t timed(t0);
  std::vector<clock::duration> waits;
  for (clock::duration t = 6s; t < 600s; t += 250ms) {
    if (timed.step(t0 + t, presence_e::missing, true) == action_e::back_off) {
      waits.push_back(timed.back_off_for());
    }
  }
  CHECK(waits.size() == 5);
  if (waits.size() == 5) {
    CHECK(waits[0] == 5s);
    CHECK(waits[1] == 10s);
    CHECK(waits[2] == 20s);
    CHECK(waits[3] == 40s);
    CHECK(waits[4] == 60s);
  }

  // Given up, it stays quiet until the display is set up again
  CHECK(run(monitor, 600s, 700s, presence_e::missing).empty());
  monitor.settle(t0 + 700s);
  CHECK(!monitor.gave_up());
  seen = run(monitor, 706s, 707s, presence_e::missing);
  CHECK(seen.size() == 1);
}

static void test_capture() {
  capture_follow_t follow;

  // On the display: nothing to do
  CHECK(follow.step(t0, true, true) == capture_e::fine);

  // Not on it: a second for capture's own re-initialization, then a restart
  CHECK(follow.step(t0 + 0ms, true, false) == capture_e::waiting);
  CHECK(follow.step(t0 + 500ms, true, false) == capture_e::waiting);
  CHECK(follow.step(t0 + 1s, true, false) == capture_e::restart);
  CHECK(follow.restarts() == 1);

  // Back on it: confirmed, once
  CHECK(follow.step(t0 + 2s, true, true) == capture_e::confirmed);
  CHECK(follow.step(t0 + 3s, true, true) == capture_e::fine);

  // Never back: restarts at 1, 6 and 11 s, then stalled and quiet
  CHECK(follow.step(t0 + 10s, true, false) == capture_e::waiting);
  CHECK(follow.step(t0 + 11s, true, false) == capture_e::restart);
  CHECK(follow.step(t0 + 15s, true, false) == capture_e::waiting);
  CHECK(follow.step(t0 + 16s, true, false) == capture_e::restart);
  CHECK(follow.step(t0 + 21s, true, false) == capture_e::restart);
  CHECK(follow.step(t0 + 26s, true, false) == capture_e::stalled);
  CHECK(follow.step(t0 + 40s, true, false) == capture_e::waiting);
  CHECK(follow.step(t0 + 90s, true, false) == capture_e::waiting);

  // Something changed (the display went off, or capture should be elsewhere): from scratch
  CHECK(follow.step(t0 + 91s, false, false) == capture_e::fine);
  CHECK(follow.step(t0 + 92s, true, false) == capture_e::waiting);
  CHECK(follow.step(t0 + 93s, true, false) == capture_e::restart);

  // Capture on another display picked with the shortcut isn't followed
  capture_follow_t chosen;
  for (clock::duration t = 0s; t < 30s; t += 1s) {
    CHECK(chosen.step(t0 + t, false, false) == capture_e::fine);
  }
}

int main() {
  test_settle();
  test_not_streaming();
  test_back_on();
  test_missing();
  test_back_off();
  test_capture();

  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("vdisplay_recovery_policy: all checks passed\n");
  return 0;
}
