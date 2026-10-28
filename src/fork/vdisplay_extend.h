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

  /// The primary display's device id now, or empty if it couldn't be read (Windows only)
  std::string primary();

  /**
   * @brief Turns the displays of @p before back on if Windows switched them off when the virtual
   * display arrived, and shows the virtual display next to them (Windows only). settle_primary()
   * then decides the primary display.
   * @param before The displays before the virtual display was added.
   * @param virtual_id The virtual display's device id, or empty if display_device doesn't list it.
   */
  void restore(const snapshot_t &before, const std::string &virtual_id);

  /// What settle_primary() left to do
  struct primary_t {
    bool pending = false;  ///< The client's screen should be primary, but Windows didn't take it: try again (vdisplay_primary_policy::retry_t)
    std::string displaced;  ///< The primary display then, or empty
  };

  /**
   * @brief Makes the client's screen the primary display when it asks (`luzPrimary=1`), or else
   * makes the PC's primary display of before primary again if the virtual display's arrival changed
   * that (Windows only). Windows saves either for this set of displays.
   * @param pc_primary The primary display before the virtual display arrived; empty at a resume,
   * which doesn't hand it back.
   * @param virtual_id The virtual display's device id, or empty if display_device doesn't list it.
   * @param client_primary The client asks for its screen to be the primary display.
   */
  primary_t settle_primary(const std::string &pc_primary, const std::string &virtual_id, bool client_primary);

  /**
   * @brief Switches the virtual display on again next to the displays that are on, switching none
   * of them off, for the recovery monitor (vdisplay_recovery.h) when Windows switched it off
   * mid-stream (Windows only).
   * @param virtual_id The virtual display's device id.
   * @return Whether it's on, or Windows took the change.
   */
  bool switch_on(const std::string &virtual_id);

  /**
   * @brief The display configuration for an extended desktop: the configured one, except that
   * `ensure_only_display` and `ensure_primary` become `ensure_active`, so the other displays stay
   * on and the primary display is settle_primary()'s.
   */
  config::video_t display_config(const config::video_t &video);
}  // namespace fork_tweaks::vdisplay_extend
