/**
 * @file src/fork/debug_log_policy.h
 * @brief Apollo's log for LuzLunar's Mac, apart from any I/O: log level names, the requests
 * `/actions/log` takes, and which of the log's lines a request gets.
 *
 * GET /actions/log?lines=N&grep=TEXT&from=OFFSET gives the last N lines of this run's log (those
 * containing TEXT, ignoring case), or with `from` the lines written since that byte offset, as the
 * reply's `next` said. POST {"level": "debug", "minutes": 30} raises or lowers the level for a while,
 * {"level": "configured"} goes back now, and {"mark": "text"} writes a line to find later.
 *
 * Header-only, without Boost, JSON or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace debug_log_policy {
  /// The levels the min_log_level option takes, in its order (0 = verbose)
  inline constexpr std::array<std::string_view, 7> level_names {"verbose", "debug", "info", "warning", "error", "fatal", "none"};

  /// Lines a GET returns when it doesn't say, and at most
  constexpr std::size_t default_lines = 200;
  constexpr std::size_t max_lines = 5000;

  /// How much of the log a GET reads: the end for a tail, more when searching, and per follow-up
  constexpr std::uint64_t tail_window = 4ull << 20;
  constexpr std::uint64_t grep_window = 32ull << 20;
  constexpr std::uint64_t follow_window = 4ull << 20;

  /// A changed level lasts this long unless the request says, and at most
  constexpr auto default_duration = std::chrono::minutes(30);
  constexpr auto max_duration = std::chrono::hours(4);

  /// A mark's text, at most
  constexpr std::size_t max_mark = 200;

  inline std::optional<int> parse_level(std::string_view name) {
    for (std::size_t i = 0; i < level_names.size(); i++) {
      if (name == level_names[i]) {
        return static_cast<int>(i);
      }
    }
    return std::nullopt;
  }

  inline std::string_view level_name(int level) {
    return level >= 0 && level < static_cast<int>(level_names.size()) ? level_names[level] : "unknown";
  }

  struct get_t {
    std::size_t lines = default_lines;
    std::string grep;  ///< Only lines containing it, ignoring case; empty for all
    std::optional<std::uint64_t> from;  ///< Only lines after this byte offset (a reply's `next`)
  };

  namespace detail {
    inline std::optional<std::uint64_t> number(std::string_view text) {
      if (text.empty() || text.size() > 19) {
        return std::nullopt;
      }
      std::uint64_t value = 0;
      for (char c : text) {
        if (c < '0' || c > '9') {
          return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
      }
      return value;
    }

    inline char lower(char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /// Whether `text` contains `lowered` (already lower case), ignoring ASCII case
    inline bool contains(std::string_view text, std::string_view lowered) {
      if (lowered.empty()) {
        return true;
      }
      if (text.size() < lowered.size()) {
        return false;
      }
      for (std::size_t i = 0; i + lowered.size() <= text.size(); i++) {
        std::size_t j = 0;
        while (j < lowered.size() && lower(text[i + j]) == lowered[j]) {
          j++;
        }
        if (j == lowered.size()) {
          return true;
        }
      }
      return false;
    }
  }  // namespace detail

  /// The query's `lines`, `grep` and `from` (empty when absent); nullopt and `error` when bad
  inline std::optional<get_t> parse_get(std::string_view lines, std::string_view grep, std::string_view from, std::string &error) {
    get_t out;
    if (!lines.empty()) {
      auto value = detail::number(lines);
      if (!value || *value == 0) {
        error = "lines must be a positive number";
        return std::nullopt;
      }
      out.lines = static_cast<std::size_t>(std::min<std::uint64_t>(*value, max_lines));
    }
    if (grep.size() > 200) {
      error = "grep is too long";
      return std::nullopt;
    }
    for (char c : grep) {
      out.grep.push_back(detail::lower(c));
    }
    if (!from.empty()) {
      out.from = detail::number(from);
      if (!out.from) {
        error = "from must be a byte offset";
        return std::nullopt;
      }
    }
    return out;
  }

  /// Where in a log of `size` bytes to start reading for the request
  inline std::uint64_t read_start(const get_t &request, std::uint64_t size) {
    if (request.from) {
      // Past the end: Apollo restarted and began a new log, so all of it is new
      return *request.from <= size ? *request.from : 0;
    }
    auto window = request.grep.empty() ? tail_window : grep_window;
    return size > window ? size - window : 0;
  }

  /// How much to read from `start` in a log of `size` bytes
  inline std::uint64_t read_length(const get_t &request, std::uint64_t start, std::uint64_t size) {
    auto length = size - std::min(start, size);
    return request.from ? std::min(length, follow_window) : length;
  }

  struct lines_t {
    std::vector<std::string> lines;
    std::uint64_t next = 0;  ///< The byte offset after the last whole line read, for a follow-up
    bool restarted = false;  ///< `from` was past the end: a new log
  };

  /**
   * The request's lines from `text`, the log's bytes from offset `start`. Only whole lines: a line
   * cut by the start of the window is skipped (unless the window starts the log or at `from`), and
   * one still being written is left for the next request.
   */
  inline lines_t pick(const get_t &request, std::string_view text, std::uint64_t start, std::uint64_t size) {
    lines_t out;
    out.restarted = request.from && *request.from > size;
    std::size_t at = 0;
    if (start > 0 && !(request.from && start == *request.from)) {
      auto newline = text.find('\n');
      at = newline == std::string_view::npos ? text.size() : newline + 1;
    }
    out.next = start + at;

    std::deque<std::string_view> kept;
    while (at < text.size()) {
      auto newline = text.find('\n', at);
      if (newline == std::string_view::npos) {
        break;
      }
      auto line = text.substr(at, newline - at);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      bool wanted = detail::contains(line, request.grep);
      // A follow-up takes the first lines, and the rest come with the next one, from here
      if (wanted && request.from && kept.size() == request.lines) {
        break;
      }
      at = newline + 1;
      out.next = start + at;
      if (!wanted) {
        continue;
      }
      kept.push_back(line);
      if (kept.size() > request.lines) {
        kept.pop_front();  // a tail keeps the last lines
      }
    }
    for (auto line : kept) {
      out.lines.emplace_back(line);
    }
    return out;
  }

  /// A mark's text as it goes in the log: one line, control characters as spaces, cut to max_mark
  /// bytes without splitting a UTF-8 character
  inline std::string clean_mark(std::string_view text) {
    std::string out;
    for (char c : text) {
      out.push_back(static_cast<unsigned char>(c) < 0x20 || c == 0x7F ? ' ' : c);
    }
    if (out.size() > max_mark) {
      std::size_t end = max_mark;
      while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80) {
        end--;
      }
      out.resize(end);
    }
    return out;
  }
}  // namespace debug_log_policy
