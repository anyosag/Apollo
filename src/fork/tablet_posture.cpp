/**
 * @file src/fork/tablet_posture.cpp
 * @brief Fork-only: Windows in tablet posture while a client uses the stream as a touchscreen.
 */
// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"
#include "tablet_posture.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>

  #include "src/platform/windows/misc.h"

  #ifndef SM_CONVERTIBLESLATEMODE
    #define SM_CONVERTIBLESLATEMODE 0x2003
  #endif
#endif

using namespace std::literals;

namespace fork_tweaks::tablet_posture {
#ifdef _WIN32
  namespace {
    constexpr auto k_key = L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl";
    constexpr auto k_convertibility = L"ConvertibilityEnabled";
    constexpr auto k_slate_mode = L"ConvertibleSlateMode";

    /// How long the posture outlasts its last hold
    constexpr auto k_linger = 3s;

    /// How long after a change the log reads what Windows reports
    constexpr auto k_settle = 1s;

    /// The two values as they were before the posture; nothing where a value didn't exist
    struct values_t {
      std::optional<DWORD> convertibility;
      std::optional<DWORD> slate_mode;
    };

    std::mutex state_mutex;
    int holders = 0;
    std::string last_holder;

    /// One change at a time; guards `saved`
    std::mutex change_mutex;
    /// What the posture replaced, while it's on
    std::optional<values_t> saved;

    std::filesystem::path state_file() {
      return platf::appdata() / "fork_tablet_posture.state";
    }

    std::string text(std::optional<DWORD> value) {
      return value ? std::to_string(*value) : "unset"s;
    }

    std::optional<DWORD> read(const wchar_t *name) {
      DWORD value = 0;
      DWORD size = sizeof(value);
      if (RegGetValueW(HKEY_LOCAL_MACHINE, k_key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return value;
    }

    bool write(const wchar_t *name, std::optional<DWORD> value) {
      LSTATUS status;
      if (value) {
        DWORD data = *value;
        status = RegSetKeyValueW(HKEY_LOCAL_MACHINE, k_key, name, REG_DWORD, &data, sizeof(data));
      } else {
        status = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, k_key, name);
        if (status == ERROR_FILE_NOT_FOUND) {
          status = ERROR_SUCCESS;
        }
      }

      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "fork: tablet posture couldn't set "sv << platf::to_utf8(name) << " to "sv << text(value) << ": error "sv << status;
        return false;
      }
      return true;
    }

    /// The values from before a posture that a stopped run left on, if it did
    std::optional<values_t> load_state() {
      std::ifstream in(state_file());
      if (!in) {
        return std::nullopt;
      }

      values_t values;
      std::string line;
      while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) {
          continue;
        }
        auto name = line.substr(0, eq);
        auto value = line.substr(eq + 1);
        std::optional<DWORD> parsed;
        if (value != "unset") {
          try {
            parsed = (DWORD) std::stoul(value);
          } catch (const std::exception &) {
            BOOST_LOG(warning) << "fork: tablet posture state has an unreadable line ["sv << line << ']';
            continue;
          }
        }
        if (name == "ConvertibilityEnabled") {
          values.convertibility = parsed;
        } else if (name == "ConvertibleSlateMode") {
          values.slate_mode = parsed;
        }
      }
      return values;
    }

    void save_state(const values_t &values) {
      std::ofstream out(state_file(), std::ios::trunc);
      out << "ConvertibilityEnabled=" << text(values.convertibility) << '\n'
          << "ConvertibleSlateMode=" << text(values.slate_mode) << '\n';
      if (!out) {
        BOOST_LOG(warning) << "fork: tablet posture couldn't write "sv << state_file().string() << "; a crash would leave it on"sv;
      }
    }

    /// What Windows reports: the posture apps see, and the touch input it knows of
    std::string metrics() {
      std::ostringstream out;
      out << "slate mode "sv << (GetSystemMetrics(SM_CONVERTIBLESLATEMODE) == 0 ? "on"sv : "off"sv)
          << ", digitizer 0x"sv << std::hex << GetSystemMetrics(SM_DIGITIZER) << std::dec
          << ", "sv << GetSystemMetrics(SM_MAXIMUMTOUCHES) << " touch points"sv;
      return out.str();
    }

    /// Tells the windows of the desktop the user sees that the posture changed
    void broadcast() {
      platf::syncThreadDesktop();

      DWORD_PTR result = 0;
      if (!SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM) L"ConvertibleSlateMode", SMTO_ABORTIFHUNG, 1000, &result)) {
        BOOST_LOG(warning) << "fork: tablet posture broadcast failed: error "sv << GetLastError();
      }
    }

    void turn_on(const std::string &device_name) {
      auto before = metrics();
      values_t was {read(k_convertibility), read(k_slate_mode)};

      // A file left by a stopped run holds the values from before that run's posture
      auto left = load_state();
      saved = left ? *left : was;
      if (!left) {
        save_state(was);
      }

      write(k_convertibility, 1);
      write(k_slate_mode, 0);
      broadcast();

      std::this_thread::sleep_for(k_settle);
      BOOST_LOG(info) << "fork: tablet posture on for client ["sv << device_name << "]: ConvertibilityEnabled "sv << text(was.convertibility)
                      << " -> 1, ConvertibleSlateMode "sv << text(was.slate_mode) << " -> 0; Windows reported "sv << before << ", now "sv << metrics();
    }

    void turn_off(std::string_view why) {
      auto values = *saved;
      bool ok = write(k_convertibility, values.convertibility);
      ok = write(k_slate_mode, values.slate_mode) && ok;
      broadcast();
      saved.reset();

      std::error_code ec;
      if (ok) {
        std::filesystem::remove(state_file(), ec);
      }

      std::this_thread::sleep_for(k_settle);
      BOOST_LOG(info) << "fork: tablet posture off ("sv << why << "): ConvertibilityEnabled back to "sv << text(values.convertibility)
                      << ", ConvertibleSlateMode back to "sv << text(values.slate_mode) << "; Windows now reports "sv << metrics();
    }

    /// Makes the posture match whether anyone holds it
    void sync(std::string_view why) {
      std::lock_guard change_lock(change_mutex);

      bool wanted;
      std::string device_name;
      {
        std::lock_guard lg(state_mutex);
        wanted = holders > 0;
        device_name = last_holder;
      }

      if (wanted && !saved) {
        turn_on(device_name);
      } else if (!wanted && saved) {
        turn_off(why);
      }
    }
  }  // namespace

  hold_t::hold_t(const std::string &device_name) {
    {
      std::lock_guard lg(state_mutex);
      holders++;
      last_holder = device_name;
    }
    std::thread([] {
      sync("");
    }).detach();
  }

  hold_t::~hold_t() {
    {
      std::lock_guard lg(state_mutex);
      holders--;
    }
    std::thread([] {
      std::this_thread::sleep_for(k_linger);
      sync("no client touching");
    }).detach();
  }

  void recover() {
    {
      std::lock_guard change_lock(change_mutex);
      auto left = load_state();
      if (!left || saved) {
        return;
      }
      saved = left;
    }
    std::thread([] {
      sync("left on when Apollo last stopped");
    }).detach();
  }
#else
  hold_t::hold_t(const std::string &) {}

  hold_t::~hold_t() {}

  void recover() {}
#endif
}  // namespace fork_tweaks::tablet_posture
