// Tests for src/fork/extend_layout.h: the displays the Virtual Display app streams with. A
// standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/extend_layout_test.cpp

#include "fork/extend_layout.h"

#include <cstdio>

using namespace extend_layout;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void test_same() {
  CHECK(same({{"a"}, {"b"}}, {{"b"}, {"a"}}));
  CHECK(same({{"a", "b"}}, {{"b", "a"}}));
  CHECK(!same({{"a", "b"}}, {{"a"}, {"b"}}));
  CHECK(!same({{"a"}}, {{"a"}, {"b"}}));
  CHECK(same({}, {}));
}

static void test_already_extended() {
  // Windows put the virtual display next to the PC's two monitors itself
  CHECK(!extended({{"m1"}, {"m2"}}, {{"m1"}, {"m2"}, {"v"}}, "v"));
  CHECK(!extended({{"m1"}, {"m2"}}, {{"v"}, {"m2"}, {"m1"}}, "v"));
}

static void test_monitors_switched_off() {
  // The layout saved from an earlier stream left only the virtual display on
  auto want = extended({{"m1"}, {"m2"}}, {{"v"}}, "v");
  CHECK(want && same(*want, {{"m1"}, {"m2"}, {"v"}}));

  // One of the two
  want = extended({{"m1"}, {"m2"}}, {{"m2"}, {"v"}}, "v");
  CHECK(want && same(*want, {{"m1"}, {"m2"}, {"v"}}));
}

static void test_mirrored() {
  // The virtual display mirrors a monitor: it gets a group of its own
  auto want = extended({{"m1"}, {"m2"}}, {{"m1", "v"}, {"m2"}}, "v");
  CHECK(want && same(*want, {{"m1"}, {"m2"}, {"v"}}));

  // The PC's own mirrored monitors stay mirrored
  want = extended({{"m1", "m2"}}, {{"v"}}, "v");
  CHECK(want && same(*want, {{"m1", "m2"}, {"v"}}));
  CHECK(!extended({{"m1", "m2"}}, {{"m2", "m1"}, {"v"}}, "v"));
}

static void test_never_switches_off() {
  // A display that came on during the launch stays on
  auto want = extended({{"m1"}}, {{"v"}, {"m3"}}, "v");
  CHECK(want && same(*want, {{"m1"}, {"m3"}, {"v"}}));

  // The PC's monitors were off before: nothing to bring back
  CHECK(!extended({}, {{"v"}}, "v"));
  CHECK(!extended({{"m1"}}, {{"m1"}, {"v"}, {"m2"}}, "v"));
}

static void test_virtual_in_before() {
  // A re-created display whose old path Windows still listed
  CHECK(!extended({{"m1"}, {"v"}}, {{"m1"}, {"v"}}, "v"));
  auto want = extended({{"m1", "v"}}, {{"v"}}, "v");
  CHECK(want && same(*want, {{"m1"}, {"v"}}));
}

static void test_unknown_virtual_id() {
  // display_device didn't list the virtual display: whatever is on stays on, the PC's displays
  // come back
  auto want = extended({{"m1"}, {"m2"}}, {{"v"}}, "");
  CHECK(want && same(*want, {{"m1"}, {"m2"}, {"v"}}));
  CHECK(!extended({{"m1"}}, {{"m1"}, {"v"}}, ""));
}

static void test_describe() {
  CHECK(describe({}) == "[]");
  CHECK(describe({{"a"}}) == "[a]");
  CHECK(describe({{"a", "b"}, {"c"}}) == "[a, b] [c]");
}

int main() {
  test_same();
  test_already_extended();
  test_monitors_switched_off();
  test_mirrored();
  test_never_switches_off();
  test_virtual_in_before();
  test_unknown_virtual_id();
  test_describe();

  if (s_Failures) {
    std::printf("%d extend layout checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All extend layout tests passed\n");
  return 0;
}
