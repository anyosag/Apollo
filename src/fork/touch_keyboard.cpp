/**
 * @file src/fork/touch_keyboard.cpp
 * @brief Fork-only: Windows' touch keyboard for stock Moonlight for iOS's taps in text fields.
 */
// standard includes
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// local includes
#include "src/logging.h"
#include "touch_keyboard.h"
#include "user_helper.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
#endif

using namespace std::literals;

namespace fork_tweaks::touch_keyboard {
#ifdef _WIN32
  namespace {
    /// How long the helper outlasts its last hold
    constexpr auto k_linger = 3s;

    /// How long a helper told to stop gets to close its keyboard and exit
    constexpr auto k_stop_wait = 3s;

    /// Starts per stretch of holds: a helper that keeps dying isn't started forever
    constexpr int k_max_starts = 3;

    /// Commands waiting for the helper beyond this are dropped: it's stuck
    constexpr std::size_t k_max_pending = 16;

    std::mutex state_mutex;
    std::condition_variable state_cv;
    int holders = 0;
    std::string last_holder;
    std::chrono::steady_clock::time_point released;  ///< When the last hold went
    std::deque<std::string_view> pending;  ///< "tap" and "key", for the helper
    bool worker_running = false;
    bool helper_exited = false;  ///< Its output ended

    /// The helper's process, its input and the thread reading its output
    class helper_t {
    public:
      ~helper_t() {
        stop();
      }

      bool running() const {
        return _process && WaitForSingleObject(_process, 0) == WAIT_TIMEOUT;
      }

      bool started() const {
        return _process != nullptr;
      }

      /// Starts it as the signed-in user, unelevated, on the user's desktop (user_helper.h)
      bool start(const std::string &device_name) {
        auto path = user_helper::path(L"apollo-touch-keyboard.exe");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
          BOOST_LOG(warning) << "fork: touch keyboard: "sv << path.string() << " is missing, so taps in text fields bring no keyboard"sv;
          return false;
        }

        user_helper::process_t started;
        if (user_helper::start(path, "touch keyboard"sv, started) != user_helper::start_e::started) {
          return false;
        }

        _process = (HANDLE) started.process;
        _input = (HANDLE) started.input;
        {
          std::lock_guard lg(state_mutex);
          helper_exited = false;
        }
        _reader = std::thread(read_output, (HANDLE) started.output);
        BOOST_LOG(info) << "fork: touch keyboard helper started for client ["sv << device_name << "] (PID "sv << GetProcessId(_process) << ')';
        return true;
      }

      /// Sends commands, one per line. @return Whether it took them
      bool write(const std::deque<std::string_view> &commands) {
        std::string text;
        for (auto command : commands) {
          text += command;
          text += '\n';
        }
        DWORD written = 0;
        return _input && WriteFile(_input, text.data(), (DWORD) text.size(), &written, nullptr) && written == text.size();
      }

      /// Closes its input, so it closes its keyboard and exits; ends it if it doesn't
      void stop() {
        if (_input) {
          CloseHandle(_input);
          _input = nullptr;
        }
        if (_process) {
          if (WaitForSingleObject(_process, (DWORD) std::chrono::milliseconds(k_stop_wait).count()) == WAIT_TIMEOUT) {
            BOOST_LOG(warning) << "fork: touch keyboard helper didn't stop; ending it"sv;
            TerminateProcess(_process, 1);
            WaitForSingleObject(_process, 1000);
          }
          log_exit();
          CloseHandle(_process);
          _process = nullptr;
        }
        if (_reader.joinable()) {
          _reader.join();
        }
      }

      /// After it exited by itself: says how, and lets go of it
      void reap() {
        BOOST_LOG(warning) << "fork: touch keyboard helper stopped by itself"sv;
        stop();
      }

