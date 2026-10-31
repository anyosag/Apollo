/**
 * @file src/fork/cursor_position.cpp
 * @brief Fork-only: the pointer on the display a stream shows, for LuzLunar (cursor_position.h).
 */
// standard includes
#include <mutex>
#include <string_view>

// local includes
#include "client_cursor.h"
#include "cursor_position.h"
#include "cursor_pulse.h"
#include "cursor_shape_policy.h"
#include "src/logging.h"
// As adaptive_bitrate.cpp has them: network.h's Boost.Asio before rtsp.h's windows.h, which would
// bring in WinSock 1 first
#include "src/network.h"
#include "src/rtsp.h"
#include "src/stream.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::cursor_position {
  namespace {
    std::mutex display_mutex;
    display_t shown_display;

    reply_t error_reply(int status, std::string_view error) {
      return {status, nlohmann::json {{"error", std::string {error}}}.dump()};
    }

#ifdef _WIN32
    std::optional<CURSORINFO> cursor_info() {
      CURSORINFO info {};
      info.cbSize = sizeof(info);
      if (GetCursorInfo(&info)) {
        return info;
      }
      return std::nullopt;
    }

    std::optional<position_t> read(const display_t &display) {
      auto info = cursor_info();
      if (!info) {
        // Windows says only on the desktop that has the input, and a secure desktop (a UAC prompt,
        // the lock screen) may have taken it since this thread last asked
        platf::syncThreadDesktop();
        info = cursor_info();
      }
      if (!info) {
        return std::nullopt;
      }
      // The display's offsets are from the virtual screen's corner, the pointer from the primary display's
      return on_display(display, info->ptScreenPos.x - GetSystemMetrics(SM_XVIRTUALSCREEN), info->ptScreenPos.y - GetSystemMetrics(SM_YVIRTUALSCREEN), (info->flags & CURSOR_SHOWING) != 0);
    }
#endif
  }  // namespace

  void set_display(const display_t &display) {
    std::lock_guard lock {display_mutex};
    shown_display = display;
  }

  display_t current_display() {
    std::lock_guard lock {display_mutex};
    return shown_display;
  }

  bool supported() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
  }

  reply_t handle(const client_t &client, const std::string &method, const std::string &body) {
    if (!client.permitted) {
      BOOST_LOG(debug) << "fork: cursor: client ["sv << client.name << "] may not view streams"sv;
      return error_reply(401, "permission");
    }
    if (!supported()) {
      return error_reply(404, "unsupported");
    }
    if (method != "GET" && method != "POST") {
      return error_reply(400, "method");
    }
    std::optional<cursor_pulse::request_t> pulse;
    std::optional<bool> draw;  // POST {"draw"}: who draws the pointer (client_cursor.h)
    if (method == "POST") {
      if (cursor_shape_policy::is_draw_request(body)) {
        draw = cursor_shape_policy::parse_draw(body);
        if (!draw) {
          return error_reply(400, "bad_request");
        }
      } else {
        pulse = cursor_pulse::parse_request(body);
        if (!pulse) {
          return error_reply(400, "bad_request");
        }
      }
    }
    // Only the client's own stream, by its certificate
    auto stream = rtsp_stream::find_session(client.uuid);
    if (!stream) {
      return error_reply(403, "not_streaming");
    }
    auto drawn = stream::session::fork_cursor(*stream);
    if (draw) {
      if (!client_cursor::enabled()) {
        return error_reply(409, "disabled");
      }
      if (!drawn) {
        // Launched without luzCursor=1, or a display group's slot past 0
        return error_reply(409, "not_launched");
      }
      if (drawn->draws.load() != *draw) {
        BOOST_LOG(info) << "fork: client ["sv << client.name << (*draw ? "] draws the pointer"sv : "] has Apollo draw the pointer"sv);
      }
      client_cursor::set_draws(*drawn, *draw);
      return {200, cursor_shape_policy::draw_json(*draw)};
    }
    // The client draws the pointer itself, so it pulses it too
    bool client_draws = drawn && drawn->draws.load();
    if (pulse && pulse->pulses == 0 && !client_draws) {
      // Settling back needs no position
      cursor_pulse::start(*pulse);
    }

#ifdef _WIN32
    display_t display;
    {
      std::lock_guard lock {display_mutex};
      display = shown_display;
    }
    auto position = read(display);
    if (!position) {
      return error_reply(503, "unavailable");
    }
    bool can_pulse = cursor_pulse::capable() && !client_draws;
    bool pulsing = false;
    if (pulse && pulse->pulses > 0 && can_pulse && position->shown && (!pulse->if_at_top || at_top(*position))) {
      cursor_pulse::start(*pulse);
      pulsing = true;
    }
    return {200, position_json(*position, can_pulse, pulsing)};
#else
    return error_reply(404, "unsupported");
#endif
  }
}  // namespace fork_tweaks::cursor_position
