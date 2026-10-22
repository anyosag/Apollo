/**
 * @file src/fork/user_helper.h
 * @brief Fork-only: Apollo's helper programs in tools\, run as the signed-in user.
 *
 * Apollo runs as SYSTEM, but some work only counts in the signed-in user's session: Windows' touch
 * keyboard only answers that user's callers (touch_keyboard.h), and a drive letter belongs to the
 * logon session that maps it (mac_drive.h). That work is done by a program in tools\, started as
 * the user, unelevated (the token Explorer runs with), on the user's desktop and outside the
 * service's job, with its standard input and output connected to Apollo.
 */
#pragma once

// standard includes
#include <filesystem>
#include <string_view>

namespace fork_tweaks::user_helper {
  /// tools\<name>, next to Apollo's executable
  std::filesystem::path path(const wchar_t *name);

  /// A started helper: its process, and Apollo's ends of its input and output (Windows handles)
  struct process_t {
    void *process = nullptr;
    void *input = nullptr;  ///< Apollo writes to it
    void *output = nullptr;  ///< Apollo reads from it
  };

  enum class start_e {
    started,
    no_user,  ///< Nobody is signed in to run it for
    failed,
  };

  /**
   * @brief Starts a helper as the signed-in user, or as Apollo's own user when Apollo doesn't run
   * as SYSTEM. Why it couldn't is logged as `fork: <what>: …`.
   * @return started, and then @p started holds three handles the caller closes.
   */
  start_e start(const std::filesystem::path &path, std::string_view what, process_t &started);

  /// Logs one of a helper's lines, "<level>: <message>", as `fork: <what>: <message>`
  void log_line(std::string_view what, std::string_view line);
}  // namespace fork_tweaks::user_helper
