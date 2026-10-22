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

  /**
   * @brief Read the fork-only options and remove them from @p vars, so they aren't reported as unrecognized.
   * @param vars The parsed sunshine.conf options.
   */
  void apply_config(std::unordered_map<std::string, std::string> &vars);
}  // namespace fork_tweaks
