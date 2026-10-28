// Tests for src/fork/vdisplay_primary_policy.h: which display the Virtual Display app's extended
// desktop makes Windows' primary one, and when it tries again after Windows didn't take the change
// (a locked session). A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/vdisplay_primary_policy_test.cpp

#include "fork/vdisplay_primary_policy.h"

#include <cstdio>
#include <cstring>

using namespace vdisplay_primary_policy;
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

// The PC's monitor, another of its monitors and the client's virtual display, as display_device ids
static const std::string pc = "{pc-monitor}";
static const std::string other = "{pc-monitor-2}";
static const std::string mac = "{luzvda-mac}";

static void test_requested() {
  // LuzLunar's ClientDisplay::primaryArg() sends this name; its test checks the same string
  CHECK(std::strcmp(k_arg, "luzPrimary") == 0);

  CHECK(requested("1"));
  CHECK(!requested("0"));
  CHECK(!requested("true"));
  CHECK(!requested(""));
  CHECK(!requested("11"));
}

static void test_target_client_primary() {
  // The client asks: its screen, unless it's primary already
  CHECK(target(true, pc, mac, pc) == mac);
  CHECK(target(true, pc, mac, other) == mac);
  CHECK(!target(true, pc, mac, mac));

  // A resume knows no PC primary; the same
  CHECK(target(true, "", mac, pc) == mac);

  // The primary display couldn't be read: tried, as when isPrimary() fails
  CHECK(target(true, pc, mac, "") == mac);

  // display_device doesn't list the virtual display: nothing to make primary now
  CHECK(!target(true, pc, "", pc));
}

static void test_target_pc_primary() {
  // The client doesn't ask: the PC's primary display again if the arrival changed it
  CHECK(target(false, pc, mac, mac) == pc);
  CHECK(target(false, pc, mac, other) == pc);
  CHECK(target(false, pc, mac, "") == pc);
  CHECK(target(false, pc, "", other) == pc);

  // Still primary, or not known (a resume, which doesn't hand it back)
  CHECK(!target(false, pc, mac, pc));
  CHECK(!target(false, "", mac, mac));

  // The virtual display was primary before it arrived (left from an earlier stream): left alone
  CHECK(!target(false, mac, mac, other));
}

static void test_retry_locked() {
  retry_t retry;
  CHECK(!retry.armed());
  CHECK(!retry.due(t0));
  CHECK(retry.step(t0, false, mac, pc) == retry_e::none);

  // Launched on a locked PC: Windows didn't take the change, and the PC's monitor stayed primary
  retry.arm(t0, pc);
  CHECK(retry.armed());
  CHECK(retry.displaced() == pc);

  // Looks once a second
  CHECK(!retry.due(t0 + 500ms));
  CHECK(retry.due(t0 + 1s));

  // However long the session stays locked, tries don't count
  for (auto t = 1s; t < 10min; t += 1s) {
    CHECK(retry.step(t0 + t, true, mac, "") == retry_e::wait);
  }
  CHECK(retry.tries() == 0);
  CHECK(retry.armed());

  // Signed in at 10 min: 1.5 s for the desktop, then the first try
  auto in = t0 + 10min;
  CHECK(retry.step(in, false, mac, pc) == retry_e::wait);
  CHECK(retry.step(in + 1s, false, mac, pc) == retry_e::wait);
  CHECK(retry.step(in + 2s, false, mac, pc) == retry_e::apply);
  CHECK(retry.tries() == 1);

  // It took
  CHECK(retry.step(in + 3s, false, mac, mac) == retry_e::done);
  CHECK(!retry.armed());
  CHECK(!retry.due(in + 10s));
  CHECK(retry.step(in + 4s, false, mac, pc) == retry_e::none);
}

static void test_retry_grace_restarts() {
  retry_t retry;
  retry.arm(t0, pc);

  // Unlocked, then locked again before the grace ran out: the grace starts again
  CHECK(retry.step(t0 + 1s, false, mac, pc) == retry_e::wait);
  CHECK(retry.step(t0 + 2s, true, mac, "") == retry_e::wait);
  CHECK(retry.step(t0 + 3s, false, mac, pc) == retry_e::wait);
  CHECK(retry.step(t0 + 4s, false, mac, pc) == retry_e::wait);
  CHECK(retry.step(t0 + 5s, false, mac, pc) == retry_e::apply);
}

static void test_retry_give_up() {
  retry_t retry;
  retry.arm(t0, pc);

  // Windows takes changes, but the display never becomes primary: 5 tries, 2 s apart, then no more
  unsigned applies = 0;
  bool gave_up = false;
  clock::duration last_apply {};
  for (auto t = 1s; t < 60s; t += 1s) {
    auto r = retry.step(t0 + t, false, mac, pc);
    if (r == retry_e::apply) {
      if (applies > 0) {
        CHECK(t - last_apply >= 2s);
      }
      last_apply = t;
      applies++;
    } else if (r == retry_e::give_up) {
      gave_up = true;
      CHECK(t - last_apply >= 2s);
      break;
    } else {
      CHECK(r == retry_e::wait);
    }
  }
  CHECK(applies == 5);
  CHECK(gave_up);
  CHECK(!retry.armed());
}

static void test_retry_user_chose() {
  retry_t retry;
  retry.arm(t0, pc);
  CHECK(retry.step(t0 + 1s, true, mac, "") == retry_e::wait);

  // Someone at the PC made another monitor primary: theirs stays
  CHECK(retry.step(t0 + 2s, false, mac, other) == retry_e::user_chose);
  CHECK(!retry.armed());

  // The primary display couldn't be read when it was armed: not taken as someone's choice
  retry_t unknown;
  unknown.arm(t0, "");
  CHECK(unknown.step(t0 + 1s, false, mac, other) == retry_e::wait);
  CHECK(unknown.step(t0 + 3s, false, mac, other) == retry_e::apply);
}

static void test_retry_unknowns() {
  retry_t retry;
  retry.arm(t0, pc);

  // display_device doesn't list the virtual display yet, or the primary couldn't be read: wait
  for (auto t = 1s; t < 30s; t += 1s) {
    CHECK(retry.step(t0 + t, false, "", pc) == retry_e::wait);
    CHECK(retry.step(t0 + t, false, mac, "") == retry_e::wait);
  }
  CHECK(retry.tries() == 0);

  // Disarmed (the client's resume no longer asks, or the app quit)
  retry.disarm();
  CHECK(!retry.armed());
  CHECK(retry.displaced().empty());
  CHECK(retry.step(t0 + 31s, false, mac, pc) == retry_e::none);

  // Armed again from scratch
  retry.arm(t0 + 40s, pc);
  CHECK(retry.tries() == 0);
  CHECK(!retry.due(t0 + 40s));
  CHECK(retry.due(t0 + 41s));
}

int main() {
  test_requested();
  test_target_client_primary();
  test_target_pc_primary();
  test_retry_locked();
  test_retry_grace_restarts();
  test_retry_give_up();
  test_retry_user_chose();
  test_retry_unknowns();

  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("vdisplay_primary_policy: all checks passed\n");
  return 0;
}
