/**
 * @file src/fork/fork_config.cpp
 * @brief Fork-only sunshine.conf options.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>
#include <initializer_list>

// local includes
#include "fork_config.h"
#include "src/logging.h"

using namespace std::literals;

namespace fork_tweaks {
  namespace {
    void bool_option(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &target) {
      auto it = vars.find(name);
      if (it == vars.end()) {
        return;
      }

      auto value = it->second;
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return std::tolower(c);
      });
      if (value == "enabled" || value == "enable" || value == "true" || value == "on" || value == "yes" || value == "1") {
        target = true;
      } else if (value == "disabled" || value == "disable" || value == "false" || value == "off" || value == "no" || value == "0") {
        target = false;
      } else {
        BOOST_LOG(warning) << "config: ignoring invalid value ["sv << it->second << "] for "sv << name;
      }
      vars.erase(it);
    }

    void choice_option(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::initializer_list<int> allowed, int &target) {
      auto it = vars.find(name);
      if (it == vars.end()) {
        return;
      }

      int value = 0;
      auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
      if (ec == std::errc {} && end == it->second.data() + it->second.size() && std::find(allowed.begin(), allowed.end(), value) != allowed.end()) {
        target = value;
      } else {
        BOOST_LOG(warning) << "config: ignoring invalid value ["sv << it->second << "] for "sv << name;
      }
      vars.erase(it);
    }
  }  // namespace

  void apply_config(std::unordered_map<std::string, std::string> &vars) {
    bool_option(vars, "fork_encoder_probe_reuse", encoder_probe_reuse);
    bool_option(vars, "fork_clipboard_rich", clipboard_rich);
    bool_option(vars, "fork_clipboard_files", clipboard_file_copies);
    bool_option(vars, "fork_web_ui_remote_first_setup", web_ui_remote_first_setup);
    bool_option(vars, "fork_virtual_display_app_extends", virtual_display_app_extends);
    bool_option(vars, "fork_ios_touch", ios_touch);
    bool_option(vars, "fork_ios_tablet_posture", ios_tablet_posture);
    bool_option(vars, "fork_ios_touch_keyboard", ios_touch_keyboard);
    bool_option(vars, "fork_touchpad_passthrough", touchpad_passthrough);
    bool_option(vars, "fork_trackpad_mac", trackpad_mac);
    bool_option(vars, "fork_auto_update", auto_update);
    bool_option(vars, "fork_mac_drive", mac_network_drive);
    bool_option(vars, "fork_host_sleep", client_sleep);
    bool_option(vars, "fork_adaptive_bitrate", live_bitrate);
    bool_option(vars, "fork_vdisplay_recovery", vdisplay_recovery_enabled);
    bool_option(vars, "fork_nv_stream_settings", nv_stream_settings);
    choice_option(vars, "fork_vdisplay_refresh_multiplier", {1, 2, 4}, vdisplay_refresh_multiplier);
    bool_option(vars, "fork_smooth_motion_half_cap", smooth_motion_half_cap);
    bool_option(vars, "fork_display_group", display_group_enabled);
    bool_option(vars, "fork_pair_from_client", pair_from_client);
  }
}  // namespace fork_tweaks
