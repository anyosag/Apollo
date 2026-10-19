/**
 * @file src/fork/pointer_speed.cpp
 * @brief Fork-only: Windows moves the pointer exactly as far as it's told while LuzLunar's Mac
 * trackpad mode streams.
 */
// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

// local includes
#include "pointer_speed.h"
#include "pointer_speed_state.h"
#include "src/logging.h"
#include "src/platform/common.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::pointer_speed {
#ifdef _WIN32
  namespace {
    /// How long the setting outlasts its last hold
    constexpr auto k_linger = 3s;

    std::mutex state_mutex;
    int holders = 0;
    std::string last_holder;

    /// One change at a time; guards `saved`
    std::mutex change_mutex;
    /// What the hold replaced, while it's on
    std::optional<values_t> saved;

    std::filesystem::path state_file() {
      return platf::appdata() / "fork_pointer_speed.state";
    }

    /// The session's values now
    std::optional<values_t> read() {
      int mouse[3] {};
      int speed = 0;
      if (!SystemParametersInfoW(SPI_GETMOUSE, 0, mouse, 0) || !SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0)) {
        BOOST_LOG(warning) << "fork: couldn't read Windows' pointer settings: error "sv << GetLastError();
        return std::nullopt;
      }
      return values_t {mouse[0], mouse[1], mouse[2], speed};
    }

    /// Sets the session's values; without SPIF_UPDATEINIFILE the user's saved ones stay as they are
    bool write(const values_t &v) {
      int mouse[3] {v.threshold1, v.threshold2, v.acceleration};
      bool ok = true;
      if (!SystemParametersInfoW(SPI_SETMOUSE, 0, mouse, 0)) {
        BOOST_LOG(warning) << "fork: couldn't set Windows' pointer acceleration: error "sv << GetLastError();
        ok = false;
      }
      if (!SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID) (INT_PTR) v.speed, 0)) {
        BOOST_LOG(warning) << "fork: couldn't set Windows' pointer speed: error "sv << GetLastError();
        ok = false;
      }
      return ok;
    }

    /// The values from before a hold that a stopped run left on, if it did
    std::optional<values_t> load_state() {
      std::ifstream in(state_file());
      if (!in) {
        return std::nullopt;
      }
      std::string line;
      std::getline(in, line);
      auto values = parse(line);
      if (!values) {
        BOOST_LOG(warning) << "fork: the pointer settings state ["sv << line << "] is unreadable; leaving Windows' pointer settings as they are"sv;
        std::error_code ec;
        std::filesystem::remove(state_file(), ec);
      }
      return values;
    }

    void save_state(const values_t &values) {
      std::ofstream out(state_file(), std::ios::trunc);
      out << format(values) << '\n';
      if (!out) {
        BOOST_LOG(warning) << "fork: couldn't write "sv << state_file().string() << "; a crash would leave Windows' pointer acceleration off until you sign out"sv;
      }
    }

    void turn_on(const std::string &device_name) {
      // The settings belong to the session of the desktop the user sees
      platf::syncThreadDesktop();
      auto was = read();
      if (!was) {
        return;
      }

      // A file left by a stopped run holds the values from before that run's hold
      auto left = load_state();
      saved = left ? *left : *was;
      if (!left) {
        save_state(*was);
      }

      if (one_to_one(*was)) {
        BOOST_LOG(info) << "fork: client ["sv << device_name << "] streams in Mac trackpad mode; Windows already moves the pointer as far as it's told ("sv
                        << describe(*was) << ')';
        return;
      }
      write(k_one_to_one);
      BOOST_LOG(info) << "fork: client ["sv << device_name << "] streams in Mac trackpad mode, so Windows doesn't accelerate the pointer again: "sv
                      << describe(*was) << " -> "sv << describe(k_one_to_one) << " for now"sv;
    }

    void turn_off(std::string_view why) {
      platf::syncThreadDesktop();
      auto values = *saved;
      bool ok = write(values);
      saved.reset();

      if (ok) {
        std::error_code ec;
        std::filesystem::remove(state_file(), ec);
      }
      BOOST_LOG(info) << "fork: Windows' pointer settings back to "sv << describe(values) << " ("sv << why << ')';
    }

    /// Makes the settings match whether anyone holds them
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
      sync("the Mac trackpad stream ended");
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
      sync("left that way when Apollo last stopped");
    }).detach();
  }
#else
  hold_t::hold_t(const std::string &) {}

  hold_t::~hold_t() {}

  void recover() {}
#endif
}  // namespace fork_tweaks::pointer_speed
