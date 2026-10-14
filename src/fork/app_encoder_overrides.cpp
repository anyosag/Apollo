/**
 * @file src/fork/app_encoder_overrides.cpp
 * @brief Fork-only: per-app encoder settings, read from an app's "encoder-overrides" object in apps.json.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <sstream>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "app_encoder_overrides.h"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging.h"

using namespace std::literals;

namespace fork_tweaks {
  namespace {
    // The settings an app may override. All of them are read when a stream starts, not only at startup.
    struct saved_settings_t {
      nvenc::nvenc_config nv;
      bool nv_realtime_hags;
      bool nv_sunshine_high_power_mode;
      double minimum_fps_target;
    };

    std::mutex mutex;
    std::optional<saved_settings_t> saved;

    std::string to_lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
      });
      return s;
    }

    // The JSON value as sunshine.conf would spell it: 6 -> "6", true -> "enabled".
    std::string conf_value(const nlohmann::json &value) {
      if (value.is_string()) {
        return to_lower(value.get<std::string>());
      }
      if (value.is_boolean()) {
        return value.get<bool>() ? "enabled" : "disabled";
      }
      return value.dump();
    }

    std::optional<bool> parse_bool(const std::string &v) {
      if (v == "enabled" || v == "enable" || v == "true" || v == "on" || v == "yes" || v == "1") {
        return true;
      }
      if (v == "disabled" || v == "disable" || v == "false" || v == "off" || v == "no" || v == "0") {
        return false;
      }
      return std::nullopt;
    }

    std::optional<double> parse_number(const std::string &v, double min, double max) {
      try {
        std::size_t used = 0;
        double n = std::stod(v, &used);
        if (used == v.size() && n >= min && n <= max) {
          return n;
        }
      } catch (const std::exception &) {
      }
      return std::nullopt;
    }

    std::optional<int> parse_int(const std::string &v, int min, int max) {
      auto n = parse_number(v, min, max);
      if (!n || *n != static_cast<int>(*n)) {
        return std::nullopt;
      }
      return static_cast<int>(*n);
    }

    // Applies one override to config::video. Returns false if the key or value isn't supported.
    bool apply_one(const std::string &key, const std::string &v) {
      auto &video = config::video;

      if (key == "nvenc_preset") {
        auto n = parse_int(v, 1, 7);
        if (!n) {
          return false;
        }
        video.nv.quality_preset = *n;
      } else if (key == "nvenc_twopass") {
        if (v == "disabled") {
          video.nv.two_pass = nvenc::nvenc_two_pass::disabled;
        } else if (v == "quarter_res") {
          video.nv.two_pass = nvenc::nvenc_two_pass::quarter_resolution;
        } else if (v == "full_res") {
          video.nv.two_pass = nvenc::nvenc_two_pass::full_resolution;
        } else {
          return false;
        }
      } else if (key == "nvenc_vbv_increase") {
        auto n = parse_int(v, 0, 400);
        if (!n) {
          return false;
        }
        video.nv.vbv_percentage_increase = *n;
      } else if (key == "nvenc_spatial_aq" || key == "nvenc_realtime_hags" || key == "nvenc_latency_over_power") {
        auto b = parse_bool(v);
        if (!b) {
          return false;
        }
        bool &target = key == "nvenc_spatial_aq"    ? video.nv.adaptive_quantization :
                       key == "nvenc_realtime_hags" ? video.nv_realtime_hags :
                                                      video.nv_sunshine_high_power_mode;
        target = *b;
      } else if (key == "minimum_fps_target") {
        auto n = parse_number(v, 0.0, 1000.0);
        if (!n) {
          return false;
        }
        video.minimum_fps_target = *n;
      } else {
        return false;
      }
      return true;
    }

    void restore_locked() {
      if (!saved) {
        return;
      }
      config::video.nv = saved->nv;
      config::video.nv_realtime_hags = saved->nv_realtime_hags;
      config::video.nv_sunshine_high_power_mode = saved->nv_sunshine_high_power_mode;
      config::video.minimum_fps_target = saved->minimum_fps_target;
      saved.reset();
      BOOST_LOG(info) << "fork: restored global encoder settings"sv;
    }
  }  // namespace

  void apply_app_encoder_overrides(const std::string &app_uuid) {
    std::lock_guard lg {mutex};

    // terminate() normally restores before every launch; this only guards against a missed restore.
    restore_locked();

    if (app_uuid.empty()) {
      return;
    }

    nlohmann::json overrides;
    std::string app_name;
    try {
      auto tree = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      for (const auto &app : tree.at("apps")) {
        if (app.is_object() && app.value("uuid", "") == app_uuid) {
          app_name = app.value("name", "");
          if (app.contains("encoder-overrides")) {
            overrides = app["encoder-overrides"];
          }
          break;
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "fork: couldn't read encoder-overrides from apps.json: "sv << e.what();
      return;
    }

    if (overrides.is_null()) {
      return;
    }
    if (!overrides.is_object()) {
      BOOST_LOG(warning) << "fork: encoder-overrides for app ["sv << app_name << "] must be an object, ignoring it"sv;
      return;
    }

    saved = saved_settings_t {
      config::video.nv,
      config::video.nv_realtime_hags,
      config::video.nv_sunshine_high_power_mode,
      config::video.minimum_fps_target,
    };

    std::stringstream applied;
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
      auto value = conf_value(it.value());
      if (apply_one(it.key(), value)) {
        applied << ' ' << it.key() << '=' << value;
      } else {
        BOOST_LOG(warning) << "fork: ignoring unsupported encoder-overrides entry ["sv << it.key() << " = "sv << value << "] for app ["sv << app_name << ']';
      }
    }
    BOOST_LOG(info) << "fork: encoder overrides for app ["sv << app_name << "]:"sv << applied.str();
  }

  void restore_app_encoder_overrides() {
    std::lock_guard lg {mutex};
    restore_locked();
  }
}  // namespace fork_tweaks
