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

  /**
   * @brief Read the fork-only options and remove them from @p vars, so they aren't reported as unrecognized.
   * @param vars The parsed sunshine.conf options.
   */
  void apply_config(std::unordered_map<std::string, std::string> &vars);
}  // namespace fork_tweaks
