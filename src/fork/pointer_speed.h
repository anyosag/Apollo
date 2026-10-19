/**
 * @file src/fork/pointer_speed.h
 * @brief Fork-only: Windows moves the pointer exactly as far as it's told while LuzLunar's Mac
 * trackpad mode streams.
 *
 * In that mode the Mac sends its pointer motion after macOS's own acceleration, so it feels like
 * the Mac's trackpad. Windows would accelerate it again (Enhance pointer precision, and the
 * pointer speed slider), and the pointer would feel like neither. While a hold exists, Enhance
 * pointer precision is off and the speed is 10, one pixel a count (pointer_speed_state.h). Only
 * the running session's values change, never the user's saved ones, so signing out puts them back
 * too. The last hold to go puts the values from before back a few seconds later; a state file
 * puts them back at the next start if Apollo stops in between.
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks::pointer_speed {
  /**
   * @brief While at least one exists, Windows doesn't accelerate the pointer. The last one to go
   * puts the values back a few seconds later, so a client reconnecting doesn't switch them back
   * and forth.
   */
  class hold_t {
  public:
    /// @param device_name The client's name, for the log.
    explicit hold_t(const std::string &device_name);
    ~hold_t();

    hold_t(const hold_t &) = delete;
    hold_t &operator=(const hold_t &) = delete;
  };

  /// At startup: puts back the values a run that stopped during a hold left behind.
  void recover();
}  // namespace fork_tweaks::pointer_speed
