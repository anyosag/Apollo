// Tests for src/fork/cursor_position.h: the pointer on the display a stream shows, for LuzLunar's
// GET /actions/cursor. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/cursor_position_test.cpp

#include "fork/cursor_position.h"

#include <cstdio>

using namespace fork_tweaks::cursor_position;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void test_on_display() {
  // No display yet: nothing to say
  CHECK(!on_display({}, 10, 10, true));

  // The streamed display right of a 1920-wide one, as the capture has it
  display_t display {1920, 0, 2560, 1440};
  auto top = on_display(display, 1920 + 400, 0, true);
  CHECK(top && top->x == 400 && top->y == 0 && top->width == 2560 && top->height == 1440 && top->shown);

  // On the other display: outside this one's width
  auto other = on_display(display, 100, 500, true);
  CHECK(other && other->x == -1820 && other->y == 500);

  // A display above this one: the pointer that went up onto it is above the top
  display_t lower {0, 1080, 1920, 1080};
  auto above = on_display(lower, 600, 1000, false);
  CHECK(above && above->y == -80 && !above->shown);
}

static void test_json() {
  auto json = nlohmann::json::parse(position_json({400, 0, 2560, 1440, true}));
  CHECK(json["x"] == 400);
  CHECK(json["y"] == 0);
  CHECK(json["width"] == 2560);
  CHECK(json["height"] == 1440);
  CHECK(json["shown"] == true);
}

int main() {
  test_on_display();
  test_json();

  if (s_Failures == 0) {
    std::printf("cursor_position: all tests passed\n");
    return 0;
  }
  std::printf("cursor_position: %d failure(s)\n", s_Failures);
  return 1;
}
