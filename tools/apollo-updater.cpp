/**
 * @file tools/apollo-updater.cpp
 * @brief [fork] Installs an over-the-air update for Apollo (src/fork/updater.cpp starts it).
 *
 * Apollo starts this as SYSTEM, from a copy in config\updates and outside the service's job, so
 * it keeps running when the installer stops the service. It:
 *   1. checks the installer is still the one Apollo checked;
 *   2. runs it silently (it uninstalls the old build, keeps config, reinstalls the service);
 *   3. waits for the service to run, for Apollo to report the new build in started.json, and for
 *      Apollo's HTTP port to answer;
 *   4. otherwise puts the previous build back the same way, and as a last resort starts the service;
 *   5. writes result.json for Apollo to report, and logs to updater.log.
 * No Boost or OpenSSL: Windows only, so nothing can be missing when Program Files is half-installed.
 */
// standard includes
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

// platform includes
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

// local includes
#include "src/fork/update_manifest.h"

using namespace std::literals;
namespace fu = fork_update;

namespace {
  constexpr auto installer_timeout = 10min;
  constexpr auto start_timeout = 3min;
  constexpr auto service_name = L"ApolloService";

  struct options_t {
    std::filesystem::path dir;
    std::filesystem::path install_dir;
    std::filesystem::path installer;
    std::string sha256;
    int64_t run = 0;
    std::string version;
    std::string running_version;
    std::filesystem::path previous;
    std::string previous_sha256;
    int64_t previous_run = 0;
    std::string previous_version;
    bool test_fail_new = false;  ///< Treat the new build as not starting, to test rolling back
  };

  std::ofstream log_file;

  std::string to_utf8(const std::wstring &s) {
    if (s.empty()) {
      return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int) s.size(), out.data(), size, nullptr, nullptr);
    return out;
  }

  void log(const std::string &message) {
    auto now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    log_file << '[' << stamp << "] " << message << std::endl;
  }

  std::optional<std::string> sha256_file(const std::filesystem::path &path) {
    std::ifstream in {path, std::ios::binary};
    if (!in) {
      return std::nullopt;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
      return std::nullopt;
    }
    std::optional<std::string> result;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
      char buffer[64 * 1024];
      bool ok = true;
      while (in) {
        in.read(buffer, sizeof(buffer));
        if (in.gcount() > 0 && BCryptHashData(hash, (PUCHAR) buffer, (ULONG) in.gcount(), 0) != 0) {
          ok = false;
          break;
        }
      }
      UCHAR digest[32];
      if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string hex;
        for (UCHAR b : digest) {
          hex += digits[b >> 4];
          hex += digits[b & 0xF];
        }
        result = hex;
      }
      BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
  }

  /// Run a program and wait for it; its exit code, or nothing if it couldn't start or timed out
  std::optional<DWORD> run_and_wait(const std::filesystem::path &program, std::wstring command, std::chrono::milliseconds timeout) {
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
      log("couldn't start " + program.string() + " (error " + std::to_string(GetLastError()) + ")");
      return std::nullopt;
    }
    CloseHandle(process.hThread);
    std::optional<DWORD> code;
    if (WaitForSingleObject(process.hProcess, (DWORD) timeout.count()) == WAIT_OBJECT_0) {
      DWORD exit_code = 0;
      GetExitCodeProcess(process.hProcess, &exit_code);
      code = exit_code;
    } else {
      log(program.filename().string() + " is still running after " + std::to_string(timeout.count() / 1000) + " s; carrying on");
    }
    CloseHandle(process.hProcess);
    return code;
  }

  bool run_installer(const std::filesystem::path &installer) {
    log("running " + installer.string() + " /S");
    auto code = run_and_wait(installer, L"\"" + installer.wstring() + L"\" /S", installer_timeout);
    if (code) {
      log("the installer exited with " + std::to_string(*code));
    }
    return code.has_value();
  }

  bool service_running() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
      return false;
    }
    bool running = false;
    if (SC_HANDLE service = OpenServiceW(manager, service_name, SERVICE_QUERY_STATUS)) {
      SERVICE_STATUS status {};
      running = QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_RUNNING;
      CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return running;
  }

  /// Start the service; false if it isn't installed
  bool start_service() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
      return false;
    }
    bool exists = false;
    if (SC_HANDLE service = OpenServiceW(manager, service_name, SERVICE_START)) {
      exists = true;
      if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        log("couldn't start the service (error " + std::to_string(GetLastError()) + ")");
      }
      CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return exists;
  }

  bool process_alive(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
      return false;
    }
    bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
  }

  bool port_answers(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
      return false;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short) port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(s, (sockaddr *) &address, sizeof(address)) == 0;
    closesocket(s);
    return ok;
  }

  std::optional<fu::started_t> read_started(const options_t &o) {
    std::ifstream in {o.dir / "started.json", std::ios::binary};
    if (!in) {
      return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return fu::parse_started(ss.str());
  }

  /// Wait for the service to run Apollo at @p run, answering on its HTTP port
  bool comes_up(const options_t &o, int64_t run) {
    auto deadline = std::chrono::steady_clock::now() + start_timeout;
    std::string last;
    while (std::chrono::steady_clock::now() < deadline) {
      std::string now;
      auto started = read_started(o);
      if (!service_running()) {
        now = "the service isn't running";
      } else if (!started || started->run != run) {
        now = "Apollo hasn't started as run " + std::to_string(run) + " yet";
      } else if (!process_alive(started->pid)) {
        now = "Apollo (pid " + std::to_string(started->pid) + ") exited";
      } else if (!port_answers(started->port)) {
        now = "Apollo doesn't answer on port " + std::to_string(started->port) + " yet";
      } else {
        log("Apollo " + started->version + " is up (pid " + std::to_string(started->pid) + ")");
        return true;
      }
      if (now != last) {
        log(now);
        last = now;
      }
      std::this_thread::sleep_for(2s);
    }
    log("gave up waiting: " + last);
    return false;
  }

  void write_result(const options_t &o, fu::outcome_e outcome, const std::string &running_version, const std::string &message) {
    fu::result_t r;
    r.target_run = o.run;
    r.target_version = o.version;
    r.outcome = outcome;
    r.running_version = running_version;
    r.message = message;
    r.time = (int64_t) std::time(nullptr);
    log(fu::describe(r));

    auto path = o.dir / "result.json";
    auto temp = o.dir / "result.json.tmp";
    {
      std::ofstream out {temp, std::ios::binary | std::ios::trunc};
      out << fu::to_json(r);
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  }

  std::optional<options_t> parse_args() {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
      return std::nullopt;
    }
    options_t o;
    for (int i = 1; i < argc; i++) {
      std::wstring key = argv[i];
      if (key == L"--test-fail-new") {
        o.test_fail_new = true;
        continue;
      }
      if (i + 1 >= argc) {
        LocalFree(argv);
        return std::nullopt;
      }
      std::wstring value = argv[++i];
      if (key == L"--dir") {
        o.dir = value;
      } else if (key == L"--install-dir") {
        o.install_dir = value;
      } else if (key == L"--installer") {
        o.installer = value;
      } else if (key == L"--sha256") {
        o.sha256 = to_utf8(value);
      } else if (key == L"--run") {
        o.run = _wtoi64(value.c_str());
      } else if (key == L"--version") {
        o.version = to_utf8(value);
      } else if (key == L"--running-version") {
        o.running_version = to_utf8(value);
      } else if (key == L"--previous") {
        o.previous = value;
      } else if (key == L"--previous-sha256") {
        o.previous_sha256 = to_utf8(value);
      } else if (key == L"--previous-run") {
        o.previous_run = _wtoi64(value.c_str());
      } else if (key == L"--previous-version") {
        o.previous_version = to_utf8(value);
      }
    }
    LocalFree(argv);
    if (o.dir.empty() || o.installer.empty() || o.sha256.empty() || o.run <= 0) {
      return std::nullopt;
    }
    return o;
  }
}  // namespace

