/**
 * @file src/fork/updater.cpp
 * @brief Apollo's over-the-air updates.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "fork_config.h"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "update_download.h"
#include "update_key.h"
#include "update_manifest.h"
#include "update_verify.h"
#include "updater.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>

  #include <aclapi.h>
  #include <sddl.h>

  // local includes
  #include "src/platform/windows/misc.h"
#endif

// CI sets these for the build (cmake/prep/build_version.cmake); a local build is a branch build
#ifndef FORK_BUILD_RUN
  #define FORK_BUILD_RUN 0
#endif
#ifndef FORK_BUILD_CHANNEL
  #define FORK_BUILD_CHANNEL "local"
#endif

using namespace std::literals;

namespace fork_tweaks::updater {
  namespace fu = fork_update;

  namespace {
    constexpr auto first_check_delay = 1min;
    constexpr auto check_interval = 30min;
    constexpr auto poll_interval = 15s;  ///< While waiting for a stream to end, or for the helper
    constexpr auto install_timeout = 20min;  ///< Apollo should have been stopped by the installer by then
    constexpr auto installer_name = "Apollo-windows-installer.exe"sv;

    /// A downloaded, checked installer
    struct download_t {
      std::filesystem::path file;
      fu::asset_t asset;
      std::string version;
      int64_t run = 0;
    };

    struct updater_t {
      std::mutex mutex;
      std::condition_variable cv;
      std::thread thread;
      std::atomic<bool> stopping {false};
      bool wake = false;
      bool check_requested = false;
      bool install_requested = false;
      bool supported = false;

      fu::build_t running;
      fu::state_e state = fu::state_e::idle;
      std::string message;
      int64_t checked = 0;  ///< When the last check ended, in seconds since 1970
      std::optional<fu::manifest_t> latest;
      std::optional<download_t> ready;
      std::optional<download_t> previous;  ///< This build's own installer, to roll back to
      std::optional<fu::result_t> last_result;
      std::chrono::steady_clock::time_point install_started;
      int64_t install_started_time = 0;
    };

    updater_t &u() {
      static updater_t instance;
      return instance;
    }

    int64_t now_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::filesystem::path updates_dir() {
      return platf::appdata() / "updates";
    }

    std::filesystem::path run_dir(int64_t run) {
      return updates_dir() / std::to_string(run);
    }

    bool read_text(const std::filesystem::path &path, std::string &out) {
      std::ifstream in {path, std::ios::binary};
      if (!in) {
        return false;
      }
      std::stringstream ss;
      ss << in.rdbuf();
      out = ss.str();
      return true;
    }

    /// Write next to the file, then rename, so a reader never sees half of it
    bool write_text(const std::filesystem::path &path, const std::string &text) {
      auto temp = path;
      temp += ".tmp";
      {
        std::ofstream out {temp, std::ios::binary | std::ios::trunc};
        if (!(out << text)) {
          return false;
        }
      }
      std::error_code ec;
      std::filesystem::rename(temp, path, ec);
      return !ec;
    }

    /// The file is there and is the asset the manifest describes
    bool matches(const std::filesystem::path &file, const fu::asset_t &asset) {
      uint64_t size = 0;
      auto hash = fu::sha256_file(file, &size);
      return hash && *hash == asset.sha256 && size == asset.size;
    }

    void set_state(fu::state_e state, std::string message = {}) {
      std::lock_guard lock {u().mutex};
      u().state = state;
      u().message = std::move(message);
    }

#ifdef _WIN32
    bool is_system() {
      BYTE sid_buffer[SECURITY_MAX_SID_SIZE];
      DWORD sid_size = sizeof(sid_buffer);
      BOOL member = FALSE;
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, sid_buffer, &sid_size) || !CheckTokenMembership(nullptr, sid_buffer, &member)) {
        return false;
      }
      return member;
    }

    /// Create the folder, open only to SYSTEM and Administrators, so nobody else can swap an installer after it's checked
    bool make_private_dir(const std::filesystem::path &dir) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        BOOST_LOG(error) << "Update: couldn't create "sv << dir.string() << ": "sv << ec.message();
        return false;
      }
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
      }
      BOOL present = FALSE;
      BOOL defaulted = FALSE;
      PACL dacl = nullptr;
      bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) &&
                SetNamedSecurityInfoW(const_cast<wchar_t *>(dir.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
      LocalFree(descriptor);
      if (!ok) {
        BOOST_LOG(error) << "Update: couldn't restrict "sv << dir.string();
      }
      return ok;
    }

    std::filesystem::path install_dir() {
      WCHAR path[MAX_PATH];
      GetModuleFileNameW(nullptr, path, _countof(path));
      return std::filesystem::path {path}.parent_path();
    }

    std::wstring quote(const std::wstring &s) {
      return L"\"" + s + L"\"";
    }

    /**
     * @brief Start tools\apollo-updater.exe from a copy in the updates folder, as SYSTEM and outside
     * the service's job, so it outlives the installer stopping the service.
     */
    bool launch_helper(const download_t &update, const std::optional<download_t> &previous, const std::string &running_version, std::string *error) {
      auto helper = updates_dir() / L"apollo-updater.exe";
      std::error_code ec;
      std::filesystem::copy_file(install_dir() / L"tools" / L"apollo-updater.exe", helper, std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        *error = "couldn't copy the install helper: " + ec.message();
        return false;
      }

      std::wstring command = quote(helper.wstring()) +
                             L" --dir " + quote(updates_dir().wstring()) +
                             L" --install-dir " + quote(install_dir().wstring()) +
                             L" --installer " + quote(update.file.wstring()) +
                             L" --sha256 " + platf::from_utf8(update.asset.sha256) +
                             L" --run " + std::to_wstring(update.run) +
                             L" --version " + quote(platf::from_utf8(update.version)) +
                             L" --running-version " + quote(platf::from_utf8(running_version));
      if (previous) {
        command += L" --previous " + quote(previous->file.wstring()) +
                   L" --previous-sha256 " + platf::from_utf8(previous->asset.sha256) +
                   L" --previous-run " + std::to_wstring(previous->run) +
                   L" --previous-version " + quote(platf::from_utf8(previous->version));
      }

      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process {};
      auto dir = updates_dir().wstring();
      if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, dir.c_str(), &startup, &process)) {
        *error = "couldn't start the install helper (error " + std::to_string(GetLastError()) + ")";
        return false;
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return true;
    }
