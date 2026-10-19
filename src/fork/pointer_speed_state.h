/**
 * @file src/fork/pointer_speed_state.h
 * @brief Fork-only: the Windows pointer settings LuzLunar's Mac trackpad mode holds while it
 * streams (pointer_speed.h), and the state file that puts them back after a crash.
 *
 * Plain C++, so tests/fork/header/pointer_speed_test.cpp builds anywhere.
 */
#pragma once

// standard includes
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace fork_tweaks::pointer_speed {
  /// `SPI_GETMOUSE`'s three values and `SPI_GETMOUSESPEED`'s
  struct values_t {
    int threshold1 = 6;
    int threshold2 = 10;
    int acceleration = 1;  ///< Non-zero: Enhance pointer precision is on
    int speed = 10;  ///< 1-20; at 10 a count moves the pointer a pixel

    bool operator==(const values_t &other) const {
      return threshold1 == other.threshold1 && threshold2 == other.threshold2 && acceleration == other.acceleration && speed == other.speed;
    }
  };

  /// While a Mac trackpad stream runs: Enhance pointer precision off and speed 10, so a move of
  /// n pixels moves the pointer n pixels, as far as macOS meant
  constexpr values_t k_one_to_one {0, 0, 0, 10};

  /// Whether Windows moves the pointer exactly as far as it's told
  inline bool one_to_one(const values_t &v) {
    return v.acceleration == 0 && v.speed == 10;
  }

  /// Values Windows could have: anything else in the state file is a damaged file
  inline bool valid(const values_t &v) {
    return v.threshold1 >= 0 && v.threshold1 <= 1000 && v.threshold2 >= 0 && v.threshold2 <= 1000 && v.acceleration >= 0 &&
           v.acceleration <= 2 && v.speed >= 1 && v.speed <= 20;
  }

  /// The state file's line: "threshold1,threshold2,acceleration,speed"
  inline std::string format(const values_t &v) {
    return std::to_string(v.threshold1) + ',' + std::to_string(v.threshold2) + ',' + std::to_string(v.acceleration) + ',' +
           std::to_string(v.speed);
  }

  /// The values in a state file's line, if it holds four valid ones (whitespace around it ignored)
  inline std::optional<values_t> parse(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
      text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
      text.remove_suffix(1);
    }

    int numbers[4] {};
    const char *at = text.data();
    const char *end = text.data() + text.size();
    for (int i = 0; i < 4; i++) {
      if (i > 0) {
        if (at == end || *at != ',') {
          return std::nullopt;
        }
        at++;
      }
      auto [next, error] = std::from_chars(at, end, numbers[i]);
      if (error != std::errc {} || next == at) {
        return std::nullopt;
      }
      at = next;
    }
    if (at != end) {
      return std::nullopt;
    }

    values_t v {numbers[0], numbers[1], numbers[2], numbers[3]};
    if (!valid(v)) {
      return std::nullopt;
    }
    return v;
  }

  /// For the log: "Enhance pointer precision on, speed 10"
  inline std::string describe(const values_t &v) {
    return std::string(v.acceleration != 0 ? "Enhance pointer precision on" : "Enhance pointer precision off") + ", speed " +
           std::to_string(v.speed);
  }
}  // namespace fork_tweaks::pointer_speed
