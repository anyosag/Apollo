/**
 * @file src/fork/updater.h
 * @brief Apollo's over-the-air updates: checks the fork's latest master release, downloads and
 * checks its installer, and hands it to tools/apollo-updater.exe to install while nobody streams.
 *
 * Apollo runs as SYSTEM under its service, so the installer needs no UAC prompt. The helper runs
 * from a copy outside Program Files, survives the installer stopping the service, checks the new
 * build comes up, and puts the previous one back if it doesn't. See FORK.md, "Updates".
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <string_view>

namespace fork_tweaks::updater {
  /**
   * @brief Record that this build started (for the helper) and start checking for updates.
   * @note [fork] Called from main() once the servers run.
   */
  void start();

  /**
   * @brief Stop checking, before Apollo exits.
   */
  void stop();

  /**
   * @brief Whether this Apollo can update itself: it runs as SYSTEM under the service.
   */
  bool supported();

  /**
   * @brief The updater's state as JSON, for `/actions/update` (LuzLunar) and `/api/update` (Web UI).
   */
  std::string status_json();

  /**
   * @brief Whether an update is installing, or about to (the app quitting, the helper starting), so
   * Apollo is about to stop (host_sleep.h waits).
   */
  bool installing();

  /**
   * @brief Ask the updater to do something.
   * @param action "check", "install" or empty.
   * @param automatic When set, turn automatic updates on or off (saved as `fork_auto_update`).
   * @param error Set to why the request was refused.
   * @return Whether it was accepted.
   */
  bool request(std::string_view action, std::optional<bool> automatic, std::string *error);
}  // namespace fork_tweaks::updater
