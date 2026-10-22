/**
 * @file src/fork/nv_stream.cpp
 * @brief Fork-only: NVIDIA's frame cap, V-Sync off and Smooth Motion while a client streams, in the
 * driver's global profile, put back when the stream ends (nv_stream_policy.h).
 *
 * The profile is the driver's current global one, or its base profile when it has none, as in
 * Vibepollo (github.com/Nonary/Vibepollo, GPLv3). Each call loads NVAPI, opens a settings session
 * and closes both again, so a driver update between streams does no harm.
 */
// standard includes
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

// local includes
#include "fork_config.h"
#include "nv_stream.h"
#include "nv_stream_policy.h"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging.h"

#ifdef _WIN32
  // clang-format off
  #include <windows.h>
  #include <nvapi.h>
  #include <NvApiDriverSettings.h>
  // clang-format on

  #include "src/platform/windows/nvprefs/undo_file.h"
#endif

using namespace std::literals;

namespace fork_tweaks::nv_stream {
  namespace policy = nv_stream_policy;

  std::mutex &nvapi_mutex() {
    static std::mutex m;
    return m;
  }

#ifdef _WIN32
  namespace {
    static_assert(policy::frame_rate_limit_id == FRL_FPS_ID && policy::frame_rate_limit_max == FRL_FPS_MAX);
    static_assert(policy::vsync_id == VSYNCMODE_ID && policy::vsync_force_off == VSYNCMODE_FORCEOFF);

    /// apply(), restore() and recover() one at a time
    std::mutex state_mutex;

    /// What the stream changed, as in the journal; none when nothing is changed
    std::optional<policy::journal_t> active;

