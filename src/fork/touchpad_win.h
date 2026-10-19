/**
 * @file src/fork/touchpad_win.h
 * @brief Trackpad passthrough on Windows: one synthetic Precision Touchpad per client, fed with
 * LuzLunar's frames (luztouchpad.h) through fork_touchpad::host_t (touchpad.h).
 *
 * The device is made with a client's first frame and removed with its session. While fingers
 * are held, a timer re-injects them if the client goes quiet (Windows drops a contact that isn't
 * injected again within about half a second, and the finger's next move then starts over as a
 * new contact, losing part of it) and lifts them once it has been gone for 1.5 s.
 */
#pragma once

#ifdef _WIN32

// standard includes
  #include <memory>

// local includes
  #include "luztouchpad.h"

namespace fork_touchpad {
  /**
   * @brief Whether this Windows makes synthetic touchpads.
   * @details The first call makes one and removes it again; once that works, later calls just
   * return true. Without the API it's false for good; a failed attempt is tried again next time.
   */
  bool available();

  class session_t;

  /**
   * @brief A client's touchpad, with no device until its first frame.
   * @param gesture_only For LuzLunar's Mac trackpad mode: the device makes gestures (three- and
   * four-finger swipes, pinch) but never moves the pointer or clicks, which macOS does.
   */
  std::shared_ptr<session_t> make_session(bool gesture_only);

  /// Feeds a frame. Call on the input thread (task_pool), like all of this client's input.
  void frame(const std::shared_ptr<session_t> &session, const luztouchpad::frame &frame);

  /// Lifts every finger, for when the stream stops. Call on the input thread.
  void cancel(const std::shared_ptr<session_t> &session);
}  // namespace fork_touchpad

namespace platf {
  class deinit_t;
  typedef deinit_t client_input_t;

  /// Feeds a frame to the client's touchpad, making it on first use, gestures only with @p gesture_only (platform/windows/input.cpp)
  void touchpad_frame(client_input_t *input, const luztouchpad::frame &frame, bool gesture_only);

  /// Lifts every finger on the client's touchpad, if it has one (platform/windows/input.cpp)
  void touchpad_cancel(client_input_t *input);
}  // namespace platf

#endif
