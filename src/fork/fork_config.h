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

  /**
   * @brief Read the fork-only options and remove them from @p vars, so they aren't reported as unrecognized.
   * @param vars The parsed sunshine.conf options.
   */
  void apply_config(std::unordered_map<std::string, std::string> &vars);
}  // namespace fork_tweaks
