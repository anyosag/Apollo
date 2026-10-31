/**
 * @file src/fork/cursor_shape_policy.h
 * @brief Fork-only: the rules of LuzLunar's pointer drawn by the Mac (client_cursor.h): Desktop
 * Duplication's shapes in the format the client reads (luzcursor.h), when the capture leaves the
 * pointer out of the picture, and what the control stream tells the client and when.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "luzcursor.h"

namespace fork_tweaks::cursor_shape_policy {
  /// DXGI_OUTDUPL_POINTER_SHAPE_TYPE's values
  constexpr int shape_monochrome = 1;
  constexpr int shape_color = 2;
  constexpr int shape_masked_color = 4;

  /// While a client draws the pointer, the control stream runs at least this often (else 150 ms)
  constexpr std::chrono::milliseconds control_interval {5};
  /// A STATE whose only news is the position or the sums waits this long after the last one
  constexpr std::chrono::milliseconds position_interval {16};
  /// A capture that hasn't said what it does with the pointer for this long isn't drawing it for
  /// the client any more (Windows Graphics Capture never says; a capture that's restarting)
  constexpr std::chrono::milliseconds capture_stale {1000};

  namespace detail {
    inline void put_pixel(luzcursor::shape_t &shape, int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a) {
      std::size_t at = (static_cast<std::size_t>(y) * shape.width + x) * 4;
      shape.bgra[at] = b;
      shape.bgra[at + 1] = g;
      shape.bgra[at + 2] = r;
      shape.bgra[at + 3] = a;
    }
  }  // namespace detail

  /**
   * @brief Desktop Duplication's shape (GetFramePointerShape()) in luzcursor.h's format, hashed.
   * Monochrome is an AND mask over an XOR mask, a bit each, so its height is twice the pointer's:
   * AND 0 is drawn (XOR 0 black, 1 white), AND 1 is clear (XOR 1 inverts). Color is straight-alpha
   * BGRA. Masked color is BGRA whose alpha says how: 0 draws the color, 0xFF XORs it, which
   * leaves black clear and inverts with any other color. None for an unknown type, a size past
   * luzcursor::k_max_side or a buffer too short for it.
   */
  inline std::optional<luzcursor::shape_t> convert(int type, const std::uint8_t *data, std::size_t size, int width, int height, int pitch, int hotspot_x, int hotspot_y) {
    int rows = type == shape_monochrome ? height / 2 : height;
    if (data == nullptr || !luzcursor::valid_size(width, rows) || pitch <= 0) {
      return std::nullopt;
    }
    std::size_t needed = static_cast<std::size_t>(pitch) * (type == shape_monochrome ? rows * 2 : rows);
    if (size < needed) {
      return std::nullopt;
    }

    // Windows can report a hotspot on the image's far edge; the format keeps it inside
    luzcursor::shape_t shape = luzcursor::blank_shape(width, rows, std::clamp(hotspot_x, 0, width - 1), std::clamp(hotspot_y, 0, rows - 1));
    switch (type) {
      case shape_monochrome:
        {
          if (static_cast<std::size_t>(pitch) < luzcursor::mask_stride(width)) {
            return std::nullopt;
          }
          for (int y = 0; y < rows; ++y) {
            const std::uint8_t *and_row = data + static_cast<std::size_t>(y) * pitch;
            const std::uint8_t *xor_row = data + static_cast<std::size_t>(y + rows) * pitch;
            for (int x = 0; x < width; ++x) {
              std::uint8_t bit = static_cast<std::uint8_t>(0x80 >> (x % 8));
              bool and_bit = (and_row[x / 8] & bit) != 0;
              bool xor_bit = (xor_row[x / 8] & bit) != 0;
              if (and_bit && xor_bit) {
                luzcursor::set_inverts(shape, x, y);
              } else if (!and_bit) {
                std::uint8_t value = xor_bit ? 0xFF : 0;
                detail::put_pixel(shape, x, y, value, value, value, 0xFF);
              }
            }
          }
          break;
        }
      case shape_color:
      case shape_masked_color:
        {
          if (pitch < width * 4) {
            return std::nullopt;
          }
          for (int y = 0; y < rows; ++y) {
            const std::uint8_t *row = data + static_cast<std::size_t>(y) * pitch;
            for (int x = 0; x < width; ++x) {
              const std::uint8_t *pixel = row + static_cast<std::size_t>(x) * 4;
              if (type == shape_color) {
                detail::put_pixel(shape, x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
              } else if (pixel[3] == 0) {
                detail::put_pixel(shape, x, y, pixel[0], pixel[1], pixel[2], 0xFF);
              } else if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
                luzcursor::set_inverts(shape, x, y);
              }
            }
          }
          break;
        }
      default:
        return std::nullopt;
    }
    shape.hash = luzcursor::hash_of(shape);
    return shape;
  }

  /**
   * @brief The capture leaves the pointer out of the picture: every stream of the display draws it,
   * its shape could be converted, and the input isn't on a secure desktop (UAC, the lock screen),
   * where Windows may not move the pointer a pixel a count and the capture starts over.
   */
  inline bool client_draws(bool every_stream_draws, bool shape_ok, bool secure_desktop) {
    return every_stream_draws && shape_ok && !secure_desktop;
  }

  /**
   * @brief A Desktop Duplication frame that only moved the pointer needs no new picture while
   * clients draw the pointer: nothing in it changed. Not while the cursor pulse runs, while the
   * pointer is drawn, or when the last picture went out with the pointer in it, which has to go.
   */
  inline bool skip_pointer_only_frame(bool client_draws, bool new_picture, bool pulsing, bool blending, bool last_picture_without_pointer) {
    return client_draws && !new_picture && !pulsing && !blending && last_picture_without_pointer;
  }

  /// What the capture of the display with the pointer last said
  struct capture_t {
    bool fresh = false;  ///< Within capture_stale
    bool client_draws = false;  ///< It leaves the pointer out of the picture (client_draws())
    bool pointer_visible = false;  ///< Desktop Duplication draws it on this display
    bool hidden_by_host = false;  ///< Ctrl+Alt+Shift+N took it out of the stream
    std::uint64_t hash = 0;  ///< The shape it converted last; 0 for none
  };

  /// Where Windows has the pointer, on the display the stream shows
  struct pointer_t {
    bool known = false;
    int x = 0;  ///< The hotspot, from the display's top-left
    int y = 0;
    int width = 0;  ///< The display's size
    int height = 0;
    bool showing = false;  ///< No app hid it (CURSOR_SHOWING)
    bool clipped = false;  ///< An app holds it in a rectangle (ClipCursor())
    /// Where Windows keeps it, from the display's top-left (right and bottom exclusive): the
    /// rectangle an app holds it in, else the virtual screen, every display's bounds
    int clip_left = 0;
    int clip_top = 0;
    int clip_right = 0;
    int clip_bottom = 0;
  };

  inline bool on_display(const pointer_t &pointer) {
    return pointer.known && pointer.x >= 0 && pointer.y >= 0 && pointer.x < pointer.width && pointer.y < pointer.height;
  }

  /**
   * @brief The STATE for a stream: the host draws the pointer unless a fresh capture leaves it out
   * for the client, and then names no shape, so none is sent. The pointer shows when no app hid it,
   * and, on this display, when Desktop Duplication draws it.
   */
  inline luzcursor::state_t state_for(const capture_t &capture, const pointer_t &pointer, std::int64_t sum_dx, std::int64_t sum_dy) {
    luzcursor::state_t state;
    bool host_draws = !capture.fresh || !capture.client_draws || capture.hash == 0;
    bool here = on_display(pointer);
    bool visible = pointer.known && pointer.showing && !capture.hidden_by_host && (!here || capture.pointer_visible);
    state.flags = static_cast<std::uint8_t>((visible ? luzcursor::flag_visible : 0) | (here ? luzcursor::flag_on_display : 0) |
                                            (host_draws ? luzcursor::flag_host_draws : 0) | (pointer.clipped ? luzcursor::flag_clipped : 0));
    state.hash = host_draws ? 0 : capture.hash;
    state.x = pointer.x;
    state.y = pointer.y;
    state.width = pointer.width;
    state.height = pointer.height;
    state.sum_dx = sum_dx;
    state.sum_dy = sum_dy;
    state.clip_left = pointer.clip_left;
    state.clip_top = pointer.clip_top;
    state.clip_right = pointer.clip_right;
    state.clip_bottom = pointer.clip_bottom;
    return state;
  }

  /// When a stream gets a STATE: the first at once, then one for each change, those that only move
  /// the pointer at most every position_interval (the last of a move is never left out)
  class pacer_t {
  public:
    bool due(const luzcursor::state_t &state, std::chrono::steady_clock::time_point now) const {
      if (!last_) {
        return true;
      }
      if (state == *last_) {
        return false;
      }
      luzcursor::state_t moved = *last_;
      moved.x = state.x;
      moved.y = state.y;
      moved.sum_dx = state.sum_dx;
      moved.sum_dy = state.sum_dy;
      bool only_moved = moved == state;
      return !only_moved || now - at_ >= position_interval;
    }

    void sent(const luzcursor::state_t &state, std::chrono::steady_clock::time_point now) {
      last_ = state;
      at_ = now;
    }

    const std::optional<luzcursor::state_t> &last() const {
      return last_;
    }

  private:
    std::optional<luzcursor::state_t> last_;
    std::chrono::steady_clock::time_point at_ {};
  };

  /// POST /actions/cursor's {"draw": "client"|"host"}: true for client; none without it
  inline std::optional<bool> parse_draw(const std::string &body) {
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("draw") || !json["draw"].is_string()) {
      return std::nullopt;
    }
    auto draw = json["draw"].get<std::string>();
    if (draw == "client") {
      return true;
    }
    if (draw == "host") {
      return false;
    }
    return std::nullopt;
  }

  /// The body has "draw", so it's a draw request (and maybe a bad one), not a pulse
  inline bool is_draw_request(const std::string &body) {
    auto json = nlohmann::json::parse(body, nullptr, false);
    return json.is_object() && json.contains("draw");
  }

  /// The answer to a draw request: who draws the stream's pointer now
  inline std::string draw_json(bool client) {
    return nlohmann::json {{"draw", client ? "client" : "host"}}.dump();
  }
}  // namespace fork_tweaks::cursor_shape_policy
