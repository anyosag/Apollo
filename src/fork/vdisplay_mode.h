/**
 * @file src/fork/vdisplay_mode.h
 * @brief The mode Apollo gives a virtual display: the client's size times the scale factor, at the
 * client's frame rate in millihertz.
 *
 * Header-only, without Boost, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <cstdint>
#include <cstdlib>

namespace vdisplay_mode {
  struct display_mode_t {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;

    bool operator==(const display_mode_t &) const = default;
  };

  /// Scale factors in percent outside this range are treated as 100, so a bad client value can't
  /// ask for an empty or enormous display
  constexpr int min_scale = 20;
  constexpr int max_scale = 400;

  /**
   * @brief The scale factor in effect: the app's if it sets one, else the client's.
   */
  inline int effective_scale(int64_t session_scale, int app_scale) {
    int64_t scale = app_scale != 100 ? app_scale : session_scale;
    return scale >= min_scale && scale <= max_scale ? (int) scale : 100;
  }

  /**
   * @brief The virtual display's mode for a client.
   * @param client_width, client_height The size the client asked for; 0 means 1920x1080.
   * @param client_fps The frame rate the client asked for, in millihertz; 0 or less means 60 Hz,
   * and values under 1000 are taken as hertz.
   * @param scale The scale factor in percent, from `effective_scale()`.
   * @param double_refresh Apollo's `double_refreshrate` option.
   */
  inline display_mode_t for_client(uint32_t client_width, uint32_t client_height, int client_fps, int scale, bool double_refresh) {
    uint32_t width = client_width ? client_width : 1920;
    uint32_t height = client_height ? client_height : 1080;

    if (scale != 100) {
      width *= ((float) scale / 100);
      height *= ((float) scale / 100);

      // Chop the last bit to ensure the scaled resolution is even numbered
      // Most odd resolutions won't work well
      width &= ~1u;
      height &= ~1u;
    }

    int fps = client_fps > 0 ? client_fps : 60000;
    if (fps < 1000) {
      fps *= 1000;
    }
    if (double_refresh) {
      fps *= 2;
    }

    return {width, height, (uint32_t) fps};
  }

  /**
   * @brief Whether a display already runs `want`: the same size, and a refresh rate within half a
   * hertz, since Windows reports the rate as a fraction that may not round-trip exactly.
   */
  inline bool runs(const display_mode_t &want, int width, int height, int refresh_mhz) {
    return (int64_t) want.width == width && (int64_t) want.height == height &&
           std::llabs((int64_t) want.refresh_mhz - refresh_mhz) <= 500;
  }
}  // namespace vdisplay_mode
