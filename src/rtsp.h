/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <memory>
#include <list>
#include <optional>

// local includes
#include "crypto.h"
#include "fork/client_display.h"
#include "fork/display_group_policy.h"  // [fork]
#include "thread_safe.h"

#ifdef _WIN32
  #include <windows.h>
#endif

// Resolve circular dependencies
namespace stream {
  struct session_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    std::string device_name;
    std::string unique_id;
    std::string client_address;  ///< Normalized address of the client that launched; only it may connect to RTSP
    crypto::PERM perm;

    bool input_only;
    bool host_audio;
    int width;
    int height;
    int fps;
    int gcmap;
    int surround_info;
    std::string surround_params;
    bool enable_hdr;
    bool enable_sops;
    bool virtual_display;
    uint32_t scale_factor;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    std::list<crypto::command_entry_t> client_do_cmds;
    std::list<crypto::command_entry_t> client_undo_cmds;

    /// [fork] The screen a LuzLunar client streams to, for the virtual display; none from other clients
    std::optional<client_display::screen_t> client_screen;

    /// [fork] Stock Moonlight for iOS, by its User-Agent: its Touchscreen mode's fingers can become touch
    bool moonlight_ios = false;

    /// [fork] LuzLunar asked for its Mac trackpad mode (luzTrackpad=mac, fork/pointer_speed.h)
    bool mac_trackpad = false;

    /// [fork] LuzLunar asked for its screen to be Windows' primary display while the Virtual Display
    /// app extends the desktop (luzPrimary=1, fork/vdisplay_primary_policy.h)
    bool client_primary = false;

    /// [fork] LuzLunar's own bitrate in kbps (luzBitrate=N), which the encoder starts at: it launched a quarter above it
    /// to leave room to raise it (fork/adaptive_bitrate.h); 0 from other clients
    std::int64_t luz_bitrate_kbps = 0;

    /// [fork] "Desktop (multiple displays)" (fork/display_group.h): what the launch said about its
    /// group, and the slot this stream takes; -1 outside a group
    struct {
      display_group_policy::request_t request;
      int slot = -1;
    } fork_group;

  #ifdef _WIN32
    GUID display_guid{};
  #endif
  };

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /// [fork] A launch waits for its RTSP handshake; another would be dropped until it's done
  bool launch_session_pending();

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  std::shared_ptr<stream::session_t>
  find_session(const std::string_view& uuid);

  std::list<std::string>
  get_all_session_uuids();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