#else
    bool is_system() {
      return false;
    }

    bool make_private_dir(const std::filesystem::path &) {
      return false;
    }

    bool launch_helper(const download_t &, const std::optional<download_t> &, const std::string &, std::string *error) {
      *error = "updates are only for Windows";
      return false;
    }
#endif

    /// Download a manifest and its signature, and check both
    std::optional<fu::manifest_t> fetch_manifest(const std::string &url, const std::string &signature_url, std::string *error) {
      auto text = fu::fetch(url, fu::max_manifest_size, error, &u().stopping);
      if (!text) {
        return std::nullopt;
      }
      auto signature = fu::fetch(signature_url, 1024, error, &u().stopping);
      if (!signature) {
        return std::nullopt;
      }
      if (!fu::verify_manifest(fu::public_key, *text, *signature)) {
        *error = "the update's signature doesn't match Apollo's key";
        return std::nullopt;
      }
      return fu::parse_manifest(*text, fu::product, fu::asset_url_prefix, error);
    }

    /// Download into the run's folder unless it's already there
    bool ensure_downloaded(const fu::manifest_t &m, download_t &out, std::string *error) {
      if (!make_private_dir(run_dir(m.run))) {
        *error = "couldn't create the updates folder";
        return false;
      }
      auto file = run_dir(m.run) / m.asset.name;
      if (!matches(file, m.asset)) {
        if (!fu::fetch_to_file(m.asset.url, file, m.asset.size, error, &u().stopping)) {
          return false;
        }
        if (!matches(file, m.asset)) {
          std::error_code ec;
          std::filesystem::remove(file, ec);
          *error = "the download doesn't match the update's SHA-256";
          return false;
        }
      }
      out = {file, m.asset, m.version, m.run};
      return true;
    }

    /// Keep this build's own installer from its release, so the helper can put it back
    void cache_previous() {
      fu::build_t running;
      {
        std::lock_guard lock {u().mutex};
        if (u().previous) {
          return;
        }
        running = u().running;
      }
      if (!running.is_master()) {
        return;
      }

      std::string error;
      auto manifest = fetch_manifest(
        fu::release_asset_url(fu::asset_url_prefix, fu::release_tag_prefix, running.run, "update.json"),
        fu::release_asset_url(fu::asset_url_prefix, fu::release_tag_prefix, running.run, "update.json.sig"),
        &error
      );
      if (manifest && (manifest->run != running.run || manifest->asset.name != installer_name)) {
        error = "its release describes another build";
        manifest.reset();
      }
      download_t previous;
      if (!manifest || !ensure_downloaded(*manifest, previous, &error)) {
        BOOST_LOG(warning) << "Update: no installer of this build to roll back to: "sv << error;
        return;
      }
      std::lock_guard lock {u().mutex};
      u().previous = previous;
    }

    /// Remove the folders of builds that are neither running nor the latest
    void prune(int64_t keep) {
      std::error_code ec;
      int64_t running_run = u().running.run;
      for (auto &entry : std::filesystem::directory_iterator {updates_dir(), ec}) {
        if (!entry.is_directory()) {
          continue;
        }
        auto name = entry.path().filename().string();
        if (name.empty() || name.size() > 18 || name.find_first_not_of("0123456789") != std::string::npos) {
          continue;
        }
        auto run = std::stoll(name);
        if (run != running_run && run != keep) {
          std::filesystem::remove_all(entry.path(), ec);
        }
      }
    }

    void check() {
      set_state(fu::state_e::checking, "Checking for updates");

      std::string error;
      auto manifest = fetch_manifest(std::string {fu::manifest_url}, std::string {fu::signature_url}, &error);
      if (!manifest) {
        BOOST_LOG(warning) << "Update: check failed: "sv << error;
        std::lock_guard lock {u().mutex};
        u().checked = now_seconds();
        u().state = fu::state_e::failed;
        u().message = "Couldn't check for updates: " + error;
        return;
      }

      bool update;
      {
        std::lock_guard lock {u().mutex};
        u().latest = manifest;
        u().checked = now_seconds();
        update = fu::is_update(*manifest, u().running);
        if (!update) {
          u().ready.reset();
          u().install_requested = false;
          u().state = fu::state_e::idle;
          u().message = "Apollo is up to date";
        } else if (u().ready && u().ready->run == manifest->run) {
          // Already downloaded; leave the state as it is (ready or waiting)
          return;
        } else {
          u().ready.reset();
          u().state = fu::state_e::downloading;
          u().message = "Downloading " + manifest->version;
        }
      }
      if (!update) {
        prune(0);
        return;
      }

      BOOST_LOG(info) << "Update: "sv << manifest->version << " (run "sv << manifest->run << ") is available"sv;
      download_t download;
      if (!ensure_downloaded(*manifest, download, &error)) {
        BOOST_LOG(warning) << "Update: download failed: "sv << error;
        set_state(fu::state_e::failed, "Couldn't download " + manifest->version + ": " + error);
        return;
      }
      {
        std::lock_guard lock {u().mutex};
        u().ready = download;
        u().state = fu::state_e::ready;
        u().message = manifest->version + " is ready to install";
      }
      cache_previous();
      prune(manifest->run);
    }

    void maybe_install() {
      bool ready;
      bool requested;
      bool automatic;
      {
        std::lock_guard lock {u().mutex};
        ready = u().ready && (u().state == fu::state_e::ready || u().state == fu::state_e::waiting);
        requested = u().install_requested;
        automatic = u().latest && fu::may_install_automatically(*u().latest, u().running, fork_tweaks::auto_update);
      }
      bool streaming = rtsp_stream::session_count() > 0;
      bool app_running = proc::proc.running() > 0;

      switch (fu::decide_install(ready, requested, automatic, streaming, app_running)) {
        case fu::install_decision_e::none:
          {
            std::lock_guard lock {u().mutex};
            if (u().state == fu::state_e::waiting) {
              // Automatic updates were turned off while it waited
              u().state = fu::state_e::ready;
              u().message = u().ready->version + " is ready to install";
            }
          }
          return;
        case fu::install_decision_e::wait:
          {
            std::lock_guard lock {u().mutex};
            u().state = fu::state_e::waiting;
            u().message = "Installs " + u().ready->version + " once nobody is streaming";
          }
          return;
        case fu::install_decision_e::install:
          break;
      }

      download_t update;
      std::optional<download_t> previous;
      std::string running_version;
      {
        std::lock_guard lock {u().mutex};
        update = *u().ready;
        previous = u().previous;
        running_version = u().running.version;
        u().install_requested = false;
      }

      // Checked again right before it's handed over
      if (!matches(update.file, update.asset)) {
        std::lock_guard lock {u().mutex};
        u().ready.reset();
        u().state = fu::state_e::failed;
        u().message = "The downloaded update changed on disk; it downloads again at the next check";
        return;
      }
      if (app_running) {
        proc::proc.terminate();
      }

      BOOST_LOG(info) << "Update: installing "sv << update.version << (previous ? "" : " (no installer to roll back to)");
      std::string error;
      if (!launch_helper(update, previous, running_version, &error)) {
        BOOST_LOG(error) << "Update: "sv << error;
        set_state(fu::state_e::failed, "Couldn't install " + update.version + ": " + error);
        return;
      }
      std::lock_guard lock {u().mutex};
      u().state = fu::state_e::installing;
      u().message = "Installing " + update.version + "; Apollo restarts in a moment";
      u().install_started = std::chrono::steady_clock::now();
      u().install_started_time = now_seconds();
    }

    /// While installing: the helper may have given up without the installer ever stopping Apollo
    void watch_install() {
      std::string text;
      std::optional<fu::result_t> result;
      if (read_text(updates_dir() / "result.json", text)) {
        result = fu::parse_result(text);
      }
      std::lock_guard lock {u().mutex};
      if (result && result->time >= u().install_started_time) {
        u().last_result = result;
        u().state = fu::state_e::failed;
        u().message = fu::describe(*result);
      } else if (std::chrono::steady_clock::now() - u().install_started > install_timeout) {
        u().state = fu::state_e::failed;
        u().message = "The install didn't start; see updater.log in Apollo's config\\updates folder";
      }
    }

    void run() {
      auto next_check = std::chrono::steady_clock::now() + first_check_delay;
      while (true) {
        bool do_check;
        fu::state_e state;
        {
          std::unique_lock lock {u().mutex};
          auto deadline = next_check;
          if (u().state == fu::state_e::waiting || u().state == fu::state_e::installing) {
            deadline = std::min(deadline, std::chrono::steady_clock::now() + poll_interval);
          }
          u().cv.wait_until(lock, deadline, [] {
            return u().stopping.load() || u().wake;
          });
          if (u().stopping) {
            return;
          }
          u().wake = false;
          state = u().state;
          do_check = state != fu::state_e::installing &&
                     (u().check_requested || std::chrono::steady_clock::now() >= next_check || (u().install_requested && !u().ready));
          u().check_requested = false;
        }

        if (state == fu::state_e::installing) {
          watch_install();
          continue;
        }
        if (do_check) {
          check();
          next_check = std::chrono::steady_clock::now() + check_interval;
        }
        maybe_install();
      }
    }

    /// Set `fork_auto_update` in sunshine.conf, changing only its own line
    bool save_automatic(bool automatic) {
      auto path = config::sunshine.config_file;
      std::string content = file_handler::read_file(path.c_str());
      std::string line = "fork_auto_update = "s + (automatic ? "enabled" : "disabled");

      std::stringstream in {content};
      std::string out;
      std::string current;
      bool found = false;
      while (std::getline(in, current)) {
        auto start = current.find_first_not_of(" \t");
        auto key_end = current.find_first_of(" \t=", start);
        if (start != std::string::npos && current.compare(start, key_end - start, "fork_auto_update") == 0) {
          if (!found) {
            out += line + "\n";
          }
          found = true;
          continue;
        }
        out += current + "\n";
      }
      if (!found) {
        out += line + "\n";
      }
      return file_handler::write_file(path.c_str(), out) == 0;
    }
  }  // namespace

  void start() {
    auto &s = u();
    s.running = {FORK_BUILD_RUN, FORK_BUILD_CHANNEL, PROJECT_VERSION, PROJECT_VERSION_COMMIT};
    s.supported = is_system();
    if (!s.supported) {
      BOOST_LOG(info) << "Update: over-the-air updates work only when Apollo runs as its service"sv;
      return;
    }
    if (!make_private_dir(updates_dir())) {
      s.supported = false;
      return;
    }

#ifdef _WIN32
    // Tells an install helper waiting on this build that it's up
    fu::started_t started {s.running.run, static_cast<uint32_t>(GetCurrentProcessId()), net::map_port(nvhttp::PORT_HTTP), s.running.version};
    write_text(updates_dir() / "started.json", fu::to_json(started));
#endif

    std::string text;
    if (read_text(updates_dir() / "result.json", text)) {
      s.last_result = fu::parse_result(text);
      if (s.last_result) {
        BOOST_LOG(info) << "Update: last install: "sv << fu::describe(*s.last_result);
      }
    }

    BOOST_LOG(info) << "Update: Apollo "sv << s.running.version << " (run "sv << s.running.run << ", "sv << s.running.channel << "), automatic updates "sv << (fork_tweaks::auto_update ? "on" : "off");
    s.thread = std::thread {run};
  }

  void stop() {
    {
      std::lock_guard lock {u().mutex};
      u().stopping = true;
    }
    u().cv.notify_all();
    if (u().thread.joinable()) {
      u().thread.join();
    }
  }

  bool supported() {
    return u().supported;
  }

  std::string status_json() {
    std::lock_guard lock {u().mutex};
    auto &s = u();
    nlohmann::json j;
    j["supported"] = s.supported;
    j["version"] = s.running.version;
    j["run"] = s.running.run;
    j["channel"] = s.running.channel;
    j["commit"] = s.running.commit;
    j["state"] = fu::to_string(s.state);
    j["message"] = s.message;
    j["automatic"] = fork_tweaks::auto_update;
    j["checked"] = s.checked;
    j["rollback"] = s.previous.has_value();
    if (s.latest) {
      j["latest"] = {
        {"version", s.latest->version},
        {"run", s.latest->run},
        {"commit", s.latest->commit},
        {"published", s.latest->published},
        {"notes", s.latest->notes},
      };
      j["update"] = fu::is_update(*s.latest, s.running);
    } else {
      j["latest"] = nullptr;
      j["update"] = false;
    }
    if (s.last_result) {
      j["last_result"] = {
        {"outcome", fu::to_string(s.last_result->outcome)},
        {"text", fu::describe(*s.last_result)},
        {"time", s.last_result->time},
      };
    } else {
      j["last_result"] = nullptr;
    }
    return j.dump();
  }

  bool request(std::string_view action, std::optional<bool> automatic, std::string *error) {
    if (!u().supported) {
      *error = "Apollo updates itself only when it runs as the Apollo service";
      return false;
    }
    if (!action.empty() && action != "check"sv && action != "install"sv) {
      *error = "unknown action";
      return false;
    }
    if (automatic) {
      if (!save_automatic(*automatic)) {
        *error = "couldn't save the setting";
        return false;
      }
      BOOST_LOG(info) << "Update: automatic updates turned "sv << (*automatic ? "on" : "off");
    }
    {
      std::lock_guard lock {u().mutex};
      if (automatic) {
        fork_tweaks::auto_update = *automatic;
      }
      if (action == "check"sv) {
        u().check_requested = true;
      } else if (action == "install"sv && u().state != fu::state_e::installing) {
        u().install_requested = true;
      }
      u().wake = true;
    }
    u().cv.notify_all();
    return true;
  }
}  // namespace fork_tweaks::updater
