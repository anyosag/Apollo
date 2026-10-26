/**
 * @file src/fork/debug_log.h
 * @brief Apollo's log for LuzLunar's Mac, through `/actions/log` (rules in debug_log_policy.h), so
 * a problem can be followed from the Mac without the web UI or a restart.
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks::debug_log {
  /// The client asking, as nvhttp knows it
  struct client_t {
    std::string name;
    std::string uuid;
    bool permitted = false;  ///< It has the Read Log permission
  };

  struct reply_t {
    int status = 200;
    std::string body;  ///< JSON
  };

  /// GET's query values, empty when absent
  struct query_t {
    std::string lines;
    std::string grep;
    std::string from;
  };

  /**
   * @brief Answers GET or POST /actions/log.
   *
   * GET: 200 {"level", "configuredLevel", "secondsLeft", "size", "next", "restarted", "lines"}, the
   * lines of this run's log the query asks for. POST {"level": name, "minutes": N}: the level for N
   * minutes (30 unless said, at most 240), then the configured one again; {"level": "configured"}
   * goes back now; {"mark": text} logs the text with the client's name. Both at once is fine. 400
   * for a bad request, 401 without the permission, 500 if the log can't be read.
   */
  reply_t handle(const client_t &client, const std::string &method, const query_t &query, const std::string &body);
}  // namespace fork_tweaks::debug_log
