/**
 * @file src/fork/mac_drive.cpp
 * @brief Fork-only: LuzLunar's Mac as a network drive on this PC, during LAN streams.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// local includes
#include "fork_config.h"
#include "mac_drive.h"
#include "mac_drive_policy.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/rtsp.h"
#include "user_helper.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::mac_drive {
  namespace policy = mac_drive_policy;

  namespace {
    reply_t error_reply(int status, std::string_view error) {
      return {status, nlohmann::json {{"error", std::string {error}}}.dump()};
    }
  }  // namespace

#ifdef _WIN32
  namespace {
    /// How often the worker looks at who's streaming
    constexpr auto k_poll = 5s;

    /// How long a helper whose output ended gets to exit before it's ended
    constexpr auto k_stop_wait = 5s;

    /// The helper holding one drive, and its answers
    struct helper_t {
      HANDLE process = nullptr;
      HANDLE input = nullptr;
      std::thread reader;
      std::deque<policy::result_t> results;  ///< Under state_mutex
      bool exited = false;  ///< Its output ended; under state_mutex
      bool answered = false;  ///< It said whether it mapped the drive
    };

    struct drive_t {
      char letter = 0;
      std::string share;
    };

    struct failure_t {
      std::string error;  ///< policy::key() of Windows' failure, or "no_user" or "helper"
      unsigned long code = 0;
    };

    /// One client's drive
    struct entry_t {
      std::string name;  ///< The client's, for the log
      std::string host;  ///< Where the drive points: unc_host() of the client's address
      std::shared_ptr<helper_t> helper;
      std::optional<drive_t> drive;  ///< What the helper mapped
      bool added = false;  ///< The drive was mapped for the client's last request
      std::optional<failure_t> failure;
      std::optional<policy::map_request_t> pending;  ///< A drive to map once no helper runs
      std::string pending_host;
      std::optional<policy::clock::time_point> ended;  ///< When the client's last stream ended
      std::optional<policy::clock::time_point> tried;  ///< When removing the drive last found files open
      bool removing = false;  ///< An unmap was sent and isn't answered yet
    };

    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::map<std::string, entry_t> entries;  ///< By client uuid
    bool worker_running = false;

    void forget(std::optional<policy::map_request_t> &request) {
      if (request) {
        policy::wipe(request->password);
        request.reset();
      }
    }

    std::string status_json(const entry_t &entry) {
      policy::status_t status;
      if (entry.pending || (entry.helper && !entry.drive && !entry.failure)) {
        status.state = policy::state_e::mapping;
      } else if (entry.drive) {
        status.state = policy::state_e::mapped;
        status.letter = entry.drive->letter;
        status.share = entry.drive->share;
        status.added = entry.added;
      } else if (entry.failure) {
        status.state = policy::state_e::failed;
        status.error = entry.failure->error;
        status.code = entry.failure->code;
      }
      return policy::status_json(status);
    }

    void read_output(std::shared_ptr<helper_t> helper, HANDLE output) {
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
            auto result = policy::parse_result(line);
            if (result.kind == policy::result_t::kind_e::none) {
              user_helper::log_line("Mac drive"sv, line);
            } else {
              std::lock_guard lg(state_mutex);
              helper->results.push_back(std::move(result));
              state_cv.notify_all();
            }
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
      helper->exited = true;
      state_cv.notify_all();
    }

    bool send(helper_t &helper, std::string line) {
      line += '\n';
      DWORD written = 0;
      return helper.input && WriteFile(helper.input, line.data(), (DWORD) line.size(), &written, nullptr) && written == line.size();
    }

    /// Lets go of a helper whose output ended, ending it if it doesn't exit
    void reap(helper_t &helper) {
      if (helper.input) {
        CloseHandle(helper.input);
        helper.input = nullptr;
      }
      if (helper.process) {
        if (WaitForSingleObject(helper.process, (DWORD) std::chrono::milliseconds(k_stop_wait).count()) == WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "fork: Mac drive helper didn't exit; ending it"sv;
          TerminateProcess(helper.process, 1);
          WaitForSingleObject(helper.process, 1000);
        }
        DWORD code = 0;
        if (GetExitCodeProcess(helper.process, &code) && code != 0) {
          BOOST_LOG(warning) << "fork: Mac drive helper exited with "sv << code;
        }
        CloseHandle(helper.process);
        helper.process = nullptr;
      }
      if (helper.reader.joinable()) {
        helper.reader.join();
      }
    }

    /// Starts a helper mapping the drive. The request's password is wiped either way.
    std::shared_ptr<helper_t> start_helper(const std::string &name, const std::string &host, policy::map_request_t &request, std::string &failure) {
      auto path = user_helper::path(L"apollo-mac-drive.exe");
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        BOOST_LOG(warning) << "fork: Mac drive: "sv << path.string() << " is missing, so LuzLunar's Mac can't be a drive"sv;
        policy::wipe(request.password);
        failure = "helper";
        return nullptr;
      }

      user_helper::process_t started;
      auto result = user_helper::start(path, "Mac drive"sv, started);
      if (result != user_helper::start_e::started) {
        policy::wipe(request.password);
        failure = result == user_helper::start_e::no_user ? "no_user" : "helper";
        return nullptr;
      }

      auto helper = std::make_shared<helper_t>();
      helper->process = (HANDLE) started.process;
      helper->input = (HANDLE) started.input;
      helper->reader = std::thread(read_output, helper, (HANDLE) started.output);

      auto line = policy::map_command('M', host, request);
      policy::wipe(request.password);
      auto ok = send(*helper, line);
      policy::wipe(line);
      if (!ok) {
        BOOST_LOG(error) << "fork: Mac drive: couldn't hand the helper its drive: error "sv << GetLastError();
        reap(*helper);
        failure = "helper";
        return nullptr;
      }

      BOOST_LOG(info) << "fork: Mac drive: mapping \\\\"sv << host << " for client ["sv << name << "] (helper PID "sv << GetProcessId(helper->process) << ')';
      return helper;
    }

    long long seconds_since(policy::clock::time_point then, policy::clock::time_point now) {
      return std::chrono::duration_cast<std::chrono::seconds>(now - then).count();
    }

    /// One of a helper's answers, under state_mutex
    void apply(entry_t &entry, const policy::result_t &result, policy::clock::time_point now) {
      using kind_e = policy::result_t::kind_e;
      switch (result.kind) {
        case kind_e::mapped:
          entry.helper->answered = true;
          entry.drive = drive_t {result.letter, result.share};
          entry.added = true;
          entry.failure.reset();
          BOOST_LOG(info) << "fork: Mac drive: "sv << result.letter << ": is \\\\"sv << entry.host << '\\' << result.share << " for client ["sv << entry.name << ']';
          // Asked for again while it was being mapped: this is the drive asked for
          if (entry.pending && entry.pending_host == entry.host) {
            forget(entry.pending);
          }
          break;
        case kind_e::failed:
          entry.helper->answered = true;
          entry.failure = failure_t {std::string {policy::key(policy::classify(result.code))}, result.code};
          BOOST_LOG(warning) << "fork: Mac drive: couldn't map \\\\"sv << entry.host << " for client ["sv << entry.name << "]: "sv << policy::describe(result.code);
          break;
        case kind_e::in_use:
          entry.removing = false;
          entry.tried = now;
          if (entry.drive) {
            BOOST_LOG(info) << "fork: Mac drive: "sv << entry.drive->letter << ": stays for now: "sv << policy::describe(result.code);
          }
          // The client asked for this drive again meanwhile, at the same address: it's still there
          if (entry.pending && entry.pending_host == entry.host) {
            forget(entry.pending);
            entry.added = false;
          }
          break;
        case kind_e::unmapped:
          entry.removing = false;
          if (entry.drive) {
            if (entry.pending) {
              BOOST_LOG(info) << "fork: Mac drive: removed "sv << entry.drive->letter << ": for client ["sv << entry.name << "]'s new request"sv;
            } else if (entry.ended) {
              BOOST_LOG(info) << "fork: Mac drive: removed "sv << entry.drive->letter << ": "sv << seconds_since(*entry.ended, now) << " s after client ["sv << entry.name << "]'s stream ended"sv;
            } else {
              BOOST_LOG(info) << "fork: Mac drive: removed "sv << entry.drive->letter << ':';
            }
          }
          entry.drive.reset();
          break;
        case kind_e::none:
          break;
      }
    }

    /**
     * @brief One thing that's due, done with the lock let go for anything slow.
     * @return Whether there was one; then the caller looks again.
     */
    bool step(std::unique_lock<std::mutex> &ul, policy::clock::time_point now) {
      // The helpers' answers
      for (auto &[uuid, entry] : entries) {
        if (entry.helper && !entry.helper->results.empty()) {
          auto result = std::move(entry.helper->results.front());
          entry.helper->results.pop_front();
          apply(entry, result, now);
          return true;
        }
      }

      // A helper that's done: after unmapping or failing, or ended by the user signing out
      for (auto &[uuid, entry] : entries) {
        if (entry.helper && entry.helper->exited) {
          auto helper = std::move(entry.helper);
          auto key = uuid;
          ul.unlock();
          reap(*helper);
          ul.lock();
          auto it = entries.find(key);
          if (it == entries.end()) {
            return true;
          }
          auto &e = it->second;
          if (e.drive) {
            // Nobody holds the drive now; a sign-out takes it, and otherwise it stays until one
            BOOST_LOG(warning) << "fork: Mac drive: the helper for "sv << e.drive->letter << ": stopped by itself"sv;
            e.drive.reset();
          } else if (!helper->answered && !e.pending) {
            e.failure = failure_t {"helper", 0};
          }
          e.removing = false;
          return true;
        }
      }

      // A drive to map, once the client's previous drive has gone
      for (auto &[uuid, entry] : entries) {
        if (entry.pending && !entry.helper) {
          auto key = uuid;
          auto name = entry.name;
          auto host = entry.pending_host;
          auto request = std::move(*entry.pending);
          entry.pending.reset();
          entry.host = host;
          entry.drive.reset();
          entry.failure.reset();
          entry.added = false;
          ul.unlock();
          std::string failure;
          auto helper = start_helper(name, host, request, failure);
          ul.lock();
          auto it = entries.find(key);
          if (it == entries.end()) {
            return true;
          }
          if (helper) {
            it->second.helper = std::move(helper);
          } else {
            it->second.failure = failure_t {failure, 0};
          }
          return true;
        }
      }

      // Drives to remove: one at an old address the client moved from, or a client that stopped streaming
      for (auto &[uuid, entry] : entries) {
        if (!entry.helper || !entry.drive || entry.removing) {
          continue;
        }
        auto replace = entry.pending && entry.pending_host != entry.host;
        auto decision = replace ? policy::removal_e::unmap_force :
                                  policy::removal(entry.ended ? std::optional {now - *entry.ended} : std::nullopt,
                                                  entry.tried ? std::optional {now - *entry.tried} : std::nullopt);
        if (decision == policy::removal_e::keep) {
          continue;
        }
        auto force = decision == policy::removal_e::unmap_force;
        BOOST_LOG(debug) << "fork: Mac drive: removing "sv << entry.drive->letter << ':' << (force ? " (forced)"sv : ""sv);
        if (send(*entry.helper, policy::unmap_command(force))) {
          entry.removing = true;
        } else {
          // Its input is gone, so it's exiting, and its exit is handled above
          entry.tried = now;
        }
        return true;
      }

      // Entries with nothing left: no drive, or a failure the client no longer streams to see
      for (auto it = entries.begin(); it != entries.end(); ++it) {
        auto &entry = it->second;
        if (!entry.helper && !entry.pending && !entry.drive && (!entry.failure || entry.ended)) {
          entries.erase(it);
          return true;
        }
      }
      return false;
    }

    /// Runs while any client has a drive or asked for one
    void worker() {
      std::unique_lock ul(state_mutex);
      auto next_poll = policy::clock::now();
      while (!entries.empty()) {
        if (policy::clock::now() >= next_poll) {
          ul.unlock();
          auto uuids = rtsp_stream::get_all_session_uuids();
          ul.lock();
          auto now = policy::clock::now();
          next_poll = now + k_poll;
          for (auto &[uuid, entry] : entries) {
            if (std::find(uuids.begin(), uuids.end(), uuid) != uuids.end()) {
              entry.ended.reset();
              entry.tried.reset();
            } else if (!entry.ended) {
              entry.ended = now;
            }
          }
        }

        if (step(ul, policy::clock::now())) {
          continue;
        }
        state_cv.wait_until(ul, next_poll);
      }
      worker_running = false;
    }

    bool streaming(const std::string &uuid) {
      auto uuids = rtsp_stream::get_all_session_uuids();
      return std::find(uuids.begin(), uuids.end(), uuid) != uuids.end();
    }

    bool on_lan(const std::string &address) {
      try {
        return net::from_address(address) == net::LAN;
      } catch (...) {
        return false;
      }
    }

    std::string joined(const std::vector<std::string> &shares) {
      std::string text;
      for (const auto &share : shares) {
        text += text.empty() ? "" : ", ";
        text += share;
      }
      return text;
    }
  }  // namespace

  bool supported() {
    return mac_network_drive && platf::is_running_as_system();
  }

  reply_t handle(const client_t &client, const std::string &address, const std::string &method, const std::string &body) {
    if (!client.permitted) {
      BOOST_LOG(debug) << "Permission Network Drive denied for ["sv << client.name << ']';
      return error_reply(401, "permission");
    }
    if (!supported()) {
      return error_reply(404, "unsupported");
    }

    if (method == "GET") {
      std::lock_guard lg(state_mutex);
      auto it = entries.find(client.uuid);
      return {200, it == entries.end() ? policy::status_json({}) : status_json(it->second)};
    }
    if (method != "POST") {
      return error_reply(400, "method");
    }

    // Only for the client's own address, while it streams over the LAN: never an address it names
    if (!streaming(client.uuid)) {
      BOOST_LOG(debug) << "fork: Mac drive: client ["sv << client.name << "] isn't streaming"sv;
      return error_reply(403, "not_streaming");
    }
    if (!on_lan(address)) {
      BOOST_LOG(info) << "fork: Mac drive: client ["sv << client.name << "] asked from "sv << address << ", which isn't on the LAN"sv;
      return error_reply(403, "not_lan");
    }
    auto host = policy::unc_host(address);
    if (host.empty()) {
      BOOST_LOG(info) << "fork: Mac drive: client ["sv << client.name << "]'s address "sv << address << " can't be a drive's"sv;
      return error_reply(403, "address");
    }

    std::string problem;
    auto request = policy::parse_map_request(body, problem);
    if (!request) {
      BOOST_LOG(warning) << "fork: Mac drive: client ["sv << client.name << "] asked for a drive: "sv << problem;
      return error_reply(400, problem);
    }

    std::lock_guard lg(state_mutex);
    auto &entry = entries[client.uuid];
    entry.name = client.name;
    entry.ended.reset();
    entry.tried.reset();

    // The drive it asks for is there already, kept from its last stream
    if (entry.helper && entry.drive && !entry.removing && !entry.pending && entry.host == host) {
      policy::wipe(request->password);
      entry.added = false;
      return {200, status_json(entry)};
    }
    // Being mapped already
    if (entry.helper && !entry.drive && !entry.failure && !entry.pending && entry.host == host) {
      policy::wipe(request->password);
      return {202, status_json(entry)};
    }

    BOOST_LOG(info) << "fork: Mac drive: client ["sv << client.name << "] asks for its Mac's disk at \\\\"sv << host << " ("sv << joined(request->shares) << ')';
    forget(entry.pending);
    entry.pending = std::move(*request);
    entry.pending_host = host;
    entry.failure.reset();
    entry.added = false;
    if (!worker_running) {
      worker_running = true;
      std::thread(worker).detach();
    }
    state_cv.notify_all();
    return {202, status_json(entry)};
  }
#else
  bool supported() {
    return false;
  }

  reply_t handle(const client_t &client, const std::string &, const std::string &, const std::string &) {
    return client.permitted ? error_reply(404, "unsupported") : error_reply(401, "permission");
  }
#endif
}  // namespace fork_tweaks::mac_drive
