/**
 * @file src/fork/user_helper.cpp
 * @brief Fork-only: Apollo's helper programs in tools\, run as the signed-in user.
 */
// local includes
#include "src/logging.h"
#include "user_helper.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <userenv.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::user_helper {
#ifdef _WIN32
  namespace {
    bool launch(const std::filesystem::path &path, std::string_view what, HANDLE input, HANDLE output, HANDLE &process_out, start_e &result) {
      auto list = platf::allocate_proc_thread_attr_list(1);
      if (!list) {
        return false;
      }
      HANDLE handles[] {input, output};
      UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);

      WCHAR desktop[] = L"winsta0\\default";
      STARTUPINFOEXW startup {};
      startup.StartupInfo.cb = sizeof(startup);
      startup.StartupInfo.lpDesktop = desktop;
      startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
      startup.StartupInfo.hStdInput = input;
      startup.StartupInfo.hStdOutput = output;
      startup.StartupInfo.hStdError = output;
      startup.lpAttributeList = list;

      auto command = L"\"" + path.wstring() + L"\"";
      auto dir = path.parent_path().wstring();
      // Out of the service's job, so stopping the service doesn't end the helper: it sees Apollo end when
      // its input closes and finishes up then (a drive is removed), and what it starts (TabTip.exe)
      // isn't ended with Apollo
      DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;
      PROCESS_INFORMATION process {};
      BOOL ok = FALSE;
      DWORD launch_error = 0;
      for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (platf::is_running_as_system()) {
          HANDLE token = platf::retrieve_users_token(false);
          if (!token) {
            BOOST_LOG(warning) << "fork: "sv << what << ": nobody is signed in to start the helper for"sv;
            platf::free_proc_thread_attr_list(list);
            result = start_e::no_user;
            return false;
          }
          void *environment = nullptr;
          CreateEnvironmentBlock(&environment, token, FALSE);
          ok = CreateProcessAsUserW(token, path.c_str(), command.data(), nullptr, nullptr, TRUE, flags, environment, dir.c_str(), &startup.StartupInfo, &process);
          launch_error = GetLastError();
          if (environment) {
            DestroyEnvironmentBlock(environment);
          }
          CloseHandle(token);
        } else {
          ok = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, flags & ~CREATE_UNICODE_ENVIRONMENT, nullptr, dir.c_str(), &startup.StartupInfo, &process);
          launch_error = GetLastError();
        }
        // A job that doesn't allow leaving it: the helper stays in it
        if (!ok && launch_error == ERROR_ACCESS_DENIED && (flags & CREATE_BREAKAWAY_FROM_JOB)) {
          flags &= ~CREATE_BREAKAWAY_FROM_JOB;
          continue;
        }
        break;
      }
      platf::free_proc_thread_attr_list(list);

      if (!ok) {
        BOOST_LOG(error) << "fork: "sv << what << ": couldn't start "sv << path.string() << ": error "sv << launch_error;
        return false;
      }
      CloseHandle(process.hThread);
      process_out = process.hProcess;
      return true;
    }
  }  // namespace

  std::filesystem::path path(const wchar_t *name) {
    WCHAR module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, _countof(module));
    return std::filesystem::path {module}.parent_path() / L"tools" / name;
  }

  start_e start(const std::filesystem::path &path, std::string_view what, process_t &started) {
    SECURITY_ATTRIBUTES inherit {sizeof(inherit), nullptr, TRUE};
    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &inherit, 0)) {
      BOOST_LOG(error) << "fork: "sv << what << ": couldn't make a pipe: error "sv << GetLastError();
      return start_e::failed;
    }
    if (!CreatePipe(&out_read, &out_write, &inherit, 0)) {
      BOOST_LOG(error) << "fork: "sv << what << ": couldn't make a pipe: error "sv << GetLastError();
      CloseHandle(in_read);
      CloseHandle(in_write);
      return start_e::failed;
    }
    // Apollo's ends stay here
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE process = nullptr;
    auto result = start_e::failed;
    auto ok = launch(path, what, in_read, out_write, process, result);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!ok) {
      CloseHandle(in_write);
      CloseHandle(out_read);
      return result;
    }

    started.process = process;
    started.input = in_write;
    started.output = out_read;
    return start_e::started;
  }
#else
  std::filesystem::path path(const wchar_t *) {
    return {};
  }

  start_e start(const std::filesystem::path &, std::string_view, process_t &) {
    return start_e::failed;
  }
#endif

  void log_line(std::string_view what, std::string_view line) {
    auto colon = line.find(": ");
    auto level = colon == std::string_view::npos ? "info"sv : line.substr(0, colon);
    auto message = colon == std::string_view::npos ? line : line.substr(colon + 2);
    if (level == "debug") {
      BOOST_LOG(debug) << "fork: "sv << what << ": "sv << message;
    } else if (level == "warning") {
      BOOST_LOG(warning) << "fork: "sv << what << ": "sv << message;
    } else if (level == "error") {
      BOOST_LOG(error) << "fork: "sv << what << ": "sv << message;
    } else {
      BOOST_LOG(info) << "fork: "sv << what << ": "sv << message;
    }
  }
}  // namespace fork_tweaks::user_helper
