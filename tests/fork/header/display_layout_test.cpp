// Tests for src/platform/windows/display_layout.h: where the isolated virtual display goes. A
// standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/display_layout_test.cpp

#include "platform/windows/display_layout.h"

#include <cstdio>
#include <random>
#include <vector>

using namespace display_layout;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static bool at(const rect_t &r, int x, int y) {
  return r.x == x && r.y == y;
}

static bool overlap(const rect_t &a, const rect_t &b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static bool touch(const rect_t &a, const rect_t &b) {
  auto m = move_to_touch(corners(a), corners(b));
  return m.x == 0 && m.y == 0;
}


static void test_move_to_touch() {
  rect_t a {0, 0, 100, 100};

  // Side by side, stacked, at a corner, and overlapping spans on a shared edge
  CHECK(touch({100, 0, 100, 100}, a));
  CHECK(touch({0, 100, 100, 100}, a));
  CHECK(touch({100, 100, 50, 50}, a));
  CHECK(touch({100, 50, 100, 100}, a));
  CHECK(touch({-30, 100, 50, 50}, a));

  // Same line but a gap along it
  auto m = move_to_touch(corners({0, 200, 100, 100}), corners(a));
  CHECK(m.x == 0 && m.y == -100);

  // Away along x, and along both
  m = move_to_touch(corners({300, 0, 100, 100}), corners(a));
  CHECK(m.x == -200 && m.y == 0);
  m = move_to_touch(corners({300, 400, 100, 100}), corners(a));
  CHECK(m.x == -200);
  CHECK(touch({300 + m.x, 400 + m.y, 100, 100}, a));
}

static void test_only_virtual() {
  auto r = isolated_layout({{0, 0, 1920, 1080}});
  CHECK(r && r->size() == 1 && at((*r)[0], 0, 0));
}

static void test_one_monitor() {
  // Windows put the new display to the right of the primary
  auto r = isolated_layout({{2560, 0, 1920, 1080}, {0, 0, 2560, 1440}});
  CHECK(r.has_value());
  if (r) {
    CHECK(at((*r)[1], 0, 0));
    CHECK(at((*r)[0], 2560, 1440));
  }
}

static void test_two_side_by_side() {
  auto r = isolated_layout({{3840, 0, 1920, 1080}, {0, 0, 1920, 1080}, {1920, 0, 1920, 1080}});
  CHECK(r.has_value());
  if (r) {
    CHECK(at((*r)[1], 0, 0));
    CHECK(at((*r)[2], 1920, 0));
    CHECK(at((*r)[0], 3840, 1080));
  }
}

static void test_left_and_below() {
  // B left of the primary A, C below it
  auto r = isolated_layout({{1920, 0, 1920, 1080}, {0, 0, 1920, 1080}, {-1920, 0, 1920, 1080}, {0, 1080, 1920, 1080}});
  CHECK(r.has_value());
  if (r) {
    CHECK(at((*r)[1], 0, 0));
    CHECK(at((*r)[2], -1920, 0));
    CHECK(at((*r)[3], 0, 1080));
    CHECK(at((*r)[0], 1920, 2160));
  }
}

static void test_bridged_by_virtual() {
  // B only touched A through the virtual display's old place, so it has to move next to A
  auto r = isolated_layout({{1920, 0, 1920, 1080}, {0, 0, 1920, 1080}, {3840, 0, 1920, 1080}});
  CHECK(r.has_value());
  if (r) {
    CHECK(at((*r)[1], 0, 0));
    CHECK(at((*r)[2], 1920, 0));
    CHECK(at((*r)[0], 3840, 1080));
  }
}

static void test_no_primary_at_origin() {
  // Used to add an uninitialized x offset; now nothing shifts
  std::vector<rect_t> before {{3000, 0, 1920, 1080}, {100, 100, 1920, 1080}};
  auto placed = place_isolated(before);
  auto r = isolated_layout(before);
  CHECK(placed && r);
  if (placed && r) {
    CHECK(at((*r)[0], (*placed)[0].x, (*placed)[0].y));
    CHECK(at((*r)[1], (*placed)[1].x, (*placed)[1].y));
  }
}

static void test_bad_index() {
  CHECK(!place_isolated({}, 0));
  CHECK(!place_isolated({{0, 0, 10, 10}}, 1));
}

static void test_valid_layout() {
  CHECK(valid_layout({{0, 0, 100, 100}, {100, 0, 100, 100}}));
  CHECK(!valid_layout({{0, 0, 100, 100}, {50, 0, 100, 100}}));
  CHECK(!valid_layout({{0, 0, 100, 100}, {300, 0, 100, 100}}));
}

// Random desktops built the way Windows keeps them, each display touching one placed before it.
// The virtual display must end up touching only at a corner, with nothing overlapping. Where the
// original algorithm already gave a valid layout, the result must be the same.
static void test_random_layouts() {
  std::mt19937 rng(1234);
  const int sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {1280, 1024}, {3024, 1964}, {1080, 1920}};
  int unsettled = 0;
  for (int n = 0; n < 2000; ++n) {
    int count = 1 + rng() % 5;
    std::vector<rect_t> displays;
    for (int i = 0; i < count; ++i) {
      auto &s = sizes[rng() % 6];
      rect_t r {0, 0, s[0], s[1]};
      if (!displays.empty()) {
        // Next to a random earlier display, on a random side, sliding along it
        bool placed = false;
        for (int attempt = 0; attempt < 50 && !placed; ++attempt) {
          const auto &a = displays[rng() % displays.size()];
          switch (rng() % 4) {
            case 0:
              r.x = a.x + a.width;
              r.y = a.y + int(rng() % a.height) - r.height / 2;
              break;
            case 1:
              r.x = a.x - r.width;
              r.y = a.y + int(rng() % a.height) - r.height / 2;
              break;
            case 2:
              r.y = a.y + a.height;
              r.x = a.x + int(rng() % a.width) - r.width / 2;
              break;
            default:
              r.y = a.y - r.height;
              r.x = a.x + int(rng() % a.width) - r.width / 2;
              break;
          }
          placed = true;
          for (const auto &d : displays) {
            if (overlap(d, r)) {
              placed = false;
              break;
            }
          }
        }
        if (!placed) {
          continue;
        }
      }
      displays.push_back(r);
    }

    // Windows' primary is at (0, 0)
    int ox = displays[0].x;
    int oy = displays[0].y;
    for (auto &d : displays) {
      d.x -= ox;
      d.y -= oy;
    }

    // The virtual display, wherever Windows first put it
    std::size_t v = rng() % displays.size();
    auto original = place_isolated(displays, v);
    auto r = isolated_layout(displays, v);
    if (!r) {
      unsettled++;
      continue;
    }
    CHECK(r->size() == displays.size());
    CHECK(valid_layout(*r));
    if (original && valid_layout(*original)) {
      auto offset = keep_primary_offset(displays, *original);
      for (std::size_t i = 0; i < r->size(); ++i) {
        CHECK(at((*r)[i], (*original)[i].x + offset.x, (*original)[i].y + offset.y));
      }
    }

    for (std::size_t i = 0; i < r->size(); ++i) {
      for (std::size_t j = i + 1; j < r->size(); ++j) {
        CHECK(!overlap((*r)[i], (*r)[j]));
      }
      if (i != v) {
        // Nothing sits to the right of, or below, the virtual display's top-left corner
        CHECK((*r)[i].x + (*r)[i].width <= (*r)[v].x || (*r)[i].y + (*r)[i].height <= (*r)[v].y);
      }
    }
  }
  // The rest are left where Windows put them. Seed 1234 gives 4: desktops where the original
  // algorithm overlapped displays and avoiding that leaves one with nowhere to go
  std::printf("random layouts left unchanged: %d of 2000\n", unsettled);
  CHECK(unsettled <= 4);
}

int main() {
  test_move_to_touch();
  test_only_virtual();
  test_one_monitor();
  test_two_side_by_side();
  test_left_and_below();
  test_bridged_by_virtual();
  test_no_primary_at_origin();
  test_bad_index();
  test_valid_layout();
  test_random_layouts();

  if (s_Failures) {
    std::printf("%d display layout checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All display layout tests passed\n");
  return 0;
}
