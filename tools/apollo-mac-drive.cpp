/**
 * @file tools/apollo-mac-drive.cpp
 * @brief [fork] Maps LuzLunar's Mac as a network drive for the signed-in user, and removes it again
 * (src/fork/mac_drive.cpp starts it).
 *
 * Apollo starts this as the signed-in user, unelevated, so the drive lands in the logon session
 * Explorer runs in, and writes one JSON object per line to its standard input
 * (src/fork/mac_drive_policy.h):
 *   {"cmd": "map", "letter", "host", "shares", "user", "password", "label"}
 *       first, and only once: map the first of the shares the Mac offers
 *   {"cmd": "unmap"}, {"cmd": "unmap", "force": true}
 *       remove the drive, unless files are open on it (or even then)
 * It answers with one JSON object per line ({"result": "mapped" | "failed" | "in-use" |
 * "unmapped", ...}), among log lines that start with their level ("info: …"). The end of its input
 * means Apollo is done: the drive is removed, even with files open, and it exits.
 *
 * The drive is a temporary connection: Windows doesn't remember it, so it never comes back at
 * sign-in, and signing out removes it. It's always mapped with the Mac's account and password,
 * never without them, so Windows can't offer the PC user's own credentials to the Mac instead.
 * Explorer shows it under the Mac's name (MountPoints2's _LabelFromReg).
 */
#ifndef WINVER
  #define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00
#endif

// standard includes
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

// platform includes
#include <windows.h>
#include <shlobj.h>
#include <winnetwk.h>

// local includes
#include "src/fork/mac_drive_policy.h"

using namespace std::literals;
namespace mdp = mac_drive_policy;

namespace {
  /// One log line for Apollo: "<level>: <message>"
  void say(const char *level, std::string message) {
    // Share names come from the Mac: no control characters, which could start a line of their own
    for (auto &c : message) {
      if ((unsigned char) c < 0x20 || c == 0x7F) {
        c = '?';
      }
    }
    std::printf("%s: %s\n", level, message.c_str());
    std::fflush(stdout);
  }

  /// One answer for Apollo
  void answer(const std::string &line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  }

