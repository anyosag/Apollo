/**
 * @file src/fork/mac_drive.h
 * @brief Fork-only: LuzLunar's Mac as a network drive on this PC, during LAN streams.
 *
 * When a stream goes over the LAN, LuzLunar posts its Mac's account and the shares to try to
 * /actions/drive (mac_drive_policy.h has the rules). A drive letter belongs to the logon session
 * that maps it and Apollo runs as SYSTEM, so tools\apollo-mac-drive.exe maps it as the signed-in
 * user, unelevated, in the session Explorer runs in (user_helper.h). That helper lives as long as
 * the drive: Apollo tells it to remove the drive a minute after the client's last stream ends, and
 * it removes it by itself when Apollo stops.
 */
#pragma once

// standard includes
#include <string>

namespace fork_tweaks::mac_drive {
  /// The paired client asking, from nvhttp's verified certificate
  struct client_t {
    std::string name;
    std::string uuid;
    bool permitted = false;  ///< It has the client_drive permission
  };

  struct reply_t {
    int status = 200;
    std::string body;  ///< JSON
  };

  /// Whether /actions/drive works: `fork_mac_drive` is on and Apollo runs as SYSTEM, so it can map
  /// the drive in the signed-in user's session
  bool supported();

  /**
   * @brief GET and POST /actions/drive. GET reports the client's drive. POST {"action": "map", ...}
   * maps one at the client's address while the client streams over the LAN, in the background: the
   * reply says "mapping", and LuzLunar asks again. Never waits for Windows.
   * @param address The client's address, as nvhttp normalizes it.
   */
  reply_t handle(const client_t &client, const std::string &address, const std::string &method, const std::string &body);
}  // namespace fork_tweaks::mac_drive
