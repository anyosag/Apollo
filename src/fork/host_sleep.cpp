/**
 * @file src/fork/host_sleep.cpp
 * @brief Putting this PC to sleep from LuzLunar.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

// local includes
#include "fork_config.h"
#include "host_sleep.h"
#include "host_sleep_policy.h"
#include "src/crypto.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "updater.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <powrprof.h>
#endif

using namespace std::literals;

namespace fork_tweaks::host_sleep {
  namespace policy = host_sleep_policy;

  namespace {
    reply_t refusal(int status, std::string_view error, const std::string &app = {}) {
      return {status, policy::refusal_json(error, app)};
    }
  }  // namespace

#ifdef _WIN32
  namespace {
    std::mutex tokens_mutex;
    policy::tokens_t tokens;

    /// A request to sleep is being carried out
    std::atomic<bool> sleeping {false};

    /// Whether Windows offers sleep (S1–S3); Modern Standby PCs only offer S0 low power idle
    bool sleep_states() {
      static const bool offered = [] {
        SYSTEM_POWER_CAPABILITIES capabilities {};
        if (!GetPwrCapabilities(&capabilities)) {
          BOOST_LOG(warning) << "fork: Sleep: couldn't read this PC's power capabilities: "sv << GetLastError();
          return false;
        }
        auto offered = capabilities.SystemS1 || capabilities.SystemS2 || capabilities.SystemS3;
        BOOST_LOG(info) << "fork: Sleep: this PC "sv << (offered ? "can sleep"sv : "has no sleep state LuzLunar can use (Modern Standby only?)"sv)
                        << " (S1 "sv << (bool) capabilities.SystemS1 << ", S2 "sv << (bool) capabilities.SystemS2
                        << ", S3 "sv << (bool) capabilities.SystemS3 << ')';
        return offered;
      }();
      return offered;
    }

    bool streaming() {
      return !rtsp_stream::get_all_session_uuids().empty();
    }

    /// What the PC is doing now; on nvhttp's thread, which also launches and quits apps
    policy::facts_t facts() {
      policy::facts_t facts;
      facts.streaming = streaming();
      facts.installing = updater::installing();
      facts.sleeping = sleeping;
      facts.app_running = proc::proc.running() > 0;
      if (facts.app_running) {
        facts.app = proc::proc.get_last_run_app_name();
      }
      return facts;
    }

    bool enable_shutdown_privilege() {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
      }
      TOKEN_PRIVILEGES privileges {};
      privileges.PrivilegeCount = 1;
      privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      auto ok = LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privileges.Privileges[0].Luid) &&
                AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
      CloseHandle(token);
      return ok;
    }

    /// Sleeps once the answer is out, unless a stream or an update started meanwhile
    void sleep_soon(std::string client) {
      std::thread([client = std::move(client)]() {
        std::this_thread::sleep_for(policy::settle);
        if (streaming() || updater::installing()) {
          BOOST_LOG(info) << "fork: Sleep: not sleeping after all for client ["sv << client << "]: "sv << (streaming() ? "a stream started"sv : "an update is installing"sv);
          sleeping = false;
          return;
        }
        if (!enable_shutdown_privilege()) {
          BOOST_LOG(warning) << "fork: Sleep: couldn't enable the shutdown privilege: "sv << GetLastError();
        }
        BOOST_LOG(info) << "fork: Sleep: putting this PC to sleep for client ["sv << client << ']';
        // Wake events stay on, so Wake-on-LAN and the keyboard still wake it. Returns after waking.
        if (SetSuspendState(FALSE, FALSE, FALSE)) {
          BOOST_LOG(info) << "fork: Sleep: this PC woke up"sv;
        } else {
          BOOST_LOG(error) << "fork: Sleep: Windows didn't sleep: "sv << GetLastError();
        }
        sleeping = false;
      }).detach();
    }

    std::string new_token() {
      return crypto::rand_alphabet(32, "0123456789abcdef"sv);
    }
  }  // namespace

  bool supported() {
    return client_sleep && sleep_states();
  }

  reply_t handle(const client_t &client, const std::string &method, const std::string &body) {
    if (!client.permitted) {
      BOOST_LOG(debug) << "Permission Sleep PC denied for ["sv << client.name << ']';
      return refusal(401, "permission");
    }
    if (!supported()) {
      return refusal(404, "unsupported");
    }

    if (method == "GET") {
      auto now = facts();
      std::string token;
      if (policy::available(now)) {
        token = new_token();
        std::lock_guard lock {tokens_mutex};
        tokens.issue(client.uuid, token, policy::clock::now());
      }
      return {200, policy::status_json(now, token)};
    }
    if (method != "POST") {
      return refusal(400, "method");
    }

    std::string problem;
    auto request = policy::parse_request(body, problem);
    if (!request) {
      return refusal(400, problem);
    }
    {
      std::lock_guard lock {tokens_mutex};
      if (!tokens.redeem(client.uuid, request->token, policy::clock::now())) {
        return refusal(403, "token");
      }
    }

    auto now = facts();
    auto reason = policy::blocker(now, request->quit_app);
    if (reason != policy::reason_e::none) {
      BOOST_LOG(info) << "fork: Sleep: client ["sv << client.name << "] asked, but "sv << policy::key(reason);
      return refusal(409, policy::key(reason), reason == policy::reason_e::app_running ? now.app : std::string {});
    }
    if (sleeping.exchange(true)) {
      return refusal(409, "sleeping");
    }

    // Quit, as LuzLunar's Quit does (nvhttp's /cancel); on this thread, which also launches apps
    if (now.app_running) {
      BOOST_LOG(info) << "fork: Sleep: quitting "sv << now.app << " first, for client ["sv << client.name << ']';
      proc::proc.terminate();
      display_device::revert_configuration();
    }

    sleep_soon(client.name);
    return {202, policy::accepted_json(now.app_running)};
  }
#else
  bool supported() {
    return false;
  }

  reply_t handle(const client_t &client, const std::string &, const std::string &) {
    return client.permitted ? refusal(404, "unsupported") : refusal(401, "permission");
  }
#endif
}  // namespace fork_tweaks::host_sleep
