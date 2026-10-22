/**
 * @file src/fork/vdisplay_mode.h
 * @brief The mode Apollo gives a virtual display: the client's size times the scale factor, at a
 * multiple of the client's frame rate in millihertz.
 *
 * Header-only, without Boost, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace vdisplay_mode {
  struct display_mode_t {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;

    bool operator==(const display_mode_t &) const = default;
  };

  /// LuzVDA refuses a display with any mode faster than this
  constexpr uint32_t max_hz = 500;
  /// The driver refuses a display with any mode faster than this: 7680x4320 at 240 Hz, with room
  constexpr uint64_t max_pixels_per_second = 8000000000ull;

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
   * @brief The client's frame rate in millihertz.
   * @param client_fps 0 or less means 60 Hz, and values under 1000 are taken as hertz.
   */
  inline uint32_t stream_mhz(int client_fps) {
    int fps = client_fps > 0 ? client_fps : 60000;
    return (uint32_t) (fps < 1000 ? fps * 1000 : fps);
  }

  /// Whether the driver takes a mode this size at this rate
  inline bool within_limits(uint32_t width, uint32_t height, uint64_t refresh_mhz) {
    return refresh_mhz <= max_hz * 1000ull && (uint64_t) width * height * refresh_mhz / 1000 <= max_pixels_per_second;
  }

  /**
   * @brief The virtual display's mode for a client.
   * @param client_width, client_height The size the client asked for; 0 means 1920x1080.
   * @param client_fps The frame rate the client asked for, in millihertz (see `stream_mhz()`).
   * @param scale The scale factor in percent, from `effective_scale()`.
   * @param multiplier The display refreshes this many times per stream frame (1, 2 or 4), or half
   * as many, or a quarter, if the driver doesn't take that; see nv_stream_policy::refresh_multiplier().
   */
  inline display_mode_t for_client(uint32_t client_width, uint32_t client_height, int client_fps, int scale, int multiplier) {
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

    uint32_t fps = stream_mhz(client_fps);
    if (multiplier < 1) {
      multiplier = 1;
    }
    while (multiplier > 1 && !within_limits(width, height, (uint64_t) fps * multiplier)) {
      multiplier /= 2;
    }

    return {width, height, fps * multiplier};
  }

  /**
   * @brief Slower rates for a display Windows won't run at `mode`'s, fastest first: twice the
   * stream's rate, then the stream's, as long as they're slower than `mode`'s.
   * @param stream The stream's rate in millihertz (`stream_mhz()`).
   */
  inline std::vector<uint32_t> slower_rates(const display_mode_t &mode, uint32_t stream) {
    std::vector<uint32_t> rates;
    if (!stream) {
      return rates;
    }
    for (uint64_t rate : {(uint64_t) stream * 2, (uint64_t) stream}) {
      if (rate < mode.refresh_mhz) {
        rates.push_back((uint32_t) rate);
      }
    }
    return rates;
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
