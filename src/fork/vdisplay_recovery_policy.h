/**
 * @file src/fork/vdisplay_recovery_policy.h
 * @brief When to bring a stream's virtual display back once Windows switched it off or the driver
 * lost it, and when capture has to go back to it.
 *
 * Windows can switch the virtual display off mid-stream: a monitor on the PC wakes up, or Windows
 * applies a layout it saved for that set of displays. Capture then falls back to another display,
 * and the client sees one of the PC's monitors instead of its own screen. The recovery monitor
 * (vdisplay_recovery.cpp) looks at the display a few times a second while someone streams and asks
 * this policy what to do: switch it on again (up to 3 times, 750 ms apart), re-create it after
 * 12 s off or half a second gone from the driver, back off when nothing helps, and restart capture
 * when it isn't on the display it should be.
 *
 * Modelled on Vibepollo's virtual display recovery (inactive_reactivation_budget_t, its monitor's
 * timings and capture_recovery.h's confirmation; github.com/Nonary/Vibepollo, GPLv3).
 *
 * Header-only, without Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <optional>

namespace vdisplay_recovery_policy {
  using clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  /// What the virtual display is now
  enum class presence_e {
    unknown,  ///< The driver or Windows couldn't be asked
    active,  ///< The driver has it and Windows shows it
    inactive,  ///< The driver has it, but Windows switched it off
    missing,  ///< The driver doesn't have it any more
  };

  /// What to do about the display now
  enum class action_e {
    none,
    reactivate,  ///< Switch it on again
    recreate,  ///< Remove it and add it again
    back_off,  ///< Re-creating didn't help: wait (back_off_for()) before trying again
    give_up,  ///< Nothing helped; leave it for the rest of the stream
  };

  struct timing_t {
    clock::duration poll = 250ms;  ///< How often the display is looked at while someone streams
    clock::duration settle = 6s;  ///< After the display is set up or set up again: the launch's own display changes come first
    clock::duration stable = 10s;  ///< On this long: the attempts so far are forgotten
    clock::duration attempt_interval = 750ms;  ///< Off this long, then as long again between attempts: switch it on
    unsigned max_reactivations = 3;
    clock::duration recreate_after = 12s;  ///< Off this long: re-create it
    clock::duration missing_grace = 500ms;  ///< Gone from the driver this long: re-create it
    clock::duration after_recreate = 3s;  ///< After a re-create, before looking at it again
    unsigned max_recreations = 3;  ///< Then back off
    clock::duration backoff = 5s;  ///< The first back-off, doubled each time
    clock::duration max_backoff = 60s;
    unsigned max_backoffs = 5;  ///< Then give up
  };

  /**
   * @brief The decisions for one virtual display, fed what the display is at each look.
   */
  class monitor_t {
  public:
    explicit monitor_t(clock::time_point now, timing_t timing = {}):
        _timing(timing) {
      settle(now);
    }

    /// The display was just set up, or set up again: nothing is tried for a while, from scratch
    void settle(clock::time_point now) {
      _quiet_until = now + _timing.settle;
      _issue_since.reset();
      _active_since.reset();
      _reactivations = 0;
      _recreations = 0;
      _backoffs = 0;
      _gave_up = false;
    }

    /**
     * @param presence What the display is now.
     * @param streaming Whether anyone streams: the display is the PC user's business otherwise.
     */
    action_e step(clock::time_point now, presence_e presence, bool streaming) {
      if (_gave_up) {
        return action_e::none;
      }
      if (!streaming || presence == presence_e::unknown || now < _quiet_until) {
        _issue_since.reset();
        _active_since.reset();
        return action_e::none;
      }

      if (presence == presence_e::active) {
        _issue_since.reset();
        if (!_active_since) {
          _active_since = now;
        } else if (now - *_active_since >= _timing.stable) {
          _reactivations = 0;
          _recreations = 0;
          _backoffs = 0;
        }
        return action_e::none;
      }

      _active_since.reset();
      if (!_issue_since || _issue != presence) {
        _issue_since = now;
        _issue = presence;
        return action_e::none;
      }

      auto off_for = now - *_issue_since;
      if (presence == presence_e::inactive) {
        if (_reactivations < _timing.max_reactivations && off_for >= _timing.attempt_interval * (_reactivations + 1)) {
          _reactivations++;
          return action_e::reactivate;
        }
        if (off_for < _timing.recreate_after) {
          return action_e::none;
        }
      } else if (off_for < _timing.missing_grace) {
        return action_e::none;
      }

      _issue_since.reset();
      if (_recreations >= _timing.max_recreations) {
        if (_backoffs >= _timing.max_backoffs) {
          _gave_up = true;
          return action_e::give_up;
        }
        _back_off_for = std::min<clock::duration>(_timing.backoff * (1u << std::min(_backoffs, 16u)), _timing.max_backoff);
        _backoffs++;
        _reactivations = 0;
        _recreations = 0;
        _quiet_until = now + _back_off_for;
        return action_e::back_off;
      }

      _recreations++;
      _reactivations = 0;
      _quiet_until = now + _timing.after_recreate;
      return action_e::recreate;
    }

    /// How long the last back_off waits
    clock::duration back_off_for() const {
      return _back_off_for;
    }

    unsigned reactivations() const {
      return _reactivations;
    }

    unsigned recreations() const {
      return _recreations;
    }

    bool gave_up() const {
      return _gave_up;
    }

  private:
    timing_t _timing;
    clock::time_point _quiet_until {};
    std::optional<clock::time_point> _issue_since;
    presence_e _issue = presence_e::unknown;
    std::optional<clock::time_point> _active_since;
    unsigned _reactivations = 0;
    unsigned _recreations = 0;
    unsigned _backoffs = 0;
    clock::duration _back_off_for {};
    bool _gave_up = false;
  };

  /// What capture_follow_t says about capture
  enum class capture_e {
    fine,  ///< On the display, or nothing to say
    waiting,  ///< Not on it, and a restart is being given time
    restart,  ///< Restart capture now: it picks the display again
    confirmed,  ///< Back on the display after restarts
    stalled,  ///< Still not on it after the restarts; no more until something changes
  };

  struct capture_timing_t {
    clock::duration grace = 1s;  ///< Capture may be re-initializing by itself; give it this long
    clock::duration retry_after = 5s;  ///< Then restart it, and again after this long
    unsigned max_restarts = 3;
  };

  /**
   * @brief Whether capture is on the virtual display it should be on. After the display was off,
   * capture runs on another display, and stays there once the display is back until it restarts.
   */
  class capture_follow_t {
  public:
    explicit capture_follow_t(capture_timing_t timing = {}):
        _timing(timing) {
    }

    void reset() {
      _since.reset();
      _restarts = 0;
      _stalled = false;
    }

    /**
     * @param should_follow Capture should be on the display: someone streams, the display is on, a
     * capture runs, and the display is the one to capture (not another picked with the display
     * shortcut).
     * @param on_display Capture is on the display.
     */
    capture_e step(clock::time_point now, bool should_follow, bool on_display) {
      if (!should_follow) {
        reset();
        return capture_e::fine;
      }
      if (on_display) {
        bool restarted = _restarts > 0;
        reset();
        return restarted ? capture_e::confirmed : capture_e::fine;
      }
      if (_stalled) {
        return capture_e::waiting;
      }
      if (!_since) {
        _since = now;
        return capture_e::waiting;
      }

      auto due = _restarts == 0 ? _timing.grace : _timing.retry_after;
      if (now - *_since < due) {
        return capture_e::waiting;
      }
      if (_restarts >= _timing.max_restarts) {
        _stalled = true;
        return capture_e::stalled;
      }
      _restarts++;
      _since = now;
      return capture_e::restart;
    }

    unsigned restarts() const {
      return _restarts;
    }

  private:
    capture_timing_t _timing;
    std::optional<clock::time_point> _since;
    unsigned _restarts = 0;
    bool _stalled = false;
  };
}  // namespace vdisplay_recovery_policy
