/**
 * @file src/fork/cursor_position.cpp
 * @brief Fork-only: the pointer on the display a stream shows, for LuzLunar (cursor_position.h).
 */
// standard includes
#include <mutex>
#include <string_view>

// local includes
#include "cursor_position.h"
#include "src/logging.h"
// As adaptive_bitrate.cpp has them: network.h's Boost.Asio before rtsp.h's windows.h, which would
// bring in WinSock 1 first
#include "src/network.h"
#include "src/rtsp.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::cursor_position {
  namespace {
    std::mutex display_mutex;
    display_t current_display;

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
    current_display = display;
  }

  bool supported() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
  }

  reply_t handle(const client_t &client, const std::string &method) {
    if (!client.permitted) {
      BOOST_LOG(debug) << "fork: cursor: client ["sv << client.name << "] may not view streams"sv;
      return error_reply(401, "permission");
    }
    if (!supported()) {
      return error_reply(404, "unsupported");
    }
    if (method != "GET") {
      return error_reply(400, "method");
    }
    // Only the client's own stream, by its certificate
    if (!rtsp_stream::find_session(client.uuid)) {
      return error_reply(403, "not_streaming");
    }

#ifdef _WIN32
    display_t display;
    {
      std::lock_guard lock {display_mutex};
      display = current_display;
    }
    auto position = read(display);
    if (!position) {
      return error_reply(503, "unavailable");
    }
    return {200, position_json(*position)};
#else
    return error_reply(404, "unsupported");
#endif
  }
}  // namespace fork_tweaks::cursor_position
