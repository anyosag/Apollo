// Tests for src/fork/pointer_speed_state.h: the Windows pointer settings LuzLunar's Mac trackpad
// mode holds, and the state file that puts them back after a crash. A standalone program, so CI
// builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/pointer_speed_test.cpp

#include "fork/pointer_speed_state.h"

#include <cstdio>

using namespace fork_tweaks::pointer_speed;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void test_one_to_one() {
  CHECK(one_to_one(k_one_to_one));
  CHECK(valid(k_one_to_one));

  // Windows' defaults accelerate
  values_t defaults;
  CHECK(!one_to_one(defaults));
  CHECK(valid(defaults));

  // Without Enhance pointer precision, only speed 10 is one pixel a count
  CHECK(!one_to_one({0, 0, 0, 12}));
  CHECK(one_to_one({6, 10, 0, 10}));
}

static void test_round_trip() {
  values_t v {6, 10, 1, 14};
  CHECK(format(v) == "6,10,1,14");
  auto back = parse(format(v));
  CHECK(back && *back == v);

  // A line as getline leaves it from a file written on Windows
  auto crlf = parse("0,0,0,10\r");
  CHECK(crlf && *crlf == k_one_to_one);
  CHECK(parse("  6,10,1,10 \n").has_value());
}

static void test_damaged() {
  CHECK(!parse(""));
  CHECK(!parse("6,10,1"));
  CHECK(!parse("6,10,1,10,4"));
  CHECK(!parse("6,10,x,10"));
  CHECK(!parse("6,,1,10"));
  CHECK(!parse("6 ,10,1,10"));
  CHECK(!parse("6,10,1,0"));  // speed is 1-20
  CHECK(!parse("6,10,1,21"));
  CHECK(!parse("6,10,3,10"));
  CHECK(!parse("-1,10,1,10"));
  CHECK(!parse("99999999999,10,1,10"));
}

static void test_describe() {
  CHECK(describe({6, 10, 1, 12}) == "Enhance pointer precision on, speed 12");
  CHECK(describe(k_one_to_one) == "Enhance pointer precision off, speed 10");
}

int main() {
  test_one_to_one();
  test_round_trip();
  test_damaged();
  test_describe();

  if (s_Failures == 0) {
    std::printf("pointer_speed: all tests passed\n");
    return 0;
  }
  std::printf("pointer_speed: %d failure(s)\n", s_Failures);
  return 1;
}
