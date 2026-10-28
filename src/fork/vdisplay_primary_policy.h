/**
 * @file src/fork/vdisplay_primary_policy.h
 * @brief Which display is Windows' primary one while the Virtual Display app extends the desktop,
 * and when to try again after Windows didn't take the change.
 *
 * Out of the box the PC's primary display stays primary: Apollo notes it before the virtual display
 * arrives and makes it primary again if the arrival changed that. A LuzLunar Mac with "Make this Mac
 * the PC's main display" on sends `luzPrimary=1` with each launch and resume, and the client's
 * screen becomes the primary display instead, at each of them. Windows saves the primary display
 * for each set of displays, so from the second time on the display usually arrives primary already.
 *
 * Windows doesn't take display changes while the session is locked, as when the user signs in
 * through the stream. retry_t then has the recovery monitor (vdisplay_recovery.cpp) try again once
 * it does, and gives up if someone made another display primary in the meantime.
 *
 * Header-only, without Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vdisplay_primary_policy {
  using clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  /// The launch argument; LuzLunar's ClientDisplay::primaryArg() sends it
  inline constexpr const char *k_arg = "luzPrimary";

  /// Whether the argument's value asks for the client's screen to be the primary display: only "1"
  inline bool requested(std::string_view value) {
    return value == "1";
  }

  /**
   * @brief The display to make primary now, or nothing.
   * @param client_primary The client asks for its screen to be the primary display.
   * @param pc_primary The primary display before the virtual display arrived, made primary again
   * when the client doesn't ask; empty when not known, or at a resume, which doesn't put it back.
   * @param virtual_id The virtual display's device id, or empty if display_device doesn't list it.
   * @param primary_now The primary display now, or empty if it couldn't be read.
   */
  inline std::optional<std::string> target(bool client_primary, const std::string &pc_primary, const std::string &virtual_id, const std::string &primary_now) {
    if (client_primary) {
      if (virtual_id.empty() || virtual_id == primary_now) {
        return std::nullopt;
      }
      return virtual_id;
    }
    if (pc_primary.empty() || pc_primary == virtual_id || pc_primary == primary_now) {
      return std::nullopt;
    }
    return pc_primary;
  }

  /// What retry_t says to do
  enum class retry_e {
    none,  ///< Nothing to try again
    wait,  ///< Not now
    apply,  ///< Make the client's screen the primary display now
    done,  ///< It is the primary display; nothing more to do
    user_chose,  ///< Another display than the one it was to replace became primary: someone chose it
    give_up,  ///< Windows didn't take it, every try
  };

  struct retry_timing_t {
    clock::duration check = 1s;  ///< How often Windows is asked while waiting
    clock::duration after_unblock = 1500ms;  ///< Windows takes display changes again (the user signed in): give the desktop this long
    clock::duration interval = 2s;  ///< Between tries
    unsigned max_tries = 5;
  };

  /**
   * @brief Making the client's screen the primary display again once Windows takes display changes.
   * Tries don't count while it doesn't, however long the session stays locked.
   */
  class retry_t {
  public:
    explicit retry_t(retry_timing_t timing = {}):
        _timing(timing) {
    }

    /**
     * @brief The client's screen should be the primary display, but Windows didn't take it.
     * @param displaced The primary display then, or empty if it couldn't be read.
     */
    void arm(clock::time_point now, std::string displaced) {
      _armed = true;
      _displaced = std::move(displaced);
      _last_check = now;
      _unblocked_since.reset();
      _last_try.reset();
      _tries = 0;
    }

    void disarm() {
      _armed = false;
      _displaced.clear();
    }

    bool armed() const {
      return _armed;
    }

    /// Whether to look now: armed, and the last look was long enough ago
    bool due(clock::time_point now) const {
      return _armed && now - _last_check >= _timing.check;
    }

    /**
     * @param blocked Windows doesn't take display changes now (a locked session).
     * @param virtual_id The virtual display's device id, or empty if not known yet.
     * @param primary_now The primary display now, or empty if not read (it isn't while blocked).
     */
    retry_e step(clock::time_point now, bool blocked, const std::string &virtual_id, const std::string &primary_now) {
      if (!_armed) {
        return retry_e::none;
      }
      _last_check = now;
      if (blocked) {
        _unblocked_since.reset();
        return retry_e::wait;
      }
      if (virtual_id.empty() || primary_now.empty()) {
        return retry_e::wait;
      }
      if (primary_now == virtual_id) {
        disarm();
        return retry_e::done;
      }
      if (!_displaced.empty() && primary_now != _displaced) {
        disarm();
        return retry_e::user_chose;
      }

      if (!_unblocked_since) {
        _unblocked_since = now;
      }
      if (now - *_unblocked_since < _timing.after_unblock || (_last_try && now - *_last_try < _timing.interval)) {
        return retry_e::wait;
      }
      if (_tries >= _timing.max_tries) {
        disarm();
        return retry_e::give_up;
      }
      _tries++;
      _last_try = now;
      return retry_e::apply;
    }

    unsigned tries() const {
      return _tries;
    }

    const std::string &displaced() const {
      return _displaced;
    }

  private:
    retry_timing_t _timing;
    bool _armed = false;
    std::string _displaced;
    clock::time_point _last_check {};
    std::optional<clock::time_point> _unblocked_since;
    std::optional<clock::time_point> _last_try;
    unsigned _tries = 0;
  };
}  // namespace vdisplay_primary_policy
