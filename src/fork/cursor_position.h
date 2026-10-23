/**
 * @file src/fork/cursor_position.h
 * @brief Fork-only: where the pointer is on the display a stream shows, for LuzLunar
 * (GET /actions/cursor).
 *
 * In LuzLunar's Mac trackpad mode the Mac moves Windows' pointer, which is drawn in the picture
 * and can't leave it for the notch bar above the picture on the Mac's screen. So the Mac follows
 * the pointer by what it sends (Windows moves it a pixel a count then, pointer_speed.h) and, when
 * it's pushed up against the top of the display, asks here: only the PC knows for sure, as an app
 * may have moved the pointer since. The Mac's own pointer goes up into the bar only once this says
 * the pointer is at the top.
 */
#pragma once

// standard includes
#include <optional>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

namespace fork_tweaks::cursor_position {
  /// The display a stream shows, in the desktop's pixels from the virtual screen's top-left
  /// corner, as the capture has it (platf::display_t's offset_x, offset_y, width and height)
  struct display_t {
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;
  };

  /// The pointer on that display, from its top-left corner. Outside 0..width-1 or 0..height-1
  /// while the pointer is on another display.
  struct position_t {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool shown = false;  ///< Windows draws it: a game may hide it
  };

  /// A point from the virtual screen's top-left corner, on the display; none before a display is known
  inline std::optional<position_t> on_display(const display_t &display, int virtual_x, int virtual_y, bool shown) {
    if (display.width <= 0 || display.height <= 0) {
      return std::nullopt;
    }
    return position_t {virtual_x - display.offset_x, virtual_y - display.offset_y, display.width, display.height, shown};
  }

  inline std::string position_json(const position_t &position) {
    return nlohmann::json {
      {"x", position.x},
      {"y", position.y},
      {"width", position.width},
      {"height", position.height},
      {"shown", position.shown},
    }
      .dump();
  }

  // cursor_position.cpp

  /// The display the capture shows now (video.cpp, each time it sets one up)
  void set_display(const display_t &display);

  /// A paired client asking about its own stream
  struct client_t {
    std::string name;
    std::string uuid;
    bool permitted = false;  ///< It may view streams (View or Launch)
  };

  /// An HTTP status and a JSON body
  struct reply_t {
    int status = 200;
    std::string body;
  };

  /// The pointer can be read here (Windows)
  bool supported();

  /**
   * @brief GET /actions/cursor: {"x", "y", "width", "height", "shown"} for the client's own stream.
   * 403 without one, 503 while Windows won't say (a secure desktop has the input).
   */
  reply_t handle(const client_t &client, const std::string &method);
}  // namespace fork_tweaks::cursor_position