    private:
      static void read_output(HANDLE output) {
        std::string buffer;
        char chunk[512];
        DWORD read = 0;
        while (ReadFile(output, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
          buffer.append(chunk, read);
          std::size_t end;
          while ((end = buffer.find('\n')) != std::string::npos) {
            std::string_view line {buffer.data(), end};
            if (!line.empty() && line.back() == '\r') {
              line.remove_suffix(1);
            }
            if (!line.empty()) {
              user_helper::log_line("touch keyboard"sv, line);
            }
            buffer.erase(0, end + 1);
          }
          // A line that never ends isn't kept forever
          if (buffer.size() > 4096) {
            buffer.clear();
          }
        }
        CloseHandle(output);

        std::lock_guard lg(state_mutex);
        helper_exited = true;
        state_cv.notify_all();
      }

      void log_exit() {
        DWORD code = 0;
        if (GetExitCodeProcess(_process, &code) && code != 0) {
          BOOST_LOG(warning) << "fork: touch keyboard helper exited with "sv << code;
        }
      }

      HANDLE _process = nullptr;
      HANDLE _input = nullptr;
      std::thread _reader;
    };

    /// Runs the helper while anyone holds it, and hands it the commands
    void worker() {
      helper_t helper;
      int starts = 0;
      bool start_wanted = true;  ///< At the first hold, and after a failure only with the next command

      std::unique_lock ul(state_mutex);
      // The last helper's output ending, after the last worker stopped it
      helper_exited = false;
      while (true) {
        if (holders == 0) {
          pending.clear();
          if (state_cv.wait_until(ul, released + k_linger, [] {
                return holders > 0;
              })) {
            continue;
          }
          break;
        }

        if (helper_exited) {
          helper_exited = false;
          if (helper.started()) {
            ul.unlock();
            helper.reap();
            ul.lock();
          }
          continue;
        }

        if (!helper.running() && start_wanted) {
          start_wanted = false;
          if (starts == k_max_starts) {
            BOOST_LOG(warning) << "fork: touch keyboard helper started "sv << k_max_starts << " times; not again until the next stream"sv;
            starts++;
          } else if (starts < k_max_starts) {
            starts++;
            auto device_name = last_holder;
            ul.unlock();
            helper.start(device_name);
            ul.lock();
            continue;
          }
        }

        if (!pending.empty()) {
          std::deque<std::string_view> commands;
          commands.swap(pending);
          if (!helper.running()) {
            // The next command starts it again
            start_wanted = true;
            continue;
          }
          ul.unlock();
          helper.write(commands);
          ul.lock();
          continue;
        }

        state_cv.wait(ul, [] {
          return !pending.empty() || holders == 0 || helper_exited;
        });
      }

      ul.unlock();
      helper.stop();
      ul.lock();
      worker_running = false;

      // A hold that came while it stopped gets a new helper
      if (holders > 0) {
        worker_running = true;
        std::thread(worker).detach();
      }
    }

    void push(std::string_view command) {
      std::lock_guard lg(state_mutex);
      if (pending.size() >= k_max_pending || (command == "key" && !pending.empty() && pending.back() == "key")) {
        return;
      }
      pending.push_back(command);
      state_cv.notify_all();
    }
  }  // namespace

  hold_t::hold_t(const std::string &device_name) {
    std::lock_guard lg(state_mutex);
    holders++;
    last_holder = device_name;
    if (!worker_running) {
      worker_running = true;
      std::thread(worker).detach();
    }
    state_cv.notify_all();
  }

  hold_t::~hold_t() {
    std::lock_guard lg(state_mutex);
    if (--holders == 0) {
      released = std::chrono::steady_clock::now();
    }
    state_cv.notify_all();
  }

  void hold_t::tapped() {
    push("tap");
  }

  void hold_t::typed() {
    push("key");
  }
#else
  hold_t::hold_t(const std::string &) {}

  hold_t::~hold_t() {}

  void hold_t::tapped() {}

  void hold_t::typed() {}
#endif
}  // namespace fork_tweaks::touch_keyboard
