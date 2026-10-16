/**
 * @file src/fork/client_display.h
 * @brief The screen a LuzLunar client streams to, from its luz* launch arguments, and the description
 * of the virtual display built from it for the LuzVDA driver: size, gamut, HDR brightness and modes.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "fork/vdisplay_mode.h"

namespace client_display {
  enum class hdr_e {
    none,  ///< The screen can't show HDR
    unknown,  ///< HDR, brightness unknown
    known,  ///< HDR with the luminance below
  };

  /// What the client said about its screen, checked
  struct screen_t {
    uint32_t width_mm = 0;
    uint32_t height_mm = 0;
    uint32_t panel_width = 0;
    uint32_t panel_height = 0;
    uint32_t safe_width = 0;
    uint32_t safe_height = 0;
    uint32_t max_hz = 0;
    bool p3 = false;
    hdr_e hdr = hdr_e::none;
    uint32_t peak_nits = 0;
    uint32_t full_frame_nits = 0;
    /// 1/10000 nits
    uint32_t min_luminance = 0;
    /// macOS's potential EDR headroom x 100, for the log
    uint32_t edr_x100 = 0;
    std::string name;
  };

  /// The virtual display's description, in the driver's terms but without its header
  struct description_t {
    uint16_t width_mm = 0;
    uint16_t height_mm = 0;
    /// Red, green, blue and white x,y in 1/1024; all 0 means sRGB
    std::array<uint16_t, 8> primaries {};
    bool hdr = false;
    uint32_t max_luminance = 0;
    uint32_t max_full_frame_luminance = 0;
    uint32_t min_luminance = 0;
    std::vector<vdisplay_mode::display_mode_t> modes;
    std::size_t preferred = 0;
  };

  constexpr uint32_t min_dimension = 320;
  constexpr uint32_t max_dimension = 8192;
  constexpr uint32_t max_size_mm = 2000;
  constexpr uint32_t max_hz = 500;
  constexpr uint32_t max_nits = 10000;
  constexpr std::size_t max_name_bytes = 63;
  constexpr std::size_t max_modes = 32;

  /// Display P3 with a D65 white point
  constexpr std::array<uint16_t, 8> p3_primaries {696, 328, 271, 707, 154, 61, 320, 337};

  /// Clients without the arguments (Moonlight) get what SudoVDA's fixed EDID said: 70x39 cm, HDR
  /// at about 3,800 nits, so nothing changes for them but the mode list
  constexpr uint16_t default_width_mm = 700;
  constexpr uint16_t default_height_mm = 390;
  constexpr uint32_t default_nits = 3805;

  /// An HDR screen of unknown brightness: an ordinary HDR10 monitor's numbers, a guess
  constexpr uint32_t unknown_peak_nits = 1000;
  constexpr uint32_t unknown_full_frame_nits = 600;
  constexpr uint32_t unknown_min_luminance = 500;

  /// A launch argument, if the client sent it
  using lookup_t = std::function<std::optional<std::string>(std::string_view)>;

  inline std::optional<uint32_t> parse_uint(std::string_view text) {
    uint32_t value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc {} || end != text.data() + text.size()) {
      return std::nullopt;
    }
    return value;
  }

  /// "302x196"
  inline std::optional<std::pair<uint32_t, uint32_t>> parse_size(std::string_view text) {
    auto x = text.find('x');
    if (x == std::string_view::npos) {
      return std::nullopt;
    }
    auto w = parse_uint(text.substr(0, x));
    auto h = parse_uint(text.substr(x + 1));
    if (!w || !h) {
      return std::nullopt;
    }
    return std::pair {*w, *h};
  }

  inline bool dimension_ok(uint32_t w, uint32_t h) {
    return w >= min_dimension && w <= max_dimension && h >= min_dimension && h <= max_dimension;
  }

  /// UTF-8 cut at a character boundary
  inline std::string truncate_utf8(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
      return text;
    }
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
      end--;
    }
    text.resize(end);
    return text;
  }

  /**
   * @brief The client's screen, or nothing unless it sent luzDisplay=1 and a usable luzPanel.
   * Values out of range are dropped one by one rather than rejecting the rest.
   */
  inline std::optional<screen_t> parse(const lookup_t &arg) {
    if (arg("luzDisplay") != std::optional<std::string> {"1"}) {
      return std::nullopt;
    }

    screen_t s;

    auto panel = arg("luzPanel");
    auto panel_size = panel ? parse_size(*panel) : std::nullopt;
    if (!panel_size || !dimension_ok(panel_size->first, panel_size->second)) {
      return std::nullopt;
    }
    s.panel_width = s.safe_width = panel_size->first;
    s.panel_height = s.safe_height = panel_size->second;

    if (auto safe = arg("luzSafe")) {
      auto size = parse_size(*safe);
      if (size && dimension_ok(size->first, size->second) && size->first <= s.panel_width && size->second <= s.panel_height) {
        s.safe_width = size->first;
        s.safe_height = size->second;
      }
    }

    if (auto mm = arg("luzMm")) {
      auto size = parse_size(*mm);
      if (size && size->first > 0 && size->second > 0 && size->first <= max_size_mm && size->second <= max_size_mm) {
        s.width_mm = size->first;
        s.height_mm = size->second;
      }
    }

    if (auto hz = arg("luzHz")) {
      auto value = parse_uint(*hz);
      if (value && *value <= max_hz) {
        s.max_hz = *value;
      }
    }

    s.p3 = arg("luzGamut") == std::optional<std::string> {"p3"};

    if (auto hdr = arg("luzHdr")) {
      if (*hdr == "unknown") {
        s.hdr = hdr_e::unknown;
      } else if (*hdr != "none") {
        // peak,full,black
        std::string_view v = *hdr;
        auto c1 = v.find(',');
        auto c2 = c1 == std::string_view::npos ? c1 : v.find(',', c1 + 1);
        if (c2 != std::string_view::npos) {
          auto peak = parse_uint(v.substr(0, c1));
          auto full = parse_uint(v.substr(c1 + 1, c2 - c1 - 1));
          auto black = parse_uint(v.substr(c2 + 1));
          if (peak && full && black && *peak > 0 && *peak <= max_nits && *full <= *peak && (uint64_t) *black < (uint64_t) *peak * 10000) {
            s.hdr = hdr_e::known;
            s.peak_nits = *peak;
            s.full_frame_nits = *full ? *full : *peak;
            s.min_luminance = *black;
          } else {
            s.hdr = hdr_e::unknown;
          }
        } else {
          s.hdr = hdr_e::unknown;
        }
      }
    }

    if (auto edr = arg("luzEdr")) {
      s.edr_x100 = parse_uint(*edr).value_or(0);
    }

    if (auto name = arg("luzName")) {
      s.name = truncate_utf8(*name, max_name_bytes);
    }

    return s;
  }

  /**
   * @brief The virtual display for a stream: the client's screen if it described one, with the
   * stream's mode first, then the screen's other shapes, each at the stream's rate, the screen's
   * fastest and 60 Hz. A resume on the same screen at another shape or rate is then a mode change,
   * since the driver can't add modes to a display once Windows has it.
   * @param render The mode the stream renders at (vdisplay_mode::for_client()).
   */
  inline description_t describe(const std::optional<screen_t> &screen, const vdisplay_mode::display_mode_t &render) {
    description_t d;

    std::vector<std::pair<uint32_t, uint32_t>> shapes {{render.width, render.height}};
    std::vector<uint32_t> rates {render.refresh_mhz};

    if (!screen) {
      d.width_mm = default_width_mm;
      d.height_mm = default_height_mm;
      d.hdr = true;
      d.max_luminance = d.max_full_frame_luminance = default_nits;
    } else {
      d.width_mm = (uint16_t) screen->width_mm;
      d.height_mm = (uint16_t) screen->height_mm;
      if (screen->p3) {
        d.primaries = p3_primaries;
      }

      switch (screen->hdr) {
        case hdr_e::none:
          break;
        case hdr_e::unknown:
          d.hdr = true;
          d.max_luminance = unknown_peak_nits;
          d.max_full_frame_luminance = unknown_full_frame_nits;
          d.min_luminance = unknown_min_luminance;
          break;
        case hdr_e::known:
          d.hdr = true;
          d.max_luminance = screen->peak_nits;
          d.max_full_frame_luminance = screen->full_frame_nits;
          d.min_luminance = screen->min_luminance;
          break;
      }

      shapes.push_back({screen->panel_width, screen->panel_height});
      shapes.push_back({screen->safe_width, screen->safe_height});
      if (screen->max_hz) {
        rates.push_back(screen->max_hz * 1000);
      }
    }
    rates.push_back(60000);

    auto add = [&](uint32_t w, uint32_t h, uint32_t mhz) {
      vdisplay_mode::display_mode_t m {w, h, mhz};
      if (!dimension_ok(w, h) || d.modes.size() >= max_modes || std::find(d.modes.begin(), d.modes.end(), m) != d.modes.end()) {
        return;
      }
      d.modes.push_back(m);
    };

    for (auto [w, h] : shapes) {
      for (auto mhz : rates) {
        add(w, h, mhz);
      }
    }

    // A stream mode out of range was left out; the driver offers 1080p60 then
    if (d.modes.empty()) {
      d.modes.push_back({1920, 1080, 60000});
    }

    // The stream's mode when it's valid, as it comes first
    d.preferred = 0;

    return d;
  }
}  // namespace client_display
