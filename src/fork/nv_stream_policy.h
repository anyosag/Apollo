/**
 * @file src/fork/nv_stream_policy.h
 * @brief NVIDIA driver settings while a client streams, apart from NVAPI: which global settings a
 * stream gets, what is remembered to put them back, what putting them back does to a setting that
 * changed meanwhile, the journal that survives a crash, and how fast the virtual display refreshes.
 *
 * While someone streams, the driver caps every game at the stream's frame rate (Max Frame Rate)
 * and forces V-Sync off, so a game renders the frames the stream sends, and the virtual display can
 * refresh at 4x the stream's rate without games rendering 4x the frames. An app can opt out of both
 * (`fork-nv-cap`), and opt in to Smooth Motion, the driver's frame generation
 * (`fork-smooth-motion`). The global profile's own values are remembered first, in memory and in a
 * journal, and put back when the stream ends, or at Apollo's next start after a crash. A value
 * someone changed during the stream is theirs and stays.
 *
 * After Vibepollo's NVIDIA Control Panel frame limiter (github.com/Nonary/Vibepollo, GPLv3).
 *
 * Header-only, without Boost, Windows or NVAPI, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace nv_stream_policy {
  /// Max Frame Rate (FRL_FPS_ID in NvApiDriverSettings.h): frames per second, 0 for off
  constexpr uint32_t frame_rate_limit_id = 0x10835002;
  constexpr uint32_t frame_rate_limit_max = 1023;

  /// Vertical sync (VSYNCMODE_ID) and its Force off value (VSYNCMODE_FORCEOFF)
  constexpr uint32_t vsync_id = 0x00A879CF;
  constexpr uint32_t vsync_force_off = 0x08416747;

  /// Smooth Motion and the graphics APIs it applies to; neither is in the open-source SDK
  constexpr uint32_t smooth_motion_id = 0xB0D384C0;
  constexpr uint32_t smooth_motion_on = 1;
  constexpr uint32_t smooth_motion_apis_id = 0xB0CC0875;
  constexpr uint32_t smooth_motion_apis_all = 7;  ///< DirectX 12, DirectX 11 and Vulkan

  /// Smooth Motion came with driver 571.86 (NvAPI_SYS_GetDriverAndBranchVersion's 57186)
  constexpr uint32_t smooth_motion_min_driver = 57186;

  /// What an app asks for, from its `fork-nv-cap` and `fork-smooth-motion` in apps.json
  struct app_t {
    bool cap = true;  ///< The frame cap at the stream's rate, with V-Sync off
    bool smooth_motion = false;
  };

  /// A checkbox's value as the Web UI may save it: a boolean, 1 or 0, or a word like "true" or "on"
  inline std::optional<bool> flag(const nlohmann::json &value) {
    if (value.is_boolean()) {
      return value.get<bool>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
      auto n = value.get<int64_t>();
      return n == 0 || n == 1 ? std::optional<bool>(n == 1) : std::nullopt;
    }
    if (!value.is_string()) {
      return std::nullopt;
    }
    std::string s = value.get<std::string>();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return (char) std::tolower(c);
    });
    for (auto [yes, no] : {std::pair {"true", "false"}, {"1", "0"}, {"enabled", "disabled"}, {"enable", "disable"}, {"yes", "no"}, {"on", "off"}}) {
      if (s == yes || s == no) {
        return s == yes;
      }
    }
    return std::nullopt;
  }

  /// An app's choices from its apps.json entry; missing or unreadable ones keep their defaults
  inline app_t app_from_json(const nlohmann::json &app) {
    app_t choice;
    if (!app.is_object()) {
      return choice;
    }
    if (auto it = app.find("fork-nv-cap"); it != app.end()) {
      choice.cap = flag(*it).value_or(choice.cap);
    }
    if (auto it = app.find("fork-smooth-motion"); it != app.end()) {
      choice.smooth_motion = flag(*it).value_or(choice.smooth_motion);
    }
    return choice;
  }

  /// `fork_smooth_motion_cap`: the cap for a Smooth Motion app, as the stream's rate or half of it
  /// (if the driver caps the frames it shows, generated ones included, the stream's rate leaves the
  /// game half of it; if it caps the frames the game renders, half does)
  enum class smooth_motion_cap_e {
    stream,
    half,
  };

  /// A setting and the value a stream gives it
  struct setting_t {
    uint32_t id;
    uint32_t value;

    bool operator==(const setting_t &) const = default;
  };

  /**
   * @brief The frame cap for a stream, in whole frames per second.
   * @param stream_mhz The stream's rate in millihertz; 59.94 Hz caps at 60.
   */
  inline uint32_t frame_cap(uint32_t stream_mhz, bool smooth_motion, smooth_motion_cap_e smooth_motion_cap) {
    uint32_t fps = (stream_mhz + 500) / 1000;
    if (smooth_motion && smooth_motion_cap == smooth_motion_cap_e::half) {
      fps = (fps + 1) / 2;
    }
    return std::clamp<uint32_t>(fps, 1, frame_rate_limit_max);
  }

  /**
   * @brief The settings a stream gets, in the order they're written.
   * @param stream_mhz The stream's rate in millihertz.
   * @param driver_version NvAPI's version, 57186 for 571.86; 0 if unknown, which rules out Smooth Motion.
   * @param note Set to why Smooth Motion was left out, if it was.
   */
  inline std::vector<setting_t> for_stream(const app_t &app, uint32_t stream_mhz, uint32_t driver_version, smooth_motion_cap_e smooth_motion_cap, std::string *note = nullptr) {
    std::vector<setting_t> want;

    bool smooth_motion = app.smooth_motion;
    if (smooth_motion && driver_version < smooth_motion_min_driver) {
      smooth_motion = false;
      if (note) {
        *note = driver_version ? "Smooth Motion needs driver 571.86 or newer" : "the driver's version is unknown, so Smooth Motion is left off";
      }
    }

    if (app.cap && stream_mhz) {
      want.push_back({frame_rate_limit_id, frame_cap(stream_mhz, smooth_motion, smooth_motion_cap)});
      want.push_back({vsync_id, vsync_force_off});
    }
    if (smooth_motion) {
      want.push_back({smooth_motion_id, smooth_motion_on});
      want.push_back({smooth_motion_apis_id, smooth_motion_apis_all});
    }
    return want;
  }

  /// A setting as the profile has it
  struct reading_t {
    bool own;  ///< The profile itself sets it, rather than the driver's default for it
    uint32_t value;  ///< The value in effect; 0 when the driver has none
  };

  /// What the journal remembers of a setting a stream changed
  struct entry_t {
    uint32_t id;
    uint32_t ours;  ///< The value the stream gave it
    std::optional<uint32_t> before;  ///< The profile's own value before; none for the driver's default

    bool operator==(const entry_t &) const = default;
  };

  /**
   * @brief What to remember before a stream changes a setting.
   *
   * An explicit Off for Smooth Motion, or no APIs for it, counts as the driver's default, as
   * Vibepollo found such values left behind by tools that didn't put them back (its "poisoned"
   * state); putting the default back leaves Smooth Motion off all the same.
   */
  inline entry_t remember(uint32_t id, uint32_t ours, const reading_t &before) {
    entry_t entry {id, ours, std::nullopt};
    bool poisoned = (id == smooth_motion_id || id == smooth_motion_apis_id) && before.value == 0;
    if (before.own && !poisoned) {
      entry.before = before.value;
    }
    return entry;
  }

  /// What putting a setting back does
  enum class restore_e {
    leave,  ///< It changed since the stream set it, so whoever changed it keeps their value
    set,  ///< The profile gets its own value back
    clear,  ///< The profile's value goes, and the driver's default applies again
  };

  inline restore_e restore_action(const entry_t &entry, const reading_t &now) {
    if (!now.own || now.value != entry.ours) {
      return restore_e::leave;
    }
    return entry.before ? restore_e::set : restore_e::clear;
  }

  /// Something to do to the profile
  struct action_t {
    enum class kind_e {
      write,  ///< Set `value`
      set,  ///< Put back the profile's own `value`
      clear,  ///< Put back the driver's default
    };

    kind_e kind;
    uint32_t id;
    uint32_t value;

    bool operator==(const action_t &) const = default;
  };

  /// The profile's changes for a stream, and the journal once they're saved
  struct update_t {
    std::vector<action_t> actions;
    std::vector<entry_t> entries;  ///< The journal after the actions
    bool unreadable = false;  ///< A setting couldn't be read, so it was left as it is
  };

  inline const entry_t *find(const std::vector<entry_t> &entries, uint32_t id) {
    auto it = std::find_if(entries.begin(), entries.end(), [id](const entry_t &e) {
      return e.id == id;
    });
    return it == entries.end() ? nullptr : &*it;
  }

  /**
   * @brief How to go from the settings a stream has (the journal) to the ones it wants: settings it
   * no longer wants are put back, new ones remembered first, and changed ones written again with
   * their first `before` kept. A setting that already has the wanted value is left alone and not
   * remembered, so it's never put back either. With nothing wanted, everything is put back.
   * @param read The profile's reading of a setting, or none if NVAPI couldn't read it.
   */
  template<typename Read>
  update_t update(const std::vector<entry_t> &journal, const std::vector<setting_t> &want, Read &&read) {
    update_t u;

    for (const auto &entry : journal) {
      bool wanted = std::any_of(want.begin(), want.end(), [&](const setting_t &s) {
        return s.id == entry.id;
      });
      if (wanted) {
        continue;
      }
      std::optional<reading_t> now = read(entry.id);
      if (!now) {
        u.unreadable = true;
        u.entries.push_back(entry);
        continue;
      }
      switch (restore_action(entry, *now)) {
        case restore_e::leave:
          break;
        case restore_e::set:
          u.actions.push_back({action_t::kind_e::set, entry.id, *entry.before});
          break;
        case restore_e::clear:
          u.actions.push_back({action_t::kind_e::clear, entry.id, 0});
          break;
      }
    }

    for (const auto &s : want) {
      std::optional<reading_t> now = read(s.id);
      if (!now) {
        u.unreadable = true;
        if (auto entry = find(journal, s.id)) {
          u.entries.push_back(*entry);
        }
        continue;
      }

      auto entry = find(journal, s.id);
      if (entry) {
        u.entries.push_back({s.id, s.value, entry->before});
      } else if (now->value != s.value) {
        u.entries.push_back(remember(s.id, s.value, *now));
      }
      if (now->value != s.value) {
        u.actions.push_back({action_t::kind_e::write, s.id, s.value});
      }
    }

    return u;
  }

  /**
   * @brief The journal while the actions are being saved: the entries before and after together,
   * so a crash, or a save that fails, still knows every value to put back. A setting the update
   * gives another value of ours is there twice, with each value: whichever the profile ends up
   * with is put back, and the other is left as someone else's.
   */
  inline std::vector<entry_t> while_saving(const std::vector<entry_t> &before, const update_t &u) {
    std::vector<entry_t> entries = u.entries;
    for (const auto &entry : before) {
      bool kept = std::any_of(entries.begin(), entries.end(), [&](const entry_t &e) {
        return e.id == entry.id && e.ours == entry.ours;
      });
      if (!kept) {
        entries.push_back(entry);
      }
    }
    return entries;
  }

  /**
   * @brief The journal after an update whose write of `id` failed: the profile still has what it
   * had, so that setting's entry is the one from before the update, or none.
   */
  inline std::vector<entry_t> write_failed(std::vector<entry_t> entries, const std::vector<entry_t> &before, uint32_t id) {
    std::erase_if(entries, [id](const entry_t &e) {
      return e.id == id;
    });
    for (const auto &entry : before) {
      if (entry.id == id) {
        entries.push_back(entry);
      }
    }
    return entries;
  }

  /// The profile a journal's settings are in: the current global one, or the base profile when the
  /// driver has no current global one
  enum class profile_e {
    global,
    base,
  };

  struct journal_t {
    profile_e profile = profile_e::global;
    std::vector<entry_t> entries;

    bool operator==(const journal_t &) const = default;
  };

  inline std::string to_json(const journal_t &journal) {
    nlohmann::json settings = nlohmann::json::array();
    for (const auto &entry : journal.entries) {
      nlohmann::json item {{"id", entry.id}, {"ours", entry.ours}};
      item["before"] = entry.before ? nlohmann::json(*entry.before) : nlohmann::json(nullptr);
      settings.push_back(std::move(item));
    }
    nlohmann::json json {
      {"version", 1},
      {"profile", journal.profile == profile_e::global ? "global" : "base"},
      {"settings", std::move(settings)},
    };
    return json.dump();
  }

  /// A journal's text, or none if it isn't one this version wrote
  inline std::optional<journal_t> from_json(std::string_view text) {
    auto json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (!json.is_object() || json.value("version", 0) != 1) {
      return std::nullopt;
    }

    auto read_u32 = [](const nlohmann::json &value) -> std::optional<uint32_t> {
      if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX) {
        return std::nullopt;
      }
      return value.get<uint32_t>();
    };

    journal_t journal;
    auto profile = json.find("profile");
    if (profile == json.end() || !profile->is_string()) {
      return std::nullopt;
    }
    if (*profile == "global") {
      journal.profile = profile_e::global;
    } else if (*profile == "base") {
      journal.profile = profile_e::base;
    } else {
      return std::nullopt;
    }

    auto settings = json.find("settings");
    if (settings == json.end() || !settings->is_array()) {
      return std::nullopt;
    }
    for (const auto &item : *settings) {
      if (!item.is_object() || !item.contains("id") || !item.contains("ours")) {
        return std::nullopt;
      }
      auto id = read_u32(item["id"]);
      auto ours = read_u32(item["ours"]);
      if (!id || !ours) {
        return std::nullopt;
      }
      entry_t entry {*id, *ours, std::nullopt};
      if (item.contains("before") && !item["before"].is_null()) {
        entry.before = read_u32(item["before"]);
        if (!entry.before) {
          return std::nullopt;
        }
      }
      journal.entries.push_back(entry);
    }
    return journal;
  }

  /**
   * @brief The virtual display's refresh rate as a multiple of the stream's, from
   * `fork_vdisplay_refresh_multiplier` (1, 2 or 4).
   *
   * Windows hands capture a frame at a display refresh, so a game frame that just misses one waits
   * for the next: up to 8.3 ms at 120 Hz for a 120 fps stream, 4.2 ms at 2x and 2.1 ms at 4x. The
   * extra refreshes cost only desktop composition, as the stream still encodes and sends its own
   * rate. But an uncapped game would render at 4x too, so 4x needs the frame cap and is 2x without.
   * vdisplay_mode::for_client() then keeps it within what the driver takes.
   */
  inline int refresh_multiplier(int configured, bool capped) {
    int multiplier = configured >= 4 ? 4 : configured >= 2 ? 2 : 1;
    return multiplier == 4 && !capped ? 2 : multiplier;
  }

  /// Whether `for_stream()` asked for a frame cap
  inline bool wants_cap(const std::vector<setting_t> &want) {
    return std::any_of(want.begin(), want.end(), [](const setting_t &s) {
      return s.id == frame_rate_limit_id;
    });
  }
}  // namespace nv_stream_policy
