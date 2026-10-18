/**
 * @file src/login_throttle.h
 * @brief Slows down password guessing against the Web UI, per address.
 *
 * Header-only, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace confighttp {
  /**
   * @brief Tracks failed password attempts per client address. After a few free tries, each
   * further failure doubles the wait before the next attempt is accepted, up to a cap.
   */
  class login_throttle_t {
  public:
    using clock = std::chrono::steady_clock;

    /// Failures allowed before any wait, so a mistyped password costs nothing
    static constexpr int free_failures = 3;
    static constexpr std::chrono::seconds first_delay {2};
    static constexpr std::chrono::seconds max_delay {300};
    /// Addresses tracked at most; the oldest entries go first, so a flood can't grow memory
    static constexpr std::size_t max_entries = 4096;

    /**
     * @brief How long the address must wait before its next attempt; zero if it may try now.
     */
    clock::duration wait_for(const std::string &address, clock::time_point now = clock::now()) {
      std::lock_guard lock {mutex};
      auto it = entries.find(address);
      if (it == entries.end() || now >= it->second.next_allowed) {
        return clock::duration::zero();
      }
      return it->second.next_allowed - now;
    }

    /** @brief Records a failed attempt from the address. */
    void failed(const std::string &address, clock::time_point now = clock::now()) {
      std::lock_guard lock {mutex};
      if (entries.size() >= max_entries && !entries.contains(address)) {
        auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
          return a.second.last_failure < b.second.last_failure;
        });
        entries.erase(oldest);
      }

      auto &entry = entries[address];
      entry.failures++;
      entry.last_failure = now;
      if (entry.failures > free_failures) {
        int doublings = std::min(entry.failures - free_failures - 1, 16);
        auto delay = std::min<clock::duration>(first_delay * (1 << doublings), max_delay);
        entry.next_allowed = now + delay;
      }
    }

    /** @brief Forgets the address's failures after a successful login. */
    void succeeded(const std::string &address) {
      std::lock_guard lock {mutex};
      entries.erase(address);
    }

  private:
    struct entry_t {
      int failures = 0;
      clock::time_point last_failure {};
      clock::time_point next_allowed {};
    };

    std::mutex mutex;
    std::unordered_map<std::string, entry_t> entries;
  };
}  // namespace confighttp
