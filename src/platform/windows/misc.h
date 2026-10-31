/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <string_view>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);

  /**
   * @brief Moves the calling thread to the input desktop, which it then keeps open while it's on it.
   * @return The input desktop, for telling whether it changed since the last call (closed if the
   * thread couldn't move to it), or nullptr if it couldn't be opened.
   */
  HDESK syncThreadDesktop();

  /**
   * @brief [fork] Puts the calling thread on the input desktop until destroyed, then back on the
   * desktop it was on (after Sunshine #5892). Windows' display configuration works only from a thread
   * on the input desktop, which on the lock screen is Winlogon's, so display changes made under one
   * work there too. Does nothing where Windows won't move the thread (one with windows or hooks) or
   * the input desktop can't be opened (Apollo not running as SYSTEM).
   */
  class input_desktop_guard_t {
  public:
    input_desktop_guard_t();
    ~input_desktop_guard_t();

    input_desktop_guard_t(const input_desktop_guard_t &) = delete;
    input_desktop_guard_t &operator=(const input_desktop_guard_t &) = delete;

  private:
    HDESK previous_desktop;
    HDESK input_desktop;
  };

  // [fork] Defined in misc.cpp; src/fork/touch_keyboard.cpp starts its helper as the signed-in user with them
  HANDLE retrieve_users_token(bool elevated);
  bool is_running_as_system();
  LPPROC_THREAD_ATTRIBUTE_LIST allocate_proc_thread_attr_list(DWORD attribute_count);
  void free_proc_thread_attr_list(LPPROC_THREAD_ATTRIBUTE_LIST list);

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring from_utf8(const std::string_view &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string to_utf8(const std::wstring_view &string);
}  // namespace platf
