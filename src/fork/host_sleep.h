/**
 * @file src/fork/host_sleep.h
 * @brief Putting this PC to sleep from LuzLunar, through `/actions/sleep` (rules in
 * host_sleep_policy.h).
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks::host_sleep {
  /// The client asking, as nvhttp knows it
  struct client_t {
    std::string name;
    std::string uuid;
    bool permitted = false;  ///< It has the Sleep PC permission
  };

  struct reply_t {
    int status = 200;
    std::string body;  ///< JSON
  };

  /**
   * @brief Whether LuzLunar can put this PC to sleep: `fork_host_sleep` is on and Windows offers
   * sleep (S1–S3). PCs with only Modern Standby don't offer it.
   */
  bool supported();

  /**
   * @brief Answers GET or POST /actions/sleep.
   *
   * GET: 200 with whether the PC can sleep now, the app that's still running, and a token for
   * one POST within 30 seconds. POST {"token", "quitApp"}: 202, then the app is quit as Quit does
   * and the PC sleeps a moment later. 400 for a bad request, 401 without the permission, 403 for a
   * stale or wrong token, 404 when unsupported, 409 when something streams, an update installs,
   * or an app runs and the request didn't say to quit it.
   */
  reply_t handle(const client_t &client, const std::string &method, const std::string &body);
}  // namespace fork_tweaks::host_sleep
