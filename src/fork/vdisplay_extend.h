/**
 * @file src/fork/vdisplay_extend.h
 * @brief Fork-only: the Virtual Display app adds the client's screen next to the PC's displays,
 * which stay on, instead of in their place.
 */
#pragma once

// standard includes
#include <string>

// local includes
#include "extend_layout.h"

namespace config {
  struct video_t;
}

namespace fork_tweaks::vdisplay_extend {
  /// The PC's active displays at one moment
  struct snapshot_t {
    extend_layout::topology_t topology;
    std::string primary;  ///< The primary display's device id, or empty
  };

  /// The active displays now (Windows only)
  snapshot_t snapshot();

  /**
   * @brief Turns the displays of @p before back on if Windows switched them off when the virtual
   * display arrived, shows the virtual display next to them, and keeps their primary display
   * (Windows only).
   * @param before The displays before the virtual display was added.
   * @param virtual_id The virtual display's device id, or empty if display_device doesn't list it.
   */
  void restore(const snapshot_t &before, const std::string &virtual_id);

  /**
   * @brief The display configuration for an extended desktop: the configured one, except that
   * `ensure_only_display` and `ensure_primary` become `ensure_active`, so the other displays stay
   * on and the primary display stays where it is.
   */
  config::video_t display_config(const config::video_t &video);
}  // namespace fork_tweaks::vdisplay_extend
