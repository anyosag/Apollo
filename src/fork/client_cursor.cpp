/**
 * @file src/fork/client_cursor.cpp
 * @brief Fork-only: LuzLunar draws the PC's pointer itself (client_cursor.h).
 */
// standard includes
#include <optional>

// local includes
#include "client_cursor.h"
#include "cursor_capture.h"
#include "cursor_position.h"
#include "fork_config.h"
#include "src/logging.h"

#ifdef _WIN32
  // platform includes
  #include <wchar.h>
  #include <windows.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::client_cursor {
  namespace {
    using clock = std::chrono::steady_clock;

    /// What the capture of the display with the pointer last said
    struct published_t {
      std::shared_ptr<const luzcursor::shape_t> shape;
      bool client_draws = false;
      bool pointer_visible = false;
      bool hidden_by_host = false;
      clock::time_point at {};
    };

    std::mutex published_mutex;
    published_t published;

    std::mutex desktop_mutex;
    bool desktop_secure = false;
    clock::time_point desktop_checked {};

#ifdef _WIN32
    /**
     * @brief Where Windows has the pointer, on the display the capture shows. Windows only answers
     * on the desktop that has the input, so after a switch to or from a secure desktop this thread
     * moves to it, at most once a second.
     */
    std::optional<cursor_shape_policy::pointer_t> read_pointer(clock::time_point now) {
      static thread_local clock::time_point next_sync {};
      CURSORINFO info {};
      info.cbSize = sizeof(info);
      bool read = GetCursorInfo(&info);
      if (!read && now >= next_sync) {
        next_sync = now + 1s;
        platf::syncThreadDesktop();
        read = GetCursorInfo(&info);
      }
      auto display = cursor_position::current_display();
      if (!read || display.width <= 0 || display.height <= 0) {
        return std::nullopt;
      }

      // The display's offsets are from the virtual screen's corner, the pointer from the primary display's
      int screen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
      int screen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
      int left = screen_x + display.offset_x;
      int top = screen_y + display.offset_y;

      cursor_shape_policy::pointer_t pointer;
      pointer.known = true;
      pointer.x = info.ptScreenPos.x - left;
      pointer.y = info.ptScreenPos.y - top;
      pointer.width = display.width;
      pointer.height = display.height;
      pointer.showing = (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr;

      // Where Windows keeps it: the whole virtual screen (every display, a display group's too), or
      // the rectangle an app holds it in
      RECT screen {screen_x, screen_y, screen_x + GetSystemMetrics(SM_CXVIRTUALSCREEN), screen_y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
      RECT clip = screen;
      if (GetClipCursor(&clip)) {
        pointer.clipped = clip.left > screen.left || clip.top > screen.top || clip.right < screen.right || clip.bottom < screen.bottom;
      } else {
        clip = screen;
      }
      pointer.clip_left = clip.left - left;
      pointer.clip_top = clip.top - top;
      pointer.clip_right = clip.right - left;
      pointer.clip_bottom = clip.bottom - top;
      return pointer;
    }
#endif
  }  // namespace

  bool enabled() {
#ifdef _WIN32
    return client_drawn_cursor;
#else
    return false;
#endif
  }

  std::shared_ptr<session_t> make_session(bool asked, bool draws_now, int slot, bool encrypted_control) {
    if (!asked) {
      return nullptr;
    }
    if (!enabled()) {
      BOOST_LOG(info) << "fork: the client asked to draw the pointer, but fork_client_cursor is off: Apollo draws it"sv;
      return nullptr;
    }
    if (slot >= 1) {
      // Only slot 0 has the pointer's shape and input; the others' captures keep drawing it
      return nullptr;
    }
    if (!encrypted_control) {
      BOOST_LOG(info) << "fork: the client asked to draw the pointer, but its control stream isn't encrypted: Apollo draws it"sv;
      return nullptr;
    }
    BOOST_LOG(info) << (draws_now ? "fork: the client draws the pointer"sv : "fork: the client can draw the pointer; Apollo draws it until it asks"sv);
    auto session = std::make_shared<session_t>();
    session->draws.store(draws_now);
    return session;
  }

  void set_draws(session_t &session, bool client) {
    if (client) {
      // Whatever it was told last, the client hears it again
      session.resend.store(true);
    }
    session.draws.store(client);
  }

  std::unique_lock<std::mutex> count_move(session_t *session, int dx, int dy) {
    if (session == nullptr) {
      return {};
    }
    std::unique_lock lock {session->moves_mutex};
    session->sum_dx += dx;
    session->sum_dy += dy;
    return lock;
  }

  void publish_shape(std::shared_ptr<const luzcursor::shape_t> shape) {
    std::lock_guard lock {published_mutex};
    published.shape = std::move(shape);
  }

  void capture_shape_t::tell() {
    if (told_) {
      return;
    }
    told_ = true;
    auto shape = cursor_shape_policy::convert(type_, data_.data(), data_.size(), width_, height_, pitch_, hotspot_x_, hotspot_y_);
    if (!shape && usable_) {
      BOOST_LOG(info) << "fork: a pointer shape the client can't draw (type "sv << type_ << ", "sv << width_ << 'x' << height_ << "): Apollo draws it"sv;
    }
    usable_ = shape.has_value();
    publish_shape(shape ? std::make_shared<const luzcursor::shape_t>(std::move(*shape)) : nullptr);
  }

  void publish_capture(bool client_draws, bool pointer_visible, bool hidden_by_host) {
    auto now = clock::now();
    std::lock_guard lock {published_mutex};
    published.client_draws = client_draws;
    published.pointer_visible = pointer_visible;
    published.hidden_by_host = hidden_by_host;
    published.at = now;
  }

  bool secure_desktop() {
#ifdef _WIN32
    auto now = clock::now();
    std::lock_guard lock {desktop_mutex};
    if (desktop_checked != clock::time_point {} && now - desktop_checked < 250ms) {
      return desktop_secure;
    }
    desktop_checked = now;

    // The input desktop's name: "Default" normally, "Winlogon" for UAC and the lock screen. One it
    // can't open counts as secure, so the capture draws the pointer.
    bool secure = true;
    if (HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)) {
      wchar_t name[64] {};
      DWORD needed = 0;
      if (GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &needed)) {
        secure = _wcsicmp(name, L"Default") != 0;
      }
      CloseDesktop(desktop);
    }
    if (secure != desktop_secure) {
      BOOST_LOG(info) << (secure ? "fork: a secure desktop has the input: Apollo draws the pointer there"sv : "fork: the input is back on the desktop: the client draws the pointer again"sv);
      desktop_secure = secure;
    }
    return desktop_secure;
#else
    return false;
#endif
  }

  void pump(session_t &session, std::chrono::steady_clock::time_point now, const std::function<bool(std::uint16_t, std::string_view)> &send) {
    if (session.send_failed) {
      return;
    }
    if (session.resend.exchange(false)) {
      session.pacer = {};
    }

    published_t seen;
    {
      std::lock_guard lock {published_mutex};
      seen = published;
    }
    cursor_shape_policy::capture_t capture;
    capture.fresh = seen.at != clock::time_point {} && now - seen.at < cursor_shape_policy::capture_stale;
    capture.client_draws = seen.client_draws;
    capture.pointer_visible = seen.pointer_visible;
    capture.hidden_by_host = seen.hidden_by_host;
    capture.hash = seen.shape ? seen.shape->hash : 0;

#ifdef _WIN32
    if (now >= session.next_read) {
      // With the sums as they are now: a move is counted and injected under the same lock
      std::lock_guard lock {session.moves_mutex};
      if (auto pointer = read_pointer(now)) {
        session.last_pointer = *pointer;
        session.last_sum_dx = session.sum_dx;
        session.last_sum_dy = session.sum_dy;
      } else {
        // The last place it was known stands until Windows answers again
        session.next_read = now + 250ms;
      }
    }
#endif

    auto state = cursor_shape_policy::state_for(capture, session.last_pointer, session.last_sum_dx, session.last_sum_dy);
    if (!session.pacer.due(state, now)) {
      return;
    }
    for (auto &message : session.sender.messages(seen.shape.get(), state)) {
      if (!send(message.type, message.payload)) {
        // The client can't be kept up to date any more: the capture draws the pointer again
        BOOST_LOG(warning) << "fork: couldn't send the pointer to the client: Apollo draws it"sv;
        session.send_failed = true;
        session.draws.store(false);
        return;
      }
    }
    session.pacer.sent(state, now);
  }
}  // namespace fork_tweaks::client_cursor
