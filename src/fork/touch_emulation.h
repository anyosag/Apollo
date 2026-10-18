/**
 * @file src/fork/touch_emulation.h
 * @brief Touch from stock Moonlight for iOS: its Touchscreen mode sends each finger as an absolute
 * mouse position and clicks, which this turns back into a touch contact for Windows.
 *
 * Moonlight for iOS (moonlight-stream/moonlight-ios) sends touch events only for the Apple Pencil,
 * never for fingers. Its two finger modes send mouse input:
 * - Touchscreen (`AbsoluteTouchHandler.m`): when a finger goes down, its position and a left press,
 *   together. A tap within 250 ms and 2.5% of the last finger up sends no position, so a double tap
 *   lands on one spot. While the finger moves, positions. When it goes up, a left release and then a
 *   right release. A finger that stays within 1% of where it went down for 650 ms gets a left release
 *   and a right press: a long press is a right click. A second finger is ignored.
 * - Touchpad (`RelativeTouchHandler.m`): relative motion and clicks, like a laptop's touchpad.
 *
 * An iPad's mouse or trackpad sends positions while hovering, with no button down, or relative
 * motion, and scrolls.
 *
 * So a left press is a finger when exactly one position arrived since the last button event, just
 * before it, or when none did and a finger went up just before (the second tap of a double tap).
 * Everything else stays a mouse. A finger's left release waits for the next button event: a right
 * release lifts the finger, a right press makes it a right click at that spot.
 *
 * Header-only, without Boost, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace touch_emulation {
  using clock = std::chrono::steady_clock;

  /// moonlight-common-c's mouse buttons (`BUTTON_LEFT`, `BUTTON_RIGHT` in Limelight.h)
  constexpr int button_left = 1;
  constexpr int button_right = 3;

  /// A finger's position and its press are sent together; this is how far apart they may be handled
  constexpr auto finger_press_window = std::chrono::milliseconds(50);

  /// The client sends no position for a tap within 250 ms of the last finger up
  constexpr auto double_tap_window = std::chrono::milliseconds(300);

  /// How long a finger's release waits for the right button event that says what it was, should
  /// the event not come: then the finger goes up
  constexpr auto release_wait = std::chrono::milliseconds(100);

  /**
   * @brief Whether a request's User-Agent is stock Moonlight for iOS or iPadOS. Its HTTP requests
   * use NSURLSession's default User-Agent, which names the app's bundle, `Moonlight/<build>
   * CFNetwork/<version> Darwin/<version>`. The Apple TV app is "Moonlight TV".
   */
  inline bool is_moonlight_ios(std::string_view user_agent) {
    constexpr std::string_view app = "Moonlight/";
    return user_agent.substr(0, app.size()) == app &&
           user_agent.find(" CFNetwork/") != std::string_view::npos &&
           user_agent.find(" Darwin/") != std::string_view::npos;
  }

  /// A User-Agent for the log: at most 160 characters, control characters as '?'
  inline std::string printable(std::string_view user_agent) {
    std::string text {user_agent.substr(0, 160)};
    for (auto &c : text) {
      if ((unsigned char) c < 0x20 || c == 0x7F) {
        c = '?';
      }
    }
    return text;
  }

  /// A position on the client's video, 0 to 1 across and down
  struct point_t {
    float x = 0;
    float y = 0;

    bool operator==(const point_t &) const = default;
  };

  /// One thing to do for an event, in order
  struct step_t {
    enum kind_e {
      touch_down,  ///< The finger touches at `at`
      touch_move,  ///< It moves to `at`
      touch_up,  ///< It lifts
      touch_cancel,  ///< It's cancelled, so Windows doesn't take it as a tap
      mouse_move,  ///< The mouse moves to `at`, a position held back earlier
      pass,  ///< The event itself goes on as a mouse event, as upstream handles it
    };

    kind_e kind;
    point_t at {};

    bool operator==(const step_t &) const = default;
  };

  using steps_t = std::vector<step_t>;

  /// What a stream's fingers did
  struct counts_t {
    uint64_t contacts = 0;  ///< Fingers sent as touch
    uint64_t long_presses = 0;  ///< Of those, long presses that became right clicks
    uint64_t mouse_clicks = 0;  ///< Left presses left as mouse clicks: touchpad mode, a mouse or a trackpad
  };

  /**
   * @brief Follows one client's mouse events and says which are fingers.
   *
   * Not thread-safe: Apollo calls it from the input thread only.
   */
  class tracker_t {
  public:
    /// An absolute mouse position
    steps_t position(point_t p, clock::time_point now) {
      steps_t steps;
      switch (_contact) {
        case contact_e::down:
          _at = p;
          return {{step_t::touch_move, p}};
        case contact_e::lifting:
          // The client sends nothing between a finger's two releases, so this is the next finger
          steps.push_back({step_t::touch_up, _at});
          end_contact(now);
          break;
        case contact_e::long_press:
          // A right button held after a long press: the mouse drags
          return {{step_t::pass}};
        case contact_e::none:
          break;
      }

      _positions++;
      _last_position = now;
      _latest = p;
      if (!_engaged) {
        steps.push_back({step_t::pass});
        return steps;
      }

      // While fingers are in use, a position waits for the press that would make it a finger, so the
      // mouse doesn't jump to each tap first. A second one without a press is a pointer hovering.
      if (!_held) {
        _held = p;
        return steps;
      }
      _held.reset();
      _engaged = false;
      steps.push_back({step_t::pass});
      return steps;
    }

    /// A mouse button going down or up
    steps_t button(int button, bool pressed, clock::time_point now) {
      auto positions = _positions;
      _positions = 0;

      if (button == button_left && pressed) {
        steps_t steps;
        switch (_contact) {
          case contact_e::down:
            return {};
          case contact_e::long_press:
            return {{step_t::pass}};
          case contact_e::lifting:
            // A new tap before the right release that should have lifted the last one
            steps.push_back({step_t::touch_up, _at});
            end_contact(now);
            break;
          case contact_e::none:
            break;
        }

        auto held = _held;
        _held.reset();
        auto double_tap = positions == 0 && _last_lift && now - *_last_lift <= double_tap_window;
        if ((positions == 1 && now - _last_position <= finger_press_window) || double_tap) {
          if (positions == 1) {
            _at = _latest;
          }
          _contact = contact_e::down;
          _engaged = true;
          _counts.contacts++;
          steps.push_back({step_t::touch_down, _at});
          return steps;
        }

        _engaged = false;
        _counts.mouse_clicks++;
        if (held) {
          steps.push_back({step_t::mouse_move, *held});
        }
        steps.push_back({step_t::pass});
        return steps;
      }

      if (button == button_left) {
        switch (_contact) {
          case contact_e::down:
            _contact = contact_e::lifting;
            _lift_deadline = now + release_wait;
            return {};
          case contact_e::lifting:
          case contact_e::long_press:
            // Its press never reached Windows as a mouse button
            return {};
          case contact_e::none:
            return {{step_t::pass}};
        }
      }

      if (button == button_right && pressed) {
        switch (_contact) {
          case contact_e::down:
          case contact_e::lifting:
            // A long press: Moonlight's right click, where the finger is
            _contact = contact_e::long_press;
            _counts.long_presses++;
            return {{step_t::touch_cancel, _at}, {step_t::mouse_move, _at}, {step_t::pass}};
          case contact_e::long_press:
            return {};
          case contact_e::none:
            break;
        }
        return mouse_in_use(now);
      }

      if (button == button_right) {
        switch (_contact) {
          case contact_e::down:
            return {};
          case contact_e::lifting:
            end_contact(now);
            return {{step_t::touch_up, _at}};
          case contact_e::long_press:
            end_contact(now);
            return {{step_t::pass}};
          case contact_e::none:
            // The client's right release after a finger that went up some other way
            return {{step_t::pass}};
        }
      }

      // The middle and side buttons only come from a mouse
      return mouse_in_use(now);
    }

    /// Relative mouse motion or scrolling: touchpad mode, a mouse or a trackpad
    steps_t pointer(clock::time_point now) {
      return mouse_in_use(now);
    }

    /// When the finger's release stops waiting for its right button event
    std::optional<clock::time_point> deadline() const {
      if (_contact != contact_e::lifting) {
        return std::nullopt;
      }
      return _lift_deadline;
    }

    /// Called at or after deadline()
    steps_t expire(clock::time_point now) {
      if (_contact != contact_e::lifting || now < _lift_deadline) {
        return {};
      }
      end_contact(now);
      return {{step_t::touch_up, _at}};
    }

    /// The stream ended: a finger still down is cancelled (a right button is upstream's to release)
    steps_t end() {
      steps_t steps;
      if (_contact == contact_e::down || _contact == contact_e::lifting) {
        steps.push_back({step_t::touch_cancel, _at});
      }
      _contact = contact_e::none;
      _held.reset();
      _engaged = false;
      return steps;
    }

    const counts_t &counts() const {
      return _counts;
    }

  private:
    enum class contact_e {
      none,
      down,
      lifting,  ///< Left released; the right button says whether it went up or was a long press
      long_press,  ///< Now Moonlight's right click, until the right button goes up
    };

    steps_t mouse_in_use(clock::time_point now) {
      steps_t steps;
      if (_contact == contact_e::down || _contact == contact_e::lifting) {
        steps.push_back({step_t::touch_cancel, _at});
        end_contact(now);
      }
      if (_held) {
        steps.push_back({step_t::mouse_move, *_held});
        _held.reset();
      }
      _engaged = false;
      _last_lift.reset();
      steps.push_back({step_t::pass});
      return steps;
    }

    void end_contact(clock::time_point now) {
      _contact = contact_e::none;
      _last_lift = now;
    }

    contact_e _contact = contact_e::none;
    point_t _at;  ///< Where the finger is, or last was
    clock::time_point _lift_deadline;
    std::optional<clock::time_point> _last_lift;

    bool _engaged = false;  ///< The last left press was a finger, and no mouse since
    std::optional<point_t> _held;  ///< A position held back while engaged

    int _positions = 0;  ///< Positions since the last button event, without a finger down
    clock::time_point _last_position;
    point_t _latest;

    counts_t _counts;
  };
}  // namespace touch_emulation
