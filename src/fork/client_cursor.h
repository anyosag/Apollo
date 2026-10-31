/**
 * @file src/fork/client_cursor.h
 * @brief Fork-only: LuzLunar draws the PC's pointer itself, in Windows' own shapes, with none of
 * the stream's latency.
 *
 * A stream launched with luzCursor=1 (fork_client_cursor on, /serverinfo's <LuzCursor>2) gets a
 * session_t. While every stream of the display draws the pointer, Desktop Duplication's capture
 * leaves it out of the picture and publishes each shape it sees (publish_shape()) and what it
 * does with the pointer (publish_capture()). The control stream thread then sends each such stream
 * the shapes it lacks and where the pointer is (pump(), luzcursor.h's SHAPE and STATE), on their
 * own ENet channel, at least every cursor_shape_policy::control_interval. The capture draws the
 * pointer as before on a secure desktop (UAC, the lock screen), with Windows Graphics Capture, and
 * in a display group's slots past 0, and the STATE says so, so the client draws none.
 *
 * For Mac trackpad mode, where LuzLunar predicts the pointer from the moves it sends, the STATE has
 * the moves injected so far added up (count_move()), read with the position.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

// local includes
#include "cursor_shape_policy.h"
#include "luzcursor.h"

namespace fork_tweaks::client_cursor {
  /// A stream whose client draws the pointer, from its launch to its end. Shared by the stream's
  /// threads: input (the sums), HTTPS (draws) and the control stream (the rest).
  struct session_t {
    /// The client draws the pointer now; POST /actions/cursor {"draw"} changes it
    std::atomic<bool> draws {true};
    /// It just went back to drawing it: the control stream sends the STATE again, unchanged or not
    std::atomic<bool> resend {false};

    /// The relative moves injected for the stream, added up as each is (count_move())
    std::mutex moves_mutex;
    std::int64_t sum_dx = 0;
    std::int64_t sum_dy = 0;

    /// The control stream thread's: the client's shapes, the STATE last sent, the pointer last read
    luzcursor::sender_t sender;
    cursor_shape_policy::pacer_t pacer;
    cursor_shape_policy::pointer_t last_pointer;
    std::int64_t last_sum_dx = 0;
    std::int64_t last_sum_dy = 0;
    std::chrono::steady_clock::time_point next_read {};
    bool send_failed = false;
  };

  /// fork_client_cursor is on, and the platform can (Windows)
  bool enabled();

  /**
   * @brief For a stream launched with luzCursor=1 (draws_now) or luzCursor=host; null when it can't
   * have it: the option is off, it's a display group's slot past 0, or its control stream isn't
   * encrypted (the messages need moonlight-common-c's V2 headers).
   */
  std::shared_ptr<session_t> make_session(bool asked, bool draws_now, int slot, bool encrypted_control);

  /// POST /actions/cursor {"draw"}: the client draws the pointer from now on, or Apollo does
  void set_draws(session_t &session, bool client);

  /**
   * @brief input.cpp, for a relative move about to be injected: counts it, and the lock returned
   * is held while the move is injected, so no position is read between the two. An empty lock for
   * a stream without it.
   */
  std::unique_lock<std::mutex> count_move(session_t *session, int dx, int dy);

  /// The capture of the display with the pointer: a new shape from Desktop Duplication, converted;
  /// null when it couldn't be
  void publish_shape(std::shared_ptr<const luzcursor::shape_t> shape);

  /// That capture, at each snapshot: it leaves the pointer out of the picture, Desktop Duplication
  /// draws the pointer on the display, and Ctrl+Alt+Shift+N took it out of the stream
  void publish_capture(bool client_draws, bool pointer_visible, bool hidden_by_host);

  /// The input is on a secure desktop (UAC, the lock screen); Windows is asked at most every 250 ms
  bool secure_desktop();

  /**
   * @brief The control stream thread, for a stream whose client draws the pointer: sends the STATE
   * when it changed (cursor_shape_policy::pacer_t), and before it the shape when the client lacks
   * it. send(type, payload) sends one message, encrypted and reliable, on luzcursor::k_channel;
   * false when it couldn't.
   */
  void pump(session_t &session, std::chrono::steady_clock::time_point now, const std::function<bool(std::uint16_t, std::string_view)> &send);
}  // namespace fork_tweaks::client_cursor
