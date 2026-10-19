/**
 * @file src/fork/touch_keyboard.h
 * @brief Fork-only: Windows' touch keyboard for stock Moonlight for iOS's taps in text fields.
 *
 * Apollo runs as SYSTEM, and the touch keyboard only answers callers of the signed-in user, so the
 * work is done by tools\apollo-touch-keyboard.exe, started as that user (unelevated) while a client
 * holds this. Apollo hands it the client's taps and typing; what it makes of them is in
 * touch_keyboard_policy.h, and its log lines come back as `fork: touch keyboard: …`.
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks::touch_keyboard {
  /**
   * @brief While at least one exists, the helper runs. The last one to go stops it a few seconds
   * later, so a client reconnecting doesn't restart it.
   */
  class hold_t {
  public:
    /// @param device_name The client's name, for the log.
    explicit hold_t(const std::string &device_name);
    ~hold_t();

    hold_t(const hold_t &) = delete;
    hold_t &operator=(const hold_t &) = delete;

    /// A finger tapped. Doesn't wait for the helper.
    void tapped();

    /// The client typed: a hardware keyboard on the iPad, or the iOS keyboard. Doesn't wait for the helper.
    void typed();
  };
}  // namespace fork_tweaks::touch_keyboard
