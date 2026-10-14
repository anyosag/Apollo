/**
 * @file src/fork/app_encoder_overrides.h
 * @brief Fork-only: per-app encoder settings, read from an app's "encoder-overrides" object in apps.json.
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks {
  /**
   * @brief Apply the "encoder-overrides" of the app with this UUID until restore_app_encoder_overrides().
   * @param app_uuid UUID of the app being launched.
   *
   * Keys and values are the same as in sunshine.conf, for example:
   * @code{.json}
   * "encoder-overrides": { "nvenc_preset": 6, "nvenc_twopass": "full_res", "nvenc_latency_over_power": "disabled" }
   * @endcode
   * Does nothing if the app has no overrides.
   */
  void apply_app_encoder_overrides(const std::string &app_uuid);

  /**
   * @brief Put back the global encoder settings changed by apply_app_encoder_overrides(), if any.
   */
  void restore_app_encoder_overrides();
}  // namespace fork_tweaks
