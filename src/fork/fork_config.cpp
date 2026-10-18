/**
 * @file src/fork/fork_config.cpp
 * @brief Fork-only sunshine.conf options.
 */
// standard includes
#include <algorithm>
#include <cctype>

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
  }  // namespace

  void apply_config(std::unordered_map<std::string, std::string> &vars) {
    bool_option(vars, "fork_encoder_probe_reuse", encoder_probe_reuse);
    bool_option(vars, "fork_clipboard_rich", clipboard_rich);
    bool_option(vars, "fork_web_ui_remote_first_setup", web_ui_remote_first_setup);
    bool_option(vars, "fork_virtual_display_app_extends", virtual_display_app_extends);
  }
}  // namespace fork_tweaks
