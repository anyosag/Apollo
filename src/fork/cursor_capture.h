/**
 * @file src/fork/cursor_capture.h
 * @brief Fork-only: a Desktop Duplication capture's side of LuzLunar's pointer drawn by the Mac
 * (client_cursor.h): the last shape it saw, converted and published once a stream draws the
 * pointer itself. Light, for platform/windows/display.h.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fork_tweaks::client_cursor {
  class capture_shape_t {
  public:
    /// A new shape from GetFramePointerShape(), kept as it came (DXGI_OUTDUPL_POINTER_SHAPE_INFO's fields)
    void seen(int type, const std::uint8_t *data, std::size_t size, int width, int height, int pitch, int hotspot_x, int hotspot_y) {
      data_.assign(data, data + size);
      type_ = type;
      width_ = width;
      height_ = height;
      pitch_ = pitch;
      hotspot_x_ = hotspot_x;
      hotspot_y_ = hotspot_y;
      told_ = false;
    }

    /// While the capture tells its streams (platf::fork_cursor_e past draw): a shape not told yet
    /// is converted and published (client_cursor.cpp)
    void tell();

    /// The shape told last could be converted, so the client can draw it
    bool usable() const {
      return usable_;
    }

  private:
    std::vector<std::uint8_t> data_;
    int type_ = 0;
    int width_ = 0;
    int height_ = 0;
    int pitch_ = 0;
    int hotspot_x_ = 0;
    int hotspot_y_ = 0;
    bool told_ = true;  ///< Nothing to tell before a shape arrives
    bool usable_ = false;
  };
}  // namespace fork_tweaks::client_cursor
