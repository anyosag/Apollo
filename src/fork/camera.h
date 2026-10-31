/**
 * @file src/fork/camera.h
 * @brief Fork-only: LuzLunar's Mac camera as this PC's "Mac Camera" (camera_policy.h, luzcamera.h).
 *
 * At startup Apollo makes the shared memory the camera's media source reads, and has
 * tools\apollo-camera.exe register "Mac Camera" with Windows (Windows 11's virtual camera API),
 * or remove it with `fork_camera` off. The camera is always listed; without a Mac it shows a
 * placeholder.
 *
 * During a stream that LuzLunar launched with its camera on (luzCamera=1), it upgrades a request
 * on the HTTPS port to its camera connection (Simple-Web-Server's on_upgrade, after nvhttp verified
 * its paired certificate). The connection needs the Camera permission and the client's running
 * stream, and lasts until either ends. Its I/O stays on nvhttp's thread, in short handlers that
 * only queue; a thread of the camera's own follows what the camera's apps want, tells the Mac to
 * start and stop, and decodes its H.264 with Windows' decoder into the shared memory.
 */
#pragma once

// standard includes
#include <memory>
#include <string>

// local includes
#include "luzcamera.h"

namespace nvhttp {
  class SunshineHTTPS;
}

namespace fork_tweaks::camera {
  /// The camera works: `fork_camera` on, Windows 11, and its shared memory made
  bool supported();

  /// Once the config is read, before nvhttp starts: the camera's shared memory, its thread, and its
  /// registration with Windows (or its removal with `fork_camera` off)
  void start();

  /// At exit: the camera's thread ends; the source shows its own icon once Apollo's frames go stale
  void stop();

  /// What nvhttp knows of an upgrade request
  struct request_t {
    std::string path;
    std::string upgrade;  ///< its Upgrade header
    bool verified = false;  ///< a paired client's certificate
    bool permitted = false;  ///< with the Camera permission
    std::string client_name;
    std::string client_uuid;
  };

  /**
   * @brief Takes over a connection whose request asked for an upgrade, on nvhttp's thread. Answers
   * 101 and keeps it as the camera's connection, or answers why not (404 for another path or
   * protocol or with the camera off, 401 without the Camera permission, 403 without a stream that
   * brings its camera, 409 while another client's camera is connected) and closes it.
   */
  void upgrade(std::unique_ptr<nvhttp::SunshineHTTPS> socket, const request_t &request);
}  // namespace fork_tweaks::camera
