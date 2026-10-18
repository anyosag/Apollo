/**
 * @file src/fork/touchpad.h
 * @brief Trackpad passthrough on the host: turns LuzLunar's frames (luztouchpad.h) into the
 * contacts Windows' synthetic Precision Touchpad takes, one client at a time.
 *
 * Windows makes the pointer motion, taps, scrolling with inertia and gestures from these contacts
 * itself, with the user's touchpad settings. What it needs, from Microsoft's touchpad injection
 * sample (precision-touchpad-guide): every finger on the pad in every injection, flagged
 * in range, in contact and confident; a finger that lifts injected once more with only
 * "confident"; positions in himetric (1/100 mm) on the pad; and a strictly increasing time,
 * since Windows reads velocity from it (inertia, flick speed).
 *
 * The time comes from the client's clock, so network jitter doesn't turn into speed changes:
 * each gesture anchors the client's clock to the host's, then follows the client's steps, kept
 * no later than now and no more than 300 ms behind.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own. The pointer flag
 * values are Windows' (winuser.h); touchpad_win.cpp checks them against Windows.h.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>

// local includes
#include "luztouchpad.h"

namespace fork_touchpad {
  /// `POINTER_FLAG_*` from winuser.h
  constexpr std::uint32_t flag_inrange = 0x00000002;
  constexpr std::uint32_t flag_incontact = 0x00000004;
  constexpr std::uint32_t flag_firstbutton = 0x00000010;
  constexpr std::uint32_t flag_confidence = 0x00004000;
  constexpr std::uint32_t flag_canceled = 0x00008000;

  /// `POINTER_BUTTON_CHANGE_TYPE` from winuser.h
  enum class button_change_e : int {
    none = 0,  ///< POINTER_CHANGE_NONE
    down = 1,  ///< POINTER_CHANGE_FIRSTBUTTON_DOWN
    up = 2,  ///< POINTER_CHANGE_FIRSTBUTTON_UP
  };

  constexpr std::int64_t us_per_ms = 1000;
  constexpr std::int64_t max_lag_us = 300 * us_per_ms;  ///< How far behind now a client time may land
  constexpr std::int64_t min_step_us = 1 * us_per_ms;  ///< Windows' time has 1 ms steps; each injection moves it on
  constexpr std::int64_t repeat_after_us = 250 * us_per_ms;  ///< Re-inject held fingers after this much silence
  constexpr std::int64_t cancel_after_us = 1500 * us_per_ms;  ///< Lift everything when the client is gone this long

  /// One contact to inject
  struct entry_t {
    std::uint32_t pointer_id = 0;  ///< The client's slot, 0-4
    std::uint32_t flags = 0;
    button_change_e button_change = button_change_e::none;
    std::int32_t x = 0;  ///< Himetric from the pad's left edge
    std::int32_t y = 0;  ///< Himetric from the pad's top edge
  };

  /// What to hand InjectSyntheticPointerInput, in slot order
  struct injection_t {
    std::size_t count = 0;
    std::array<entry_t, luztouchpad::k_max_contacts> entries {};
    std::int64_t time_us = 0;  ///< On the host's clock
  };

  /// What the client's silence calls for
  enum class tick_e {
    idle,  ///< Nothing is held, so nothing to do
    wait,  ///< Fingers are held and the client is still talking
    repeat,  ///< Fingers are held but the client is quiet: inject them again so Windows keeps them
    cancel,  ///< The client has been gone too long: lift everything
  };

  /// One client's touchpad: its fingers, its button and its clock
  class host_t {
  public:
    /// The size of the device Windows was given, which positions are scaled to
    void set_device_size(std::uint16_t width, std::uint16_t height) {
      device_width = width;
      device_height = height;
    }

    std::uint16_t width() const {
      return device_width;
    }

    std::uint16_t height() const {
      return device_height;
    }

    /// Whether a finger is down or the pad is pressed
    bool active() const {
      if (button_down) {
        return true;
      }
      for (auto &s : slots) {
        if (s.active) {
          return true;
        }
      }
      return false;
    }

    /// Whether the device must be made again for a frame's pad size. Only between gestures,
    /// so a finger is never moved to another device.
    bool needs_recreate(std::uint16_t width, std::uint16_t height) const {
      return (width != device_width || height != device_height) && !active();
    }

    /// How many fingers a frame moved that the host never saw go down (a lost frame)
    std::uint32_t adopted() const {
      return adopted_count;
    }

    /**
     * @brief Applies a frame that arrived at @p now_us on the host's clock.
     * @return The contacts to inject; none when the frame changes nothing Windows needs.
     */
    injection_t apply(const luztouchpad::frame &frame, std::int64_t now_us) {
      last_frame_us = now_us;
      if (frame.flags & luztouchpad::flag_cancel_all) {
        return cancel_all(now_us);
      }

      bool was_active = active();
      bool pressed = (frame.buttons & luztouchpad::button_pressed) != 0;
      button_change_e change = button_change_e::none;
      if (pressed != button_down) {
        change = pressed ? button_change_e::down : button_change_e::up;
      }

      std::array<bool, luztouchpad::k_max_contacts> listed {};
      std::array<int, luztouchpad::k_max_contacts> ending {};  // 0 stays, 1 lifts, 2 is cancelled
      for (std::size_t i = 0; i < frame.count && i < luztouchpad::k_max_contacts; i++) {
        const auto &c = frame.contacts[i];
        if (c.slot >= luztouchpad::k_max_contacts) {
          continue;
        }
        auto &s = slots[c.slot];
        listed[c.slot] = true;
        switch (c.phase) {
          case luztouchpad::phase_down:
          case luztouchpad::phase_move:
            if (!s.active) {
              if (c.phase == luztouchpad::phase_move) {
                adopted_count++;
              }
              s.active = true;
            }
            s.x = scale(c.x, device_width);
            s.y = scale(c.y, device_height);
            break;
          case luztouchpad::phase_up:
            if (s.active) {
              s.x = scale(c.x, device_width);
              s.y = scale(c.y, device_height);
              ending[c.slot] = 1;
            }
            break;
          case luztouchpad::phase_cancel:
            if (s.active) {
              ending[c.slot] = 2;
            }
            break;
          default:
            break;
        }
      }

      // A finger the frame leaves out has lifted
      bool any_stays = false;
      for (std::size_t slot = 0; slot < luztouchpad::k_max_contacts; slot++) {
        if (slots[slot].active && !listed[slot] && ending[slot] == 0) {
          ending[slot] = 1;
        }
        if (slots[slot].active && ending[slot] == 0) {
          any_stays = true;
        }
      }

      // The button can't stay down without a finger on the pad. The Mac may report the click's
      // release just after the lift, so release it with the last finger.
      if (pressed && !any_stays) {
        pressed = false;
        change = button_down ? button_change_e::up : button_change_e::none;
      }

      if (!was_active) {
        anchor(frame.time_ms, now_us);
      }

      injection_t out;
      for (std::size_t slot = 0; slot < luztouchpad::k_max_contacts; slot++) {
        auto &s = slots[slot];
        if (!s.active) {
          continue;
        }
        entry_t e;
        e.pointer_id = (std::uint32_t) slot;
        e.x = s.x;
        e.y = s.y;
        e.button_change = change;
        if (ending[slot] == 1) {
          e.flags = flag_confidence;
        } else if (ending[slot] == 2) {
          e.flags = flag_canceled | flag_confidence;
        } else {
          e.flags = flag_inrange | flag_incontact | flag_confidence | (pressed ? flag_firstbutton : 0);
        }
        out.entries[out.count++] = e;
        if (ending[slot] != 0) {
          s.active = false;
        }
      }
      button_down = pressed;

      if (out.count == 0) {
        return out;
      }
      out.time_us = client_time(frame.time_ms, now_us);
      return out;
    }

    /// The held fingers again, unmoved, for when the client is quiet
    injection_t repeat(std::int64_t now_us) {
      injection_t out;
      for (std::size_t slot = 0; slot < luztouchpad::k_max_contacts; slot++) {
        auto &s = slots[slot];
        if (!s.active) {
          continue;
        }
        entry_t e;
        e.pointer_id = (std::uint32_t) slot;
        e.x = s.x;
        e.y = s.y;
        e.flags = flag_inrange | flag_incontact | flag_confidence | (button_down ? flag_firstbutton : 0);
        out.entries[out.count++] = e;
      }
      if (out.count > 0) {
        out.time_us = next_time(now_us);
      }
      return out;
    }

    /// Cancels every finger and releases the button
    injection_t cancel_all(std::int64_t now_us) {
      injection_t out;
      for (std::size_t slot = 0; slot < luztouchpad::k_max_contacts; slot++) {
        auto &s = slots[slot];
        if (!s.active) {
          continue;
        }
        entry_t e;
        e.pointer_id = (std::uint32_t) slot;
        e.x = s.x;
        e.y = s.y;
        e.flags = flag_canceled | flag_confidence;
        e.button_change = button_down ? button_change_e::up : button_change_e::none;
        out.entries[out.count++] = e;
        s.active = false;
      }
      button_down = false;
      if (out.count > 0) {
        out.time_us = next_time(now_us);
      }
      return out;
    }

    /// Records an injection made at @p now_us, so tick() knows when the last one was
    void injected(std::int64_t now_us) {
      last_inject_us = now_us;
    }

    /// What to do on a timer tick at @p now_us
    tick_e tick(std::int64_t now_us) const {
      if (!active()) {
        return tick_e::idle;
      }
      if (now_us - last_frame_us >= cancel_after_us) {
        return tick_e::cancel;
      }
      if (now_us - last_frame_us >= repeat_after_us && now_us - last_inject_us >= repeat_after_us) {
        return tick_e::repeat;
      }
      return tick_e::wait;
    }

  private:
    struct slot_t {
      bool active = false;
      std::int32_t x = 0;
      std::int32_t y = 0;
    };

    /// 0-65535 on the pad to himetric on a device @p size himetric across
    static std::int32_t scale(std::uint16_t value, std::uint16_t size) {
      if (size == 0) {
        return 0;
      }
      std::int64_t v = ((std::int64_t) value * size + 32767) / 65535;
      return (std::int32_t) (v > size ? size : v);
    }

    void anchor(std::uint32_t client_ms, std::int64_t now_us) {
      anchor_client_ms = client_ms;
      anchor_host_us = now_us;
      anchored = true;
    }

    /// The host time for a client time, following the client's steps since the gesture began
    std::int64_t client_time(std::uint32_t client_ms, std::int64_t now_us) {
      if (!anchored) {
        anchor(client_ms, now_us);
      }
      // The difference works across the client clock wrapping
      std::int64_t t = anchor_host_us + (std::int64_t) (std::int32_t) (client_ms - anchor_client_ms) * us_per_ms;
      if (t > now_us) {
        // The client's clock ran ahead of ours: never inject from the future
        anchor_host_us -= t - now_us;
        t = now_us;
      } else if (now_us - t > max_lag_us) {
        // Frames were held up (a network stall): catch up instead of replaying old times
        anchor_host_us += now_us - t - max_lag_us;
        t = now_us - max_lag_us;
      }
      return next_time(t);
    }

    /// @p t, moved on if needed so the time always increases
    std::int64_t next_time(std::int64_t t) {
      if (has_time && t < last_time_us + min_step_us) {
        t = last_time_us + min_step_us;
      }
      last_time_us = t;
      has_time = true;
      return t;
    }

    std::array<slot_t, luztouchpad::k_max_contacts> slots {};
    bool button_down = false;
    std::uint16_t device_width = 0;
    std::uint16_t device_height = 0;

    bool anchored = false;
    std::uint32_t anchor_client_ms = 0;
    std::int64_t anchor_host_us = 0;
    bool has_time = false;
    std::int64_t last_time_us = 0;

    std::int64_t last_frame_us = 0;
    std::int64_t last_inject_us = 0;
    std::uint32_t adopted_count = 0;
  };
}  // namespace fork_touchpad
