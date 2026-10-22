/**
 * @file src/fork/nv_stream.h
 * @brief Fork-only: NVIDIA's frame cap, V-Sync off and Smooth Motion while a client streams, in the
 * driver's global profile from a stream's launch until its app quits (nv_stream_policy.h).
 */
#pragma once

// standard includes
#include <cstdint>
#include <mutex>
#include <string>

namespace fork_tweaks::nv_stream {
  /**
   * @brief Give the driver's global profile what a stream of this app wants, and put back what an
   * earlier stream changed that this one doesn't want. Does nothing without an NVIDIA driver, and
   * puts everything back with `fork_nv_stream_settings` off.
   * @param app_uuid The app launched or streamed; its `fork-nv-cap` and `fork-smooth-motion` decide.
   * @param stream_mhz The stream's rate in millihertz.
   * @return Whether games are capped at the stream's rate now.
   */
  bool apply(const std::string &app_uuid, uint32_t stream_mhz);

  /**
   * @brief Put back what apply() changed. A value someone changed since stays theirs. If anything
   * couldn't be put back, the journal stays for the next try.
   */
  void restore();

  /**
   * @brief Put back what a run that didn't end properly left behind, from the journal in
   * %ProgramData%\\Sunshine. At startup, and from `--restore-nvprefs-undo` at uninstall.
   */
  void recover();

  /**
   * @brief NVAPI's wrapper (platform/windows/nvprefs) loads and unloads the library in shared
   * state, so nvprefs and this module take turns.
   */
  std::mutex &nvapi_mutex();
}  // namespace fork_tweaks::nv_stream
