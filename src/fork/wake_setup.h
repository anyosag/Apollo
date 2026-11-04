/**
 * @file src/fork/wake_setup.h
 * @brief Turning this PC on from off with LuzLunar's Wake PC, through `/actions/wake`: checks the
 * network adapter LuzLunar reaches the PC through and Fast Startup, and fixes them on request
 * (rules in wake_setup_policy.h).
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks::wake_setup {
  /// The client asking, as nvhttp knows it
  struct client_t {
    std::string name;
    std::string uuid;
    bool permitted = false;  ///< It has the Sleep and Shut Down PC permission
  };

  struct reply_t {
    int status = 200;
    std::string body;  ///< JSON
  };

  /// Whether LuzLunar can check and fix this PC's wake settings: `fork_host_wake_setup` is on, on Windows
  bool supported();

  /// At startup: reads and logs how the PC was last powered on (SMBIOS), which GET then reports
  void start();

  /**
   * @brief Answers GET or POST /actions/wake.
   *
   * GET: 200 with the checks for the adapter that has `local_address` (the address the client
   * reached), how the PC was last powered on, its uptime, and a token for one POST within 30
   * seconds when there is something to fix or undo. POST {"token", "action": "fix"|"undo"}: 202,
   * then, a moment later, the journal is saved, the settings change and the adapter restarts.
   * 400 for a bad request, 401 without the permission, 403 for a stale or wrong token, 404 when
   * unsupported, 409 while something streams, an update installs, a change is running, or there is
   * nothing to do.
   */
  reply_t handle(const client_t &client, const std::string &local_address, const std::string &method, const std::string &body);
}  // namespace fork_tweaks::wake_setup
