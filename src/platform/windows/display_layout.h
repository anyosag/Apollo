/**
 * @file src/platform/windows/display_layout.h
 * @brief Where the isolated virtual display goes: below and to the right of every other display,
 * touching them only at a corner, so the mouse rarely wanders onto it.
 *
 * Header-only, without Windows types, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <array>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace display_layout {
  struct point_t {
    int x;
    int y;
  };

  /// A display's place on the desktop, in pixels
  struct rect_t {
    int x;
    int y;
    int width;
    int height;
  };

  using corners_t = std::array<point_t, 4>;

  inline corners_t corners(const rect_t &r) {
    return {{
      {r.x, r.y},
      {r.x + r.width, r.y},
      {r.x, r.y + r.height},
      {r.x + r.width, r.y + r.height},
    }};
  }

  /**
   * @brief The move that makes `unknown` touch `connected`, along an edge or at a corner.
   * @return {0, 0} if they already touch. Otherwise the shortest move along x, followed by the
   * move along y that is still needed after it.
   */
  inline point_t move_to_touch(const corners_t &unknown, const corners_t &connected, bool allow_x = true) {
    struct difference_t {
      point_t left;
      point_t right;
      point_t diff;
      point_t abs;
    };

    std::vector<difference_t> differences;
    for (const auto &c : connected) {
      for (const auto &u : unknown) {
        point_t diff {c.x - u.x, c.y - u.y};
        differences.push_back({c, u, diff, {std::abs(diff.x), std::abs(diff.y)}});
      }
    }

    // Corners within a pixel, or a shared vertical or horizontal line
    std::vector<difference_t> vertical;
    std::vector<difference_t> horizontal;
    for (const auto &d : differences) {
      if (d.abs.x <= 1 && d.abs.y <= 1) {
        return {0, 0};
      }
      if (d.abs.x <= 1) {
        vertical.push_back(d);
      }
      if (d.abs.y <= 1) {
        horizontal.push_back(d);
      }
    }

    // On a shared line, the boxes touch only if their spans overlap: some corners on each side
    auto spans_overlap = [](const std::vector<difference_t> &on_line, bool along_y) {
      int less = 0;
      int greater = 0;
      for (const auto &d : on_line) {
        int l = along_y ? d.left.y : d.left.x;
        int r = along_y ? d.right.y : d.right.x;
        less += l <= r;
        greater += l >= r;
      }
      return less > 0 && greater > 0;
    };
    if (spans_overlap(vertical, true) || spans_overlap(horizontal, false)) {
      return {0, 0};
    }

    if (allow_x) {
      const difference_t *shortest = nullptr;
      for (const auto &d : differences) {
        if (!shortest || d.abs.x < shortest->abs.x) {
          shortest = &d;
        }
      }
      if (shortest && shortest->abs.x > 1) {
        // Move along x, then see what y move is left
        int dx = shortest->diff.x;
        corners_t moved = unknown;
        for (auto &p : moved) {
          p.x += dx;
        }
        return {dx, move_to_touch(moved, connected, false).y};
      }
    }

    const difference_t *shortest = nullptr;
    for (const auto &d : differences) {
      if (!shortest || d.abs.y < shortest->abs.y) {
        shortest = &d;
      }
    }
    if (shortest && shortest->abs.y > 1) {
      return {0, shortest->diff.y};
    }
    return {0, 0};
  }

  inline bool overlaps(const rect_t &a, const rect_t &b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
  }

  /// Whether moving `displays[index]` by `move` would put it on top of another display
  inline bool overlaps_after(const std::vector<rect_t> &displays, std::size_t index, point_t move) {
    rect_t moved = displays[index];
    moved.x += move.x;
    moved.y += move.y;
    for (std::size_t i = 0; i < displays.size(); ++i) {
      if (i != index && overlaps(moved, displays[i])) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Puts `displays[virtual_index]` at (0, 0), puts the bottom-right corner of the rightmost
   * other display against its top-left corner, then moves the others until each touches one that is
   * already placed.
   * @param avoid_overlap Never take a move that puts one display on top of another. Without it,
   * this is the original Apollo algorithm, which can leave displays overlapping.
   * @return The new positions, or nothing if the moves didn't settle or a display couldn't be placed.
   */
  inline std::optional<std::vector<rect_t>> place_isolated(std::vector<rect_t> displays, std::size_t virtual_index = 0, bool avoid_overlap = false) {
    const std::size_t count = displays.size();
    if (virtual_index >= count) {
      return std::nullopt;
    }

    // The rightmost other display; for a tie, the lowest
    int max_x = INT_MIN;
    int max_y = INT_MIN;
    bool found = false;
    for (std::size_t i = 0; i < count; ++i) {
      if (i == virtual_index) {
        continue;
      }
      int right = displays[i].x + displays[i].width;
      int bottom = displays[i].y + displays[i].height;
      if (max_x < right || (max_x == right && max_y < bottom)) {
        max_x = right;
        max_y = bottom;
        found = true;
      }
    }

    if (found) {
      for (std::size_t i = 0; i < count; ++i) {
        if (i != virtual_index) {
          displays[i].x -= max_x;
          displays[i].y -= max_y;
        }
      }
    }
    displays[virtual_index].x = 0;
    displays[virtual_index].y = 0;

    std::vector<bool> connected(count, false);
    connected[virtual_index] = true;

    // Only the first display placed may touch the virtual one
    bool first = true;

    // Each round either places a display without moving it or moves one; give up long before an
    // endless loop could hang the launch
    const std::size_t max_rounds = 4 * count * count + 16;
    for (std::size_t round = 0;; ++round) {
      if (round >= max_rounds) {
        return std::nullopt;
      }

      int best_move = INT_MAX;
      std::size_t best_anchor = 0;
      std::size_t best_display = 0;

      bool added;
      do {
        added = false;
        for (std::size_t anchor = 0; anchor < count && !added; ++anchor) {
          if (!connected[anchor] || (!first && anchor == virtual_index)) {
            continue;
          }
          auto anchor_corners = corners(displays[anchor]);

          for (std::size_t other = 0; other < count; ++other) {
            if (other == anchor || connected[other] || other == virtual_index) {
              continue;
            }

            auto move = move_to_touch(corners(displays[other]), anchor_corners);
            if (move.x == 0 && move.y == 0) {
              connected[other] = true;
              first = false;
              added = true;
              best_move = INT_MAX;
              break;
            }
            if (anchor != virtual_index && std::abs(move.x) < best_move && !(avoid_overlap && overlaps_after(displays, other, move))) {
              best_move = std::abs(move.x);
              best_anchor = anchor;
              best_display = other;
            }
          }
        }
      } while (added);

      first = false;

      if (best_move == INT_MAX) {
        // Done, unless a display was left out because every move for it would overlap another
        for (bool c : connected) {
          if (!c) {
            return std::nullopt;
          }
        }
        return displays;
      }

      auto move = move_to_touch(corners(displays[best_display]), corners(displays[best_anchor]));
      displays[best_display].x += move.x;
      displays[best_display].y += move.y;
    }
  }

  /**
   * @brief The offset that keeps the display that was at (0, 0), the primary, at (0, 0).
   * @return {0, 0} if no display was at (0, 0).
   */
  inline point_t keep_primary_offset(const std::vector<rect_t> &before, const std::vector<rect_t> &after) {
    for (std::size_t i = 0; i < before.size() && i < after.size(); ++i) {
      if (before[i].x == 0 && before[i].y == 0) {
        return {-after[i].x, -after[i].y};
      }
    }
    return {0, 0};
  }

  /// No two displays overlap, and each touches another, so the desktop is one piece
  inline bool valid_layout(const std::vector<rect_t> &displays) {
    for (std::size_t i = 0; i < displays.size(); ++i) {
      for (std::size_t j = i + 1; j < displays.size(); ++j) {
        if (overlaps(displays[i], displays[j])) {
          return false;
        }
      }
    }

    std::vector<bool> seen(displays.size(), false);
    std::vector<std::size_t> todo;
    if (!displays.empty()) {
      seen[0] = true;
      todo.push_back(0);
    }
    while (!todo.empty()) {
      auto i = todo.back();
      todo.pop_back();
      for (std::size_t j = 0; j < displays.size(); ++j) {
        auto m = move_to_touch(corners(displays[j]), corners(displays[i]));
        if (!seen[j] && m.x == 0 && m.y == 0) {
          seen[j] = true;
          todo.push_back(j);
        }
      }
    }
    for (bool s : seen) {
      if (!s) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief The isolated layout, shifted so the primary display stays where it is.
   *
   * Uses the original algorithm's result when it is valid, so working setups keep their layout;
   * otherwise the one that avoids overlaps.
   */
  inline std::optional<std::vector<rect_t>> isolated_layout(const std::vector<rect_t> &displays, std::size_t virtual_index = 0) {
    auto placed = place_isolated(displays, virtual_index);
    if (!placed || !valid_layout(*placed)) {
      placed = place_isolated(displays, virtual_index, true);
    }
    if (!placed || !valid_layout(*placed)) {
      return std::nullopt;
    }

    auto offset = keep_primary_offset(displays, *placed);
    for (auto &r : *placed) {
      r.x += offset.x;
      r.y += offset.y;
    }
    return placed;
  }

  inline std::string describe(const std::vector<rect_t> &displays) {
    std::string out;
    for (std::size_t i = 0; i < displays.size(); ++i) {
      const auto &r = displays[i];
      out += "[" + std::to_string(i) + "] " + std::to_string(r.width) + "x" + std::to_string(r.height) +
             " at " + std::to_string(r.x) + "," + std::to_string(r.y) + "\n";
    }
    return out;
  }
}  // namespace display_layout
