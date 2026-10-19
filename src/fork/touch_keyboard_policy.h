/**
 * @file src/fork/touch_keyboard_policy.h
 * @brief When Windows' touch keyboard comes up for stock Moonlight for iOS: which fingers are taps,
 * which focused elements take text, and when the keyboard is shown and hidden.
 *
 * The client's fingers reach Windows as touch (touch_emulation.h), but Windows only opens its touch
 * keyboard by itself on a PC without a keyboard attached. So Apollo's helper
 * (tools/apollo-touch-keyboard.cpp) does: a tap is followed by a look at what has the focus, and a
 * text field gets the keyboard.
 * - A tap is a finger that went up near where it went down, soon. A drag scrolls, and a long press is
 *   Moonlight's right click (touch_emulation.h cancels it), so neither opens the keyboard.
 * - A field is judged from what UI Automation says about the focused element: an Edit unless it's
 *   read-only, a combo box only when its value can be written, a document or group (an editor, a
 *   web page's contenteditable) only when its text where the caret is can be written, and the
 *   terminals by their class names. A browser's page offers the Text and TextEdit patterns too, so
 *   those alone don't make text. Menus, and the touch keyboard itself, leave it as it is. Nothing a
 *   field holds or is called is ever read.
 * - Keys or text from the client mean a hardware keyboard or the iOS keyboard is in use: a keyboard
 *   that's up closes, and taps don't open it until the client has typed nothing for 30 s.
 * - The keyboard the helper opened closes when a tap lands on something that isn't text, or when the
 *   focus has left text at two looks in a row, 300 ms apart. One the user or Windows opened is only
 *   closed by the client's typing.
 * - Windows' only way to open and close it, ITipInvocation::Toggle, flips it, and what Windows says
 *   is up lags behind while it slides in or out. So for 1 s after a toggle, the policy goes by what
 *   it asked for. Then, if it asked for the keyboard and Windows says it isn't up, it toggles once
 *   more: the first toggle after the keyboard's process starts can be lost. When the helper can't
 *   tell a floating keyboard is up (only IFrameworkInputPane, which sees docked ones), it never goes
 *   by Windows after its own toggles.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <chrono>
#include <cmath>
#include <optional>
#include <string_view>

namespace touch_keyboard_policy {
  using clock = std::chrono::steady_clock;

  /// A finger that moved further than this, across or down the video (0 to 1), was a drag
  constexpr float tap_slop = 0.02f;

  /// A finger down for longer than this wasn't a tap. Moonlight makes 650 ms a right click, which
  /// cancels the touch, so this only catches what that missed
  constexpr auto tap_max = std::chrono::milliseconds(900);

  /// After a tap, how long the app gets to move the focus before it's looked at. Also long enough for
  /// Windows to open the keyboard itself where it does, so a toggle doesn't close that one
  constexpr auto settle = std::chrono::milliseconds(400);

  /// While the helper's keyboard is up, how often the focus is looked at
  constexpr auto poll = std::chrono::milliseconds(300);

  /// Looks in a row that found no text before the helper's keyboard closes
  constexpr int leave_polls = 2;

  /// After the client's last key, how long taps don't open the keyboard
  constexpr auto key_quiet = std::chrono::seconds(30);

  /// After a toggle, how long what was asked for counts over what Windows reports
  constexpr auto toggle_settle = std::chrono::milliseconds(1000);

  /**
   * @brief Says whether a finger was a tap.
   *
   * Not thread-safe: Apollo calls it from the input thread only.
   */
  class tap_tracker_t {
  public:
    /// The finger touches at (x, y), 0 to 1 across and down the video
    void down(float x, float y, clock::time_point now) {
      _down = true;
      _moved = false;
      _x = x;
      _y = y;
      _since = now;
    }

    /// It moves to (x, y)
    void move(float x, float y) {
      if (_down && (std::fabs(x - _x) > tap_slop || std::fabs(y - _y) > tap_slop)) {
        _moved = true;
      }
    }

    /// It lifts. @return Whether it was a tap
    bool up(clock::time_point now) {
      auto tap = _down && !_moved && now - _since <= tap_max;
      _down = false;
      return tap;
    }

    /// It was cancelled: a long press, or the mouse took over
    void cancel() {
      _down = false;
    }

  private:
    bool _down = false;
    bool _moved = false;
    float _x = 0;
    float _y = 0;
    clock::time_point _since;
  };

  /// UI Automation's control type ids (UIA_ComboBoxControlTypeId and so on in UIAutomationClient.h)
  constexpr int control_combo_box = 50003;
  constexpr int control_edit = 50004;
  constexpr int control_menu = 50009;
  constexpr int control_menu_bar = 50010;
  constexpr int control_menu_item = 50011;
  constexpr int control_spinner = 50016;
  constexpr int control_custom = 50025;
  constexpr int control_group = 50026;
  constexpr int control_document = 50030;
  constexpr int control_pane = 50033;

  /// What UI Automation says about the focused element
  struct field_t {
    int control_type = 0;
    bool enabled = true;
    bool has_value = false;  ///< Supports the Value pattern
    bool value_read_only = false;  ///< Its value can't be written
    /// The Text pattern's IsReadOnly where the caret is (or over the whole text); nothing without
    /// the pattern, or when it's mixed
    std::optional<bool> text_read_only;
    std::string_view class_name;
    std::string_view process;  ///< The executable's file name, in lowercase
  };

  /// Whether a field takes text, and why, for the log
  struct verdict_t {
    /// Nothing: leave the keyboard as it is (a menu, or the keyboard itself)
    std::optional<bool> editable;
    std::string_view why;
  };

  /// Whether the field's Text pattern decides it, so the helper should read text_read_only
  inline bool needs_text(const field_t &field) {
    switch (field.control_type) {
      case control_edit:
        return !field.has_value || field.value_read_only;
      case control_document:
      case control_group:
      case control_custom:
      case control_pane:
        return true;
      default:
        return false;
    }
  }

  /// Whether the focused element takes typing
  inline verdict_t judge(const field_t &field) {
    if (field.process == "textinputhost.exe") {
      return {std::nullopt, "the touch keyboard"};
    }
    if (!field.enabled) {
      return {false, "disabled"};
    }

    // Consoles and Windows Terminal take typing as a whole
    if (field.class_name == "ConsoleWindowClass" || field.class_name == "CASCADIA_HOSTING_WINDOW_CLASS" || field.class_name == "TermControl") {
      return {true, "a terminal"};
    }

    auto writable = field.has_value && !field.value_read_only;
    switch (field.control_type) {
      case control_menu:
      case control_menu_bar:
      case control_menu_item:
        // An autocomplete's or app's menu: Windows keeps the keyboard up for them
        return {std::nullopt, "a menu"};
      case control_edit:
        if (writable || field.text_read_only == false) {
          // Rich edits (WinUI's RichEditBox, WPF's RichTextBox) say their value is read-only
          return {true, "a text box"};
        }
        if (field.value_read_only || field.text_read_only == true) {
          return {false, "a read-only text box"};
        }
        return {true, "a text box"};
      case control_combo_box:
      case control_spinner:
        if (writable) {
          return {true, "an editable combo box"};
        }
        return {false, "a list"};
      case control_document:
      case control_group:
      case control_custom:
      case control_pane:
        // A browser's page is a read-only document; an editor's document or a contenteditable can be written
        if (field.text_read_only == false || (field.control_type == control_document && writable)) {
          return {true, "editable text"};
        }
        return {false, "not editable"};
      default:
        return {false, "not text"};
    }
  }

  /**
   * @brief Decides when the keyboard opens and closes.
   *
   * Each call takes whether Windows reports the keyboard up, and returns what to do: show and hide
   * are a toggle each, which the caller makes. Not thread-safe: the helper calls it from one thread.
   */
  class policy_t {
  public:
    enum class action_e {
      none,
      show,  ///< Toggle it up
      hide,  ///< Toggle it down
    };

    /// @param trust_windows Whether what Windows reports covers a floating keyboard too
    explicit policy_t(bool trust_windows = true):
        _trust_windows {trust_windows} {
    }

    /// A tap: the focus is looked at from next_check()
    void tap(clock::time_point now) {
      _tap_check = now + settle;
    }

    /// A key or text from the client
    action_e key(bool visible, clock::time_point now) {
      _last_key = now;
      _tap_check.reset();
      if (!up(visible, now)) {
        return action_e::none;
      }
      return toggle(false, now);
    }

    /// When the focus should be looked at next; nothing while there's no reason to
    std::optional<clock::time_point> next_check() const {
      std::optional<clock::time_point> next = _tap_check;
      auto sooner = [&next](std::optional<clock::time_point> t) {
        if (t && (!next || *t < *next)) {
          next = t;
        }
      };
      sooner(_verify_at);
      if (_ours) {
        sooner(_next_poll);
      }
      return next;
    }

    /**
     * @brief The focused element, looked at no earlier than next_check().
     * @param editable Whether it takes text; nothing when it couldn't be read (an elevated window, or
     * an app that didn't answer) or should leave the keyboard as it is (a menu).
     * @param visible Whether Windows reports the keyboard up.
     */
    action_e focus(std::optional<bool> editable, bool visible, clock::time_point now) {
      // A second toggle where the first one was lost
      if (_verify_at && now >= *_verify_at) {
        _verify_at.reset();
        if (_trust_windows && _ours && !visible) {
          return toggle(true, now, true);
        }
      }

      auto is_up = up(visible, now);
      if (_ours && !is_up) {
        // The user closed it, or Windows did
        _ours = false;
      }

      if (_tap_check && now >= *_tap_check) {
        _tap_check.reset();
        if (!editable) {
          return action_e::none;
        }
        if (*editable) {
          _misses = 0;
          if (is_up || typing(now)) {
            return action_e::none;
          }
          return toggle(true, now);
        }
        if (_ours && is_up) {
          return toggle(false, now);
        }
        return action_e::none;
      }

      if (_ours && now >= _next_poll) {
        _next_poll = now + poll;
        if (!editable) {
          return action_e::none;
        }
        if (*editable) {
          _misses = 0;
          return action_e::none;
        }
        if (++_misses >= leave_polls) {
          return toggle(false, now);
        }
      }
      return action_e::none;
    }

    /// Whether the keyboard that's up is the one this opened, so it should close when Apollo stops
    bool ours(bool visible, clock::time_point now) const {
      return _ours && up(visible, now);
    }

  private:
    /// Whether it's up: what was asked for, right after a toggle (or always after one, when Windows
    /// can't be trusted); otherwise what Windows says
    bool up(bool visible, clock::time_point now) const {
      if (_toggled && (!_trust_windows || now - *_toggled < toggle_settle)) {
        return _asked_up;
      }
      return visible;
    }

    /// The client typed recently
    bool typing(clock::time_point now) const {
      return _last_key && now - *_last_key < key_quiet;
    }

    action_e toggle(bool show, clock::time_point now, bool retry = false) {
      _toggled = now;
      _asked_up = show;
      _ours = show;
      _misses = 0;
      _next_poll = now + poll;
      _verify_at.reset();
      if (show && !retry) {
        _verify_at = now + toggle_settle;
      }
      return show ? action_e::show : action_e::hide;
    }

    bool _trust_windows;
    std::optional<clock::time_point> _tap_check;
    std::optional<clock::time_point> _last_key;
    std::optional<clock::time_point> _toggled;
    std::optional<clock::time_point> _verify_at;  ///< When a show is checked, once
    bool _asked_up = false;
    bool _ours = false;  ///< The keyboard was opened by this
    int _misses = 0;
    clock::time_point _next_poll;
  };
}  // namespace touch_keyboard_policy
