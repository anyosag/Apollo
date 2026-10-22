/**
 * @file src/fork/vdisplay_recovery.h
 * @brief Fork-only: brings a stream's virtual display back when Windows switches it off mid-stream
 * (a monitor on the PC waking up) or the driver loses it, and puts capture back on it.
 *
 * proc_t starts one monitor per virtual display once the app's display is set up and stops it
 * before the display goes. While someone streams it looks at the display four times a second and
 * follows vdisplay_recovery_policy.h: switch it on again, re-create it, back off; and once it's on,
 * restart capture if capture is on another display. Between streams it does nothing, so it never
 * switches off a monitor someone is using at the PC.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace fork_tweaks::vdisplay_recovery {
  /// The display capture is on now, from the capture thread; empty while nothing captures
  void note_capture(const std::string &display);
  void forget_capture();
  std::string captured();

  class monitor_t {
  public:
    /// Starts watching the virtual display with this GUID (Windows only; a no-op elsewhere)
    explicit monitor_t(const std::array<std::uint8_t, 16> &guid);

    /// Stops watching, waiting for an action in progress to finish
    ~monitor_t();

    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;

    /// The display was just set up again (a resume's new mode or re-created display): wait a while
    /// before judging it, from scratch
    void settle();

  private:
    void run();

    std::array<std::uint8_t, 16> _guid;
    std::mutex _mutex;
    std::condition_variable _wake;
    bool _stop = false;
    std::atomic<bool> _settle {false};
    std::thread _thread;
  };
}  // namespace fork_tweaks::vdisplay_recovery
