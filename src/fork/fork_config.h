/**
 * @file src/fork/fork_config.h
 * @brief Fork-only sunshine.conf options.
 */
#pragma once

// standard includes
#include <string>
#include <unordered_map>

namespace fork_tweaks {
  /// `fork_encoder_probe_reuse`: reuse the last encoder probe when only the display layout changed.
  inline bool encoder_probe_reuse = true;

  /// `fork_clipboard_rich`: rich clipboard sync for LuzLunar (`/actions/clipboard?type=bundle`). Off behaves like stock Apollo (text only).
  inline bool clipboard_rich = true;

  /// `fork_clipboard_files`: with rich sync, files copied in Explorer or LuzLunar's Finder cross too
  /// (clipboard_files.h), for clients with the File Download and File Upload permissions.
  inline bool clipboard_file_copies = true;

  /// `fork_web_ui_remote_first_setup`: allow setting the first Web UI password from the network (the
  /// addresses `origin_web_ui_allowed` allows), not only from this PC.
  inline bool web_ui_remote_first_setup = false;

  /// `fork_virtual_display_app_extends`: the Virtual Display app adds the client's screen next to the
  /// PC's displays, which stay on, instead of in their place. Other apps keep the configured display
  /// configuration (e.g. `ensure_only_display`).
  inline bool virtual_display_app_extends = true;

  /// `fork_ios_touch`: stock Moonlight for iOS in Touchscreen mode: fingers reach Windows as touch, not
  /// as mouse clicks (touch_emulation.h).
  inline bool ios_touch = true;

  /// `fork_ios_tablet_posture`: Windows in tablet posture while such a client touches (tablet_posture.h).
  inline bool ios_tablet_posture = true;

  /// `fork_ios_touch_keyboard`: Windows' touch keyboard when such a client taps a text field (touch_keyboard.h).
  inline bool ios_touch_keyboard = true;

  /// `fork_touchpad_passthrough`: offer LuzLunar's trackpad passthrough, a synthetic Windows touchpad
  /// fed with the Mac trackpad's fingers (src/fork/touchpad_win.*). Off, the trackpad stays a mouse.
  inline bool touchpad_passthrough = true;

  /// `fork_trackpad_mac`: also offer LuzLunar's Mac trackpad mode, where macOS points, clicks and
  /// scrolls and the touchpad only gets gestures; Windows doesn't accelerate the pointer while it
  /// streams (pointer_speed.h). Off, such a client gets the Windows touchpad.
  inline bool trackpad_mac = true;

  /// `fork_auto_update`: install the fork's new master builds by themselves while nobody streams
  /// (src/fork/updater.*). Off, they download and wait for Install in the Web UI or LuzLunar.
  inline bool auto_update = false;

  /// `fork_mac_drive`: on LAN streams, LuzLunar can have its Mac's disk mapped as a network drive in
  /// the signed-in user's session, until shortly after the stream ends (mac_drive.h).
  inline bool mac_network_drive = true;

  /// `fork_host_sleep`: LuzLunar can put this PC to sleep, for clients with the Sleep PC permission,
  /// quitting an app that's still running first; never while anything streams (host_sleep.h).
  inline bool client_sleep = true;

  /// `fork_adaptive_bitrate`: LuzLunar can lower its stream's bitrate while it runs, and raise it back
  /// to the bitrate it launched with (adaptive_bitrate.h). NVENC changes it without a keyframe; other
  /// encoders refuse.
  inline bool live_bitrate = true;

  /// `fork_vdisplay_recovery`: bring the virtual display back when Windows switches it off or the
  /// driver loses it mid-stream, and put capture back on it (vdisplay_recovery.h).
  inline bool vdisplay_recovery_enabled = true;

  /// `fork_nv_stream_settings`: from a stream's launch until its app quits, NVIDIA's driver caps games
  /// at the stream's frame rate with V-Sync off, and gives Smooth Motion to apps that ask for it
  /// (nv_stream.h).
  inline bool nv_stream_settings = true;

  /// `fork_vdisplay_refresh_multiplier` (1, 2 or 4): the virtual display refreshes this many times
  /// per stream frame; 4 only while games are capped, else 2 (nv_stream_policy::refresh_multiplier()).
  /// Replaces `double_refreshrate`.
  inline int vdisplay_refresh_multiplier = 4;

  /// `fork_smooth_motion_half_cap`: a Smooth Motion app is capped at half the stream's rate rather
  /// than the stream's, for a driver that caps the frames the game renders rather than those it shows.
  inline bool smooth_motion_half_cap = false;

  /// `fork_display_group`: the "Desktop (multiple displays)" app for LuzLunar: one virtual display
  /// per Mac display, arranged like the Mac's, each streamed by its own session, with the PC's
  /// displays off while it runs (display_group.h).
  inline bool display_group_enabled = true;

  /// `fork_pair_from_client`: a client with the "Pair other devices" permission can give Apollo the
  /// PIN of another device waiting to pair at a given address (POST /actions/pairdevice), as
  /// LuzLunar does for the iMac's Moonlight.
  inline bool pair_from_client = true;

  /// `fork_client_cursor`: LuzLunar can draw the PC's pointer itself, in its Windows shape, with none
  /// of the stream's latency, when it asks (luzCursor=1, client_cursor.h). Off, Apollo always draws it.
  inline bool client_drawn_cursor = true;

  /**
   * @brief Read the fork-only options and remove them from @p vars, so they aren't reported as unrecognized.
   * @param vars The parsed sunshine.conf options.
   */
  void apply_config(std::unordered_map<std::string, std::string> &vars);
}  // namespace fork_tweaks
