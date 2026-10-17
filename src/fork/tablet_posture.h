/**
 * @file src/fork/tablet_posture.h
 * @brief Fork-only: Windows in tablet posture while a client uses the stream as a touchscreen.
 *
 * Windows 11 has no tablet mode switch. A 2-in-1 enters tablet posture when its keyboard folds away
 * (touch-friendly taskbar spacing and touch-sized controls), and two values under
 * `HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl` say whether it's there:
 * `ConvertibilityEnabled` (since 22H2; non-zero: the PC is a convertible) and `ConvertibleSlateMode`
 * (0: tablet, 1: laptop). Tools that switch real 2-in-1s by hand set both and broadcast
 * `WM_SETTINGCHANGE` with "ConvertibleSlateMode". This does the same, and puts the values back
 * when the last client lets go. Whether Windows follows them on a desktop PC is what the log is for.
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks::tablet_posture {
  /**
   * @brief While at least one exists, Windows is in tablet posture. The last one to go puts the
   * values back a few seconds later, so a client reconnecting doesn't switch it off and on.
   */
  class hold_t {
  public:
    /// @param device_name The client's name, for the log.
    explicit hold_t(const std::string &device_name);
    ~hold_t();

    hold_t(const hold_t &) = delete;
    hold_t &operator=(const hold_t &) = delete;
  };

  /// At startup: puts back the values a run that stopped during tablet posture left behind.
  void recover();
}  // namespace fork_tweaks::tablet_posture