int main() {
  auto options = parse_args();
  if (!options) {
    std::fprintf(stderr, "Usage: apollo-updater --dir <updates> --install-dir <Apollo> --installer <exe> --sha256 <hex> --run <n> --version <v> --running-version <v> [--previous <exe> --previous-sha256 <hex> --previous-run <n> --previous-version <v>] [--test-fail-new]\n");
    return 64;
  }
  auto &o = *options;

  // Keep the log small: start over past 1 MB
  auto log_path = o.dir / "updater.log";
  std::error_code ec;
  bool fresh = std::filesystem::file_size(log_path, ec) > 1024 * 1024;
  log_file.open(log_path, std::ios::binary | (fresh ? std::ios::trunc : std::ios::app));
  log("updating Apollo " + o.running_version + " to " + o.version + " (run " + std::to_string(o.run) + ")");

  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  // 1. The installer must still be the one Apollo checked; nothing is touched otherwise
  if (sha256_file(o.installer) != o.sha256) {
    write_result(o, fu::outcome_e::failed, o.running_version, "the installer changed after Apollo checked it");
    return 1;
  }

  // 2-3. Install, and wait for the new build
  run_installer(o.installer);
  if (o.test_fail_new) {
    log("--test-fail-new: treating the new build as not starting");
  } else if (comes_up(o, o.run)) {
    write_result(o, fu::outcome_e::updated, o.version, {});
    return 0;
  }

  // 4. Put the previous build back
  if (!o.previous.empty() && o.previous_run > 0) {
    if (sha256_file(o.previous) != o.previous_sha256) {
      log("the previous installer doesn't match its SHA-256; not using it");
    } else {
      log("putting " + o.previous_version + " back");
      run_installer(o.previous);
      if (comes_up(o, o.previous_run)) {
        write_result(o, fu::outcome_e::rolled_back, o.previous_version, "the new build didn't start");
        return 2;
      }
    }
  } else {
    log("no previous installer to put back");
  }

  // Last resort: make sure the service exists and runs, whatever it runs
  if (!start_service()) {
    auto script = o.install_dir / "scripts" / "install-service.bat";
    log("the service isn't installed; running " + script.string());
    auto cmd = std::filesystem::path {_wgetenv(L"ComSpec") ? _wgetenv(L"ComSpec") : L"C:\\Windows\\System32\\cmd.exe"};
    run_and_wait(cmd, L"\"" + cmd.wstring() + L"\" /c \"" + script.wstring() + L"\"", 2min);
  }
  write_result(o, fu::outcome_e::failed, {}, "neither the new nor the previous build started; the service was started as a last resort");
  return 3;
}
