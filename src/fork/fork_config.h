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

  /// `fork_touchpad_passthrough`: offer LuzLunar's trackpad passthrough, a synthetic Windows touchpad
  /// fed with the Mac trackpad's fingers (src/fork/touchpad_win.*). Off, the trackpad stays a mouse.
  inline bool touchpad_passthrough = true;

  /**
   * @brief Read the fork-only options and remove them from @p vars, so they aren't reported as unrecognized.
   * @param vars The parsed sunshine.conf options.
   */
  void apply_config(std::unordered_map<std::string, std::string> &vars);
}  // namespace fork_tweaks
