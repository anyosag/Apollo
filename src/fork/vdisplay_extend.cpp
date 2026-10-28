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
#include "vdisplay_primary_policy.h"

#ifdef _WIN32
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>

  #include "src/platform/windows/utils.h"
#endif

using namespace std::literals;

namespace fork_tweaks::vdisplay_extend {
#ifdef _WIN32
  namespace {
    display_device::WinDisplayDevice make_device() {
      return display_device::WinDisplayDevice {std::make_shared<display_device::WinApiLayer>()};
    }

    std::string primary_of(display_device::WinDisplayDevice &device, const extend_layout::topology_t &topology) {
      for (const auto &group : topology) {
        for (const auto &id : group) {
          if (device.isPrimary(id)) {
            return id;
          }
        }
      }
      return {};
    }
  }  // namespace

  snapshot_t snapshot() {
    auto device = make_device();

    snapshot_t now;
    now.topology = device.getCurrentTopology();
    now.primary = primary_of(device, now.topology);
    return now;
  }

  std::string primary() {
    auto device = make_device();
    return primary_of(device, device.getCurrentTopology());
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
  }

  primary_t settle_primary(const std::string &pc_primary, const std::string &virtual_id, bool client_primary) {
    if (!client_primary && pc_primary.empty()) {
      return {};
    }

    auto device = make_device();
    auto primary_now = primary_of(device, device.getCurrentTopology());
    auto target = vdisplay_primary_policy::target(client_primary, pc_primary, virtual_id, primary_now);
    if (!target) {
      if (client_primary && !virtual_id.empty()) {
        BOOST_LOG(info) << "vdisplay: "sv << virtual_id << " is the primary display already, as the client asks"sv;
      } else if (client_primary) {
        // display_device doesn't list the virtual display yet; the recovery monitor tries again once it does
        BOOST_LOG(warning) << "vdisplay: display_device doesn't list the virtual display, so it can't be made the primary display yet"sv;
        return {true, primary_now};
      }
      return {};
    }

    // Windows doesn't take display changes while the session is locked, as when the user signs in
    // through the stream
    if (client_primary && is_changing_settings_going_to_fail()) {
      BOOST_LOG(info) << "vdisplay: Windows doesn't take display changes now (a locked session?); "sv << *target
                      << " becomes the primary display once it does"sv;
      return {true, primary_now};
    }

    if (device.setAsPrimary(*target)) {
      if (client_primary) {
        BOOST_LOG(info) << "vdisplay: made "sv << *target << " the primary display, as the client asks"sv;
      } else {
        BOOST_LOG(info) << "vdisplay: made "sv << *target << " the primary display again"sv;
      }
      return {};
    }

    if (client_primary) {
      BOOST_LOG(warning) << "vdisplay: couldn't make "sv << *target << " the primary display; trying again"sv;
      return {true, primary_now};
    }
    BOOST_LOG(warning) << "vdisplay: couldn't make "sv << *target << " the primary display again"sv;
    return {};
  }

  bool switch_on(const std::string &virtual_id) {
    auto device = make_device();

    auto now = device.getCurrentTopology();
    auto want = extend_layout::extended(now, now, virtual_id);
    if (!want) {
      return true;
    }
    BOOST_LOG(info) << "vdisplay: switching the virtual display on again: "sv << extend_layout::describe(now) << " are on, setting "sv << extend_layout::describe(*want);
    if (!device.setTopology(*want)) {
      BOOST_LOG(warning) << "vdisplay: Windows didn't switch the virtual display on again"sv;
      return false;
    }
    return true;
  }
#else
  bool switch_on(const std::string &) {
    return false;
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
