/**
 * @file src/fork/luzvda_desc.h
 * @brief The requests for the LuzVDA driver built from a client_display::description_t: the display
 * to add, and the modes to give a display it already has.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own. The protocol header is
 * MIT and copied byte for byte from the driver release (third-party/luzvda/).
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// lib includes
#include <luzvda/luzvda-ioctl.h>

// local includes
#include "client_display.h"

namespace luzvda_desc {
  /// A GUID's 16 bytes, as they lie in memory
  using guid_bytes_t = std::array<uint8_t, 16>;

  inline void copy_modes(const client_display::description_t &d, uint32_t &count, uint32_t &preferred, LUZVDA_MODE *modes) {
    count = (uint32_t) std::min<std::size_t>(d.modes.size(), LUZVDA_MAX_MODES);
    preferred = d.preferred < count ? (uint32_t) d.preferred : 0;
    for (uint32_t i = 0; i < count; ++i) {
      modes[i] = {d.modes[i].width, d.modes[i].height, d.modes[i].refresh_mhz};
    }
  }

  /**
   * @brief The display to add.
   * @param name The name Windows shows; cut to fit, at a character boundary.
   */
  inline LUZVDA_DISPLAY_DESC display(const client_display::description_t &d, const std::string &name, const guid_bytes_t &guid) {
    LUZVDA_DISPLAY_DESC out {};
    out.struct_size = sizeof(out);
    out.flags = d.hdr ? LUZVDA_DISPLAY_HDR : 0;
    std::memcpy(out.monitor_guid.bytes, guid.data(), sizeof(out.monitor_guid.bytes));

    // The name ends at its first NUL, which the array must hold
    auto fitted = client_display::truncate_utf8(name.substr(0, name.find('\0')), sizeof(out.name) - 1);
    std::memcpy(out.name, fitted.data(), fitted.size());

    out.width_mm = d.width_mm;
    out.height_mm = d.height_mm;
    std::copy(d.primaries.begin(), d.primaries.end(), out.primaries);
    if (d.hdr) {
      out.max_luminance = d.max_luminance;
      out.max_full_frame_luminance = d.max_full_frame_luminance;
      out.min_luminance = d.min_luminance;
    }

    copy_modes(d, out.mode_count, out.preferred_mode, out.modes);
    return out;
  }

  /// New modes for a display that is already there
  inline LUZVDA_UPDATE_MODES update(const client_display::description_t &d, const guid_bytes_t &guid) {
    LUZVDA_UPDATE_MODES out {};
    out.struct_size = sizeof(out);
    std::memcpy(out.monitor_guid.bytes, guid.data(), sizeof(out.monitor_guid.bytes));
    copy_modes(d, out.mode_count, out.preferred_mode, out.modes);
    return out;
  }

  /**
   * @brief Whether two displays are the same screen to Windows: everything that goes into the
   * EDID. Displays that differ only in modes can keep their monitor; any other change needs a new
   * one, since Windows reads the EDID only when a monitor arrives.
   */
  inline bool same_screen(const LUZVDA_DISPLAY_DESC &a, const LUZVDA_DISPLAY_DESC &b) {
    return a.flags == b.flags &&
           std::memcmp(a.monitor_guid.bytes, b.monitor_guid.bytes, sizeof(a.monitor_guid.bytes)) == 0 &&
           std::memcmp(a.name, b.name, sizeof(a.name)) == 0 &&
           a.width_mm == b.width_mm && a.height_mm == b.height_mm &&
           std::memcmp(a.primaries, b.primaries, sizeof(a.primaries)) == 0 &&
           a.max_luminance == b.max_luminance && a.max_full_frame_luminance == b.max_full_frame_luminance &&
           a.min_luminance == b.min_luminance;
  }

  /// Whether the display already offers the mode
  inline bool offers(const LUZVDA_DISPLAY_DESC &d, const vdisplay_mode::display_mode_t &m) {
    for (uint32_t i = 0; i < d.mode_count && i < LUZVDA_MAX_MODES; ++i) {
      if (d.modes[i].width == m.width && d.modes[i].height == m.height && d.modes[i].refresh_mhz == m.refresh_mhz) {
        return true;
      }
    }
    return false;
  }
}  // namespace luzvda_desc
