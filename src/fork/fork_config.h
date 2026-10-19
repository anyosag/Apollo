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

  /// `fork_touchpad_passthrough`: offer LuzLunar's trackpad passthrough, a synthetic Windows touchpad
  /// fed with the Mac trackpad's fingers (src/fork/touchpad_win.*). Off, the trackpad stays a mouse.
  inline bool touchpad_passthrough = true;

  /// `fork_auto_update`: install the fork's new master builds by themselves while nobody streams
  /// (src/fork/updater.*). Off, they download and wait for Install in the Web UI or LuzLunar.
  inline bool auto_update = false;

  /**
   * @brief Read the fork-only options and remove them from @p vars, so they aren't reported as unrecognized.
   * @param vars The parsed sunshine.conf options.
   */
  void apply_config(std::unordered_map<std::string, std::string> &vars);
}  // namespace fork_tweaks