  std::wstring wide(std::string_view text) {
    if (text.empty()) {
      return {};
    }
    auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), result.data(), size);
    return result;
  }

  void wipe(std::wstring &text) {
    SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    text.clear();
  }

  std::wstring drive_name(char letter) {
    return {(wchar_t) letter, L':'};
  }

  constexpr auto k_mount_points = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2\\";

  /// Explorer's name for the drive, set before it's mapped so Explorer shows it from the start
  void set_label(const std::string &host, const std::string &share, const std::string &label) {
    auto key_path = k_mount_points + wide(mdp::label_key(host, share));
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
      say("warning", "couldn't name the drive for Explorer");
      return;
    }
    auto name = wide(label);
    RegSetValueExW(key, L"_LabelFromReg", 0, REG_SZ, (const BYTE *) name.c_str(), (DWORD) ((name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
  }

  void clear_label(const std::string &host, const std::string &share) {
    auto key_path = k_mount_points + wide(mdp::label_key(host, share));
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
      RegDeleteValueW(key, L"_LabelFromReg");
      RegCloseKey(key);
    }
  }

  void tell_explorer(LONG event, char letter) {
    wchar_t root[] = {(wchar_t) letter, L':', L'\\', L'\0'};
    SHChangeNotify(event, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, root, nullptr);
  }

  /// The letter a network drive to this UNC path already has, from a helper that ended without removing it
  std::optional<char> existing_letter(const std::wstring &unc) {
    for (char letter = 'D'; letter <= 'Z'; letter++) {
      wchar_t remote[MAX_PATH + 1];
      DWORD size = (DWORD) std::size(remote);
      if (WNetGetConnectionW(drive_name(letter).c_str(), remote, &size) == NO_ERROR && CompareStringOrdinal(remote, -1, unc.c_str(), -1, TRUE) == CSTR_EQUAL) {
        return letter;
      }
    }
    return std::nullopt;
  }

  struct mapped_t {
    char letter = 0;
    std::string share;
  };

  /// Maps the first share the Mac offers; answers Apollo either way
  std::optional<mapped_t> map(mdp::command_t &command) {
    auto user = wide(command.request.user);
    auto password = wide(command.request.password);
    mdp::wipe(command.request.password);

    std::optional<mapped_t> mapped;
    unsigned long last_error = 0;
    std::string last_share;
    for (const auto &share : command.request.shares) {
      auto unc = wide(mdp::unc_path(command.host, share));
      last_share = share;

      if (auto letter = existing_letter(unc)) {
        say("info", "\\\\" + command.host + '\\' + share + " is drive " + *letter + ": already");
        set_label(command.host, share, command.request.label);
        mapped = mapped_t {*letter, share};
        break;
      }

      // The letters in use, and those Windows refused because it remembers them for another drive
      std::uint32_t refused = 0;
      DWORD error = ERROR_NO_MORE_DEVICES;
      while (true) {
        auto letter = mdp::choose_letter(GetLogicalDrives() | refused, command.letter);
        if (!letter) {
          error = ERROR_NO_MORE_DEVICES;
          break;
        }
        auto local = drive_name(letter);
        NETRESOURCEW resource {};
        resource.dwType = RESOURCETYPE_DISK;
        resource.lpLocalName = local.data();
        resource.lpRemoteName = unc.data();
        set_label(command.host, share, command.request.label);
        error = WNetAddConnection2W(&resource, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
        if (error == NO_ERROR) {
          mapped = mapped_t {letter, share};
          break;
        }
        clear_label(command.host, share);
        if (mdp::classify(error) != mdp::failure_e::letter_taken) {
          break;
        }
        refused |= 1u << (letter - 'A');
      }
      if (mapped) {
        break;
      }

      last_error = error;
      auto failure = mdp::classify(error);
      if (failure != mdp::failure_e::no_share && failure != mdp::failure_e::denied) {
        break;
      }
      say("info", share + ": " + mdp::describe(error) + "; trying the next share");
    }
    wipe(password);

    if (!mapped) {
      answer(mdp::failed_result(last_error, last_share));
      return std::nullopt;
    }
    tell_explorer(SHCNE_DRIVEADD, mapped->letter);
    answer(mdp::mapped_result(mapped->letter, mapped->share));
    return mapped;
  }

  /// @return Whether the drive is gone
  bool unmap(const mapped_t &mapped, const std::string &host, bool force) {
    auto error = WNetCancelConnection2W(drive_name(mapped.letter).c_str(), 0, force ? TRUE : FALSE);
    auto failure = mdp::classify(error);
    if (error != NO_ERROR && failure != mdp::failure_e::gone) {
      // The drive stays; Apollo tries again, forced in the end
      if (failure != mdp::failure_e::in_use) {
        say("warning", "couldn't remove the drive: " + mdp::describe(error));
      }
      answer(mdp::in_use_result(error));
      return false;
    }
    clear_label(host, mapped.share);
    tell_explorer(SHCNE_DRIVEREMOVED, mapped.letter);
    answer(mdp::unmapped_result());
    return true;
  }

  /// A line from Apollo; false at the end of its input
  bool read_line(std::string &line) {
    if (!std::getline(std::cin, line)) {
      return false;
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return true;
  }
}  // namespace

int main(int argc, char **) {
  if (argc > 1) {
    std::fprintf(stderr,
                 "Usage: apollo-mac-drive\n"
                 "Apollo starts it for LuzLunar's Mac drive and talks to it on its standard input and output;\n"
                 "see src/fork/mac_drive_policy.h for the lines.\n");
    return 2;
  }

  std::string line;
  if (!read_line(line)) {
    return 0;
  }
  auto command = mdp::parse_command(line);
  mdp::wipe(line);
  if (command.kind != mdp::command_t::kind_e::map) {
    say("error", "the first line wasn't a map command");
    return 2;
  }

  auto mapped = map(command);
  if (!mapped) {
    return 0;
  }

  while (read_line(line)) {
    auto next = mdp::parse_command(line);
    if (next.kind != mdp::command_t::kind_e::unmap) {
      say("warning", "unknown command");
      continue;
    }
    if (unmap(*mapped, command.host, next.force)) {
      return 0;
    }
  }

  // Apollo stopped: the drive goes with it
  unmap(*mapped, command.host, true);
  return 0;
}
