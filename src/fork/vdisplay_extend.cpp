/**
 * @file src/fork/vdisplay_extend.cpp
 * @brief Fork-only: the Virtual Display app adds the client's screen next to the PC's displays,
 * which stay on, instead of in their place.
 */
// standard includes
#include <memory>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "vdisplay_extend.h"

#ifdef _WIN32
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
#endif

using namespace std::literals;

namespace fork_tweaks::vdisplay_extend {
#ifdef _WIN32
  namespace {
    display_device::WinDisplayDevice make_device() {
      return display_device::WinDisplayDevice {std::make_shared<display_device::WinApiLayer>()};
    }
  }  // namespace

  snapshot_t snapshot() {
    auto device = make_device();

    snapshot_t now;
    now.topology = device.getCurrentTopology();
    for (const auto &group : now.topology) {
      for (const auto &id : group) {
        if (now.primary.empty() && device.isPrimary(id)) {
          now.primary = id;
        }
      }
    }
    return now;
  }

  void restore(const snapshot_t &before, const std::string &virtual_id) {
    auto device = make_device();

    auto now = device.getCurrentTopology();
    if (auto want = extend_layout::extended(before.topology, now, virtual_id)) {
      BOOST_LOG(info) << "vdisplay: extending the desktop: the displays on were "sv << extend_layout::describe(before.topology)
                      << " before the virtual display, "sv << extend_layout::describe(now) << " after; setting "sv << extend_layout::describe(*want);
      if (!device.setTopology(*want)) {
        BOOST_LOG(warning) << "vdisplay: couldn't turn the PC's displays back on next to the virtual display"sv;
      }
    } else {
      BOOST_LOG(info) << "vdisplay: extending the desktop: "sv << extend_layout::describe(now)
                      << " are on, the PC's displays as before"sv;
    }

    if (!before.primary.empty() && !device.isPrimary(before.primary)) {
      if (device.setAsPrimary(before.primary)) {
        BOOST_LOG(info) << "vdisplay: made "sv << before.primary << " the primary display again"sv;
      } else {
        BOOST_LOG(warning) << "vdisplay: couldn't make "sv << before.primary << " the primary display again"sv;
      }
    }
  }
#endif

  config::video_t display_config(const config::video_t &video) {
    using enum config::video_t::dd_t::config_option_e;

    auto extended = video;
    if (extended.dd.configuration_option == ensure_only_display || extended.dd.configuration_option == ensure_primary) {
      extended.dd.configuration_option = ensure_active;
    }
    return extended;
  }
}  // namespace fork_tweaks::vdisplay_extend