    // NVAPI's interfaces this module looks up itself: the open-source wrapper has some only under
    // their older ids, and drivers answer settings it doesn't know (Smooth Motion's) through the
    // newer ones. The ids and the newer-first order are Vibepollo's.
    using query_interface_fn = void *(__cdecl *) (NvU32);
    using driver_version_fn = NvAPI_Status(__cdecl *)(NvU32 *, NvAPI_ShortString);
    using global_profile_fn = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle *);
    using get_setting_fn = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING *, NvU32 *);
    using set_setting_fn = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING *, NvU32, NvU32);
    using profile_setting_fn = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32);

    constexpr NvU32 driver_version_id = 0x2926AAAD;
    constexpr NvU32 global_profile_id = 0x617BFF9F;
    constexpr NvU32 get_setting_ids[] {0xEA99498D, 0x73BF8338};
    constexpr NvU32 set_setting_ids[] {0x8A2CF5F5, 0x577DD202};
    constexpr NvU32 restore_default_ids[] {0x7DD5B261, 0x53F0381E};
    constexpr NvU32 delete_setting_ids[] {0xD20D29DF, 0xE4A26362};

    std::string hex(uint32_t value) {
      char text[16];
      std::snprintf(text, sizeof(text), "0x%08X", (unsigned) value);
      return text;
    }

    std::string nvapi_error(NvAPI_Status status) {
      NvAPI_ShortString message = {};
      NvAPI_GetErrorMessage(status, message);
      return message[0] ? std::string(message) : "status "s + std::to_string((int) status);
    }

    std::string name(uint32_t id) {
      switch (id) {
        case policy::frame_rate_limit_id:
          return "Max Frame Rate";
        case policy::vsync_id:
          return "V-Sync";
        case policy::smooth_motion_id:
          return "Smooth Motion";
        case policy::smooth_motion_apis_id:
          return "Smooth Motion's APIs";
        default:
          return "setting "s + hex(id);
      }
    }

    std::string value_text(uint32_t id, uint32_t value) {
      if (id == policy::frame_rate_limit_id) {
        return value ? std::to_string(value) + " fps" : "off"s;
      }
      if (id == policy::vsync_id && value == policy::vsync_force_off) {
        return "forced off"s;
      }
      if (id == policy::smooth_motion_id) {
        return value ? "on"s : "off"s;
      }
      return hex(value);
    }

    /// NVAPI loaded with a settings session, for one call
    class drs_t {
    public:
      drs_t(const drs_t &) = delete;
      drs_t &operator=(const drs_t &) = delete;
      drs_t() = default;

      ~drs_t() {
        if (session) {
          NvAPI_DRS_DestroySession(session);
        }
        if (loaded) {
          NvAPI_Unload();
        }
      }

      /// False without an NVIDIA driver, quietly
      bool open() {
        if (NvAPI_Initialize() != NVAPI_OK) {
          return false;
        }
        loaded = true;

        HMODULE dll = GetModuleHandleW(L"nvapi64.dll");
        auto query = dll ? reinterpret_cast<query_interface_fn>(reinterpret_cast<void *>(GetProcAddress(dll, "nvapi_QueryInterface"))) : nullptr;
        if (!query) {
          BOOST_LOG(warning) << "fork: NVIDIA: nvapi64.dll has no nvapi_QueryInterface"sv;
          return false;
        }
        auto lookup = [&]<typename F, std::size_t N>(F &fn, const NvU32 (&ids)[N]) {
          for (auto id : ids) {
            if ((fn = reinterpret_cast<F>(query(id)))) {
              return;
            }
          }
        };
        driver_version = reinterpret_cast<driver_version_fn>(query(driver_version_id));
        global_profile = reinterpret_cast<global_profile_fn>(query(global_profile_id));
        lookup(get_setting, get_setting_ids);
        lookup(set_setting, set_setting_ids);
        lookup(restore_default, restore_default_ids);
        lookup(delete_setting, delete_setting_ids);
        if (!get_setting || !set_setting || (!restore_default && !delete_setting)) {
          BOOST_LOG(warning) << "fork: NVIDIA: the driver doesn't offer the settings calls Apollo needs"sv;
          return false;
        }

        auto status = NvAPI_DRS_CreateSession(&session);
        if (status != NVAPI_OK) {
          session = nullptr;
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't open the driver's settings: "sv << nvapi_error(status);
          return false;
        }
        status = NvAPI_DRS_LoadSettings(session);
        if (status != NVAPI_OK) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't load the driver's settings: "sv << nvapi_error(status);
          return false;
        }
        return true;
      }

      /// The driver's version as NvAPI gives it, 57186 for 571.86; 0 if it won't say
      uint32_t version() {
        NvU32 v = 0;
        NvAPI_ShortString branch = {};
        return driver_version && driver_version(&v, branch) == NVAPI_OK ? v : 0;
      }

      /// The profile to change: the current global one if it answers, else the base profile
      NvDRSProfileHandle profile(policy::profile_e kind) {
        NvDRSProfileHandle p = nullptr;
        if (kind == policy::profile_e::global) {
          if (!global_profile || global_profile(session, &p) != NVAPI_OK || !p) {
            return nullptr;
          }
          NVDRS_SETTING probe {};
          probe.version = NVDRS_SETTING_VER;
          NvU32 extra = 0;
          auto status = get_setting(session, p, FRL_FPS_ID, &probe, &extra);
          return status == NVAPI_OK || status == NVAPI_SETTING_NOT_FOUND ? p : nullptr;
        }
        return NvAPI_DRS_GetBaseProfile(session, &p) == NVAPI_OK ? p : nullptr;
      }

      std::optional<policy::reading_t> read(NvDRSProfileHandle profile, policy::profile_e kind, uint32_t id) {
        NVDRS_SETTING s {};
        s.version = NVDRS_SETTING_VER;
        NvU32 extra = 0;
        auto status = get_setting(session, profile, id, &s, &extra);
        if (status == NVAPI_SETTING_NOT_FOUND) {
          return policy::reading_t {false, 0};
        }
        if (status != NVAPI_OK) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't read "sv << name(id) << ": "sv << nvapi_error(status);
          return std::nullopt;
        }
        bool here = s.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION ||
                    s.settingLocation == (kind == policy::profile_e::global ? NVDRS_GLOBAL_PROFILE_LOCATION : NVDRS_BASE_PROFILE_LOCATION);
        return policy::reading_t {here && !s.isCurrentPredefined, s.u32CurrentValue};
      }

      bool write(NvDRSProfileHandle profile, uint32_t id, uint32_t value) {
        NVDRS_SETTING s {};
        s.version = NVDRS_SETTING_VER;
        s.settingId = id;
        s.settingType = NVDRS_DWORD_TYPE;
        s.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
        s.u32CurrentValue = value;
        auto status = set_setting(session, profile, &s, 0, 0);
        if (status != NVAPI_OK) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't set "sv << name(id) << ": "sv << nvapi_error(status);
          return false;
        }
        return true;
      }

      /// The profile's own value goes
      bool remove(NvDRSProfileHandle profile, uint32_t id) {
        auto status = delete_setting ? delete_setting(session, profile, id) : NVAPI_NOT_SUPPORTED;
        if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't remove "sv << name(id) << " from the profile: "sv << nvapi_error(status);
          return false;
        }
        return true;
      }

      /// Back to the driver's default: the profile's predefined value, or none
      bool clear(NvDRSProfileHandle profile, uint32_t id) {
        auto status = restore_default ? restore_default(session, profile, id) : NVAPI_NOT_SUPPORTED;
        if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND && delete_setting) {
          status = delete_setting(session, profile, id);
        }
        if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't put back the default "sv << name(id) << ": "sv << nvapi_error(status);
          return false;
        }
        return true;
      }

      bool save() {
        auto status = NvAPI_DRS_SaveSettings(session);
        if (status != NVAPI_OK) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't save the driver's settings: "sv << nvapi_error(status);
          return false;
        }
        return true;
      }

    private:
      bool loaded = false;
      NvDRSSessionHandle session = nullptr;
      driver_version_fn driver_version = nullptr;
      global_profile_fn global_profile = nullptr;
      get_setting_fn get_setting = nullptr;
      set_setting_fn set_setting = nullptr;
      profile_setting_fn restore_default = nullptr;
      profile_setting_fn delete_setting = nullptr;
    };

    std::optional<std::filesystem::path> journal_folder() {
      wchar_t program_data[MAX_PATH];
      auto length = GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
      if (length == 0 || length >= MAX_PATH) {
        return std::nullopt;
      }
      // nvprefs' undo file lives there too
      return std::filesystem::path(program_data) / L"Sunshine";
    }

    std::optional<std::filesystem::path> journal_path() {
      auto folder = journal_folder();
      return folder ? std::optional(*folder / L"fork_nv_stream.json") : std::nullopt;
    }

    /// Writes the journal in full or not at all: into a new file, which then replaces the old one
    bool write_journal(const policy::journal_t &journal) {
      auto folder = journal_folder();
      // SYSTEM and Administrators only, so nobody else can plant a link there
      if (!folder || !nvprefs::prepare_undo_folder(*folder)) {
        BOOST_LOG(warning) << "fork: NVIDIA: no folder for the journal in %ProgramData%"sv;
        return false;
      }
      auto path = *folder / L"fork_nv_stream.json";
      auto temp = *folder / L"fork_nv_stream.json.new";
      {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << policy::to_json(journal);
        out.flush();
        if (!out.good()) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't write the journal"sv;
          return false;
        }
      }
      std::error_code ec;
      std::filesystem::rename(temp, path, ec);
      if (ec) {
        BOOST_LOG(warning) << "fork: NVIDIA: couldn't write the journal: "sv << ec.message();
        return false;
      }
      return true;
    }

    /// The journal a stream left, if there is one; a file that isn't one is logged and left alone
    std::optional<policy::journal_t> read_journal() {
      auto path = journal_path();
      std::error_code ec;
      if (!path || !std::filesystem::is_regular_file(*path, ec)) {
        return std::nullopt;
      }
      std::ifstream in(*path, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      auto journal = policy::from_json(text.str());
      if (!journal) {
        BOOST_LOG(warning) << "fork: NVIDIA: "sv << path->string() << " isn't a journal this Apollo can read; the settings it lists stay as they are"sv;
      }
      return journal;
    }

    void delete_journal() {
      if (auto path = journal_path()) {
        std::error_code ec;
        std::filesystem::remove(*path, ec);
        if (ec) {
          BOOST_LOG(warning) << "fork: NVIDIA: couldn't delete the journal: "sv << ec.message();
        }
      }
    }

    /// The journal from now on, in memory and on disk
    void keep(const std::optional<policy::journal_t> &journal) {
      if (journal == active && (!journal || !journal->entries.empty())) {
        return;
      }
      if (journal && !journal->entries.empty()) {
        write_journal(*journal);
        active = journal;
      } else {
        delete_journal();
        active.reset();
      }
    }

    policy::app_t app_choice(const std::string &app_uuid) {
      if (app_uuid.empty()) {
        return {};
      }
      try {
        auto tree = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
        for (const auto &app : tree.at("apps")) {
          if (app.is_object() && app.value("uuid", "") == app_uuid) {
            return policy::app_from_json(app);
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "fork: NVIDIA: couldn't read the app's choices from apps.json: "sv << e.what();
      }
      return {};
    }

    /**
     * @brief Brings the profile from the journal's settings to `want`'s (nv_stream_policy::update()).
     * @return Whether the frame cap is in effect afterwards.
     */
    bool update(drs_t &drs, const std::vector<policy::setting_t> &want) {
      auto kind = active ? active->profile : policy::profile_e::global;
      auto profile = drs.profile(kind);
      if (!profile && !active) {
        kind = policy::profile_e::base;
        profile = drs.profile(kind);
      }
      if (!profile) {
        BOOST_LOG(warning) << "fork: NVIDIA: couldn't open the driver's "sv << (kind == policy::profile_e::global ? "global"sv : "base"sv) << " profile"sv;
        return false;
      }

      std::vector<policy::entry_t> journal = active ? active->entries : std::vector<policy::entry_t> {};
      auto u = policy::update(journal, want, [&](uint32_t id) {
        return drs.read(profile, kind, id);
      });

      if (!u.actions.empty()) {
        // Every value to put back is in the journal before anything changes
        policy::journal_t saving {kind, policy::while_saving(journal, u)};
        if (!saving.entries.empty() && !write_journal(saving)) {
          BOOST_LOG(warning) << "fork: NVIDIA: the driver's settings stay as they are, as Apollo couldn't note how to put them back"sv;
          return false;
        }

        for (const auto &action : u.actions) {
          bool ok = action.kind == policy::action_t::kind_e::clear ? drs.clear(profile, action.id) : drs.write(profile, action.id, action.value);
          if (ok && action.kind == policy::action_t::kind_e::clear) {
            // If the profile still has our value as its own, the default didn't come back: it goes
            auto now = drs.read(profile, kind, action.id);
            bool still_ours = now && now->own && std::any_of(journal.begin(), journal.end(), [&](const policy::entry_t &e) {
              return e.id == action.id && e.ours == now->value;
            });
            if (still_ours) {
              ok = drs.remove(profile, action.id);
            }
          }
          if (ok) {
            if (action.kind == policy::action_t::kind_e::write) {
              BOOST_LOG(info) << "fork: NVIDIA: "sv << name(action.id) << ' ' << value_text(action.id, action.value) << " while streaming"sv;
            } else if (action.kind == policy::action_t::kind_e::set) {
              BOOST_LOG(info) << "fork: NVIDIA: "sv << name(action.id) << " back to "sv << value_text(action.id, action.value);
            } else {
              BOOST_LOG(info) << "fork: NVIDIA: "sv << name(action.id) << " back to the driver's default"sv;
            }
          } else if (action.kind == policy::action_t::kind_e::write) {
            // The profile keeps what it had, and the journal what it knew of it
            u.entries = policy::write_failed(std::move(u.entries), journal, action.id);
          } else {
            // Not put back, so still to be put back
            for (const auto &entry : journal) {
              if (entry.id == action.id) {
                u.entries.push_back(entry);
              }
            }
          }
        }

        if (!drs.save()) {
          active = saving;
          return false;
        }
      }

      keep(policy::journal_t {kind, u.entries});
      if (u.unreadable) {
        BOOST_LOG(warning) << "fork: NVIDIA: some settings couldn't be read and stay as they are"sv;
      }

      auto cap = std::find_if(want.begin(), want.end(), [](const policy::setting_t &s) {
        return s.id == policy::frame_rate_limit_id;
      });
      if (cap == want.end()) {
        return false;
      }
      auto now = drs.read(profile, kind, cap->id);
      return now && now->value == cap->value;
    }

    /// Puts back the journal's settings, from memory or else from the file
    void put_back(const char *why) {
      if (!active) {
        active = read_journal();
        if (!active) {
          return;
        }
        BOOST_LOG(info) << "fork: NVIDIA: putting back the settings "sv << why;
      }

      drs_t drs;
      if (!drs.open()) {
        BOOST_LOG(warning) << "fork: NVIDIA: couldn't reach the driver to put its settings back; the journal stays for the next try"sv;
        return;
      }
      update(drs, {});
    }
  }  // namespace

  bool apply(const std::string &app_uuid, uint32_t stream_mhz) {
    std::lock_guard lg {state_mutex};
    std::lock_guard nvapi_lg {nvapi_mutex()};

    // A journal a run left behind (recover() couldn't put it back) holds the values from before it
    if (!active) {
      active = read_journal();
    }

    if (!nv_stream_settings) {
      if (active) {
        put_back("as fork_nv_stream_settings is off");
      }
      return false;
    }

    drs_t drs;
    if (!drs.open()) {
      return false;
    }

    std::string note;
    auto choice = app_choice(app_uuid);
    auto want = policy::for_stream(choice, stream_mhz, drs.version(), smooth_motion_half_cap ? policy::smooth_motion_cap_e::half : policy::smooth_motion_cap_e::stream, &note);
    if (!note.empty()) {
      BOOST_LOG(warning) << "fork: NVIDIA: "sv << note;
    }
    return update(drs, want);
  }

  void restore() {
    std::lock_guard lg {state_mutex};
    std::lock_guard nvapi_lg {nvapi_mutex()};
    put_back("a stream changed");
  }

  void recover() {
    std::lock_guard lg {state_mutex};
    std::lock_guard nvapi_lg {nvapi_mutex()};
    put_back("an earlier run changed and didn't put back");
  }
#else
  bool apply(const std::string &, uint32_t) {
    return false;
  }

  void restore() {
  }

  void recover() {
  }
#endif
}  // namespace fork_tweaks::nv_stream
