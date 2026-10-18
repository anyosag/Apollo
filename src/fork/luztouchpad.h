/**
 * @file luztouchpad.h
 * @brief Trackpad passthrough frames, sent by LuzLunar to this Apollo build.
 *
 * Shared, byte for byte, by anyosag/Apollo (src/fork/luztouchpad.h) and anyosag/LuzLunar
 * (app/fork/luztouchpad.h). Keep both copies identical; tests/fork/luztouchpad_test.cpp checks the
 * same behavior in both repos' CI. Header-only C++17 with no dependencies.
 *
 * LuzLunar sends the fingers on the Mac trackpad as frames, and Apollo feeds them to Windows'
 * synthetic Precision Touchpad, so Windows makes its own pointer motion, taps, scrolling and
 * gestures. A frame is an input packet in the encrypted control stream, like Moonlight's touch
 * events, and is only sent to a host that advertises k_host_feature_flag.
 *
 * Packet, 50 bytes. The size is big-endian, as in every Moonlight input packet; the rest is
 * little-endian:
 *   u32 size (the bytes after it: 46), u32 magic (0x55004C01), u8 version (1),
 *   u8 contact count (0-5), u8 buttons (bit 0: the pad is pressed down),
 *   u8 flags (bit 0: cancel every contact), u16 pad width and u16 pad height in 1/100 mm,
 *   u32 the client's clock in ms (it wraps),
 *   5 contacts of u8 slot (0-4), u8 phase, u16 x and u16 y (0-65535 from the pad's top-left).
 *   Contacts past the count are zero.
 *
 * Every frame lists every finger still on the pad. A finger that lifts is listed once more with
 * phase_up; a slot missing from a frame has lifted too, so a lost frame can't leave one stuck.
 * While fingers are down the client repeats the current frame every k_keepalive_ms.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace luztouchpad {

  constexpr std::uint32_t k_host_feature_flag = 0x40000000;  ///< In the host's x-ss-general.featureFlags
  constexpr std::uint32_t k_magic = 0x55004C01;
  constexpr std::uint8_t k_version = 1;
  constexpr std::size_t k_max_contacts = 5;  ///< Windows' synthetic touchpad takes at most 5
  constexpr std::size_t k_header_size = 20;
  constexpr std::size_t k_contact_size = 6;
  constexpr std::size_t k_packet_size = k_header_size + k_max_contacts * k_contact_size;
  constexpr std::uint32_t k_declared_size = k_packet_size - 4;  ///< The size field counts the bytes after it
  constexpr std::uint8_t k_channel = 0x05;  ///< moonlight-common-c's CTRL_CHANNEL_TOUCH
  constexpr std::uint16_t k_min_pad_size = 1000;  ///< 10 mm, in 1/100 mm
  constexpr std::uint16_t k_max_pad_size = 40000;  ///< 400 mm
  constexpr std::uint32_t k_keepalive_ms = 100;

  static_assert(k_packet_size == 50, "The packet is 50 bytes");

  enum phase : std::uint8_t {
    phase_down = 1,  ///< The finger touched the pad in this frame
    phase_move = 2,  ///< Still touching
    phase_up = 3,  ///< Lifted in this frame
    phase_cancel = 4,  ///< Taken away without a lift, like a finger macOS now treats as resting
  };

  enum button : std::uint8_t {
    button_pressed = 1,  ///< The pad is clicked down
  };

  enum flag : std::uint8_t {
    flag_cancel_all = 1,  ///< Cancel every contact and release the button (the stream lost focus or ended)
  };

  struct contact {
    std::uint8_t slot = 0;
    std::uint8_t phase = phase_move;
    std::uint16_t x = 0;  ///< 0 at the pad's left edge, 65535 at its right
    std::uint16_t y = 0;  ///< 0 at the pad's top edge, 65535 at its bottom
  };

  struct frame {
    std::uint8_t count = 0;
    std::uint8_t buttons = 0;
    std::uint8_t flags = 0;
    std::uint16_t width = 0;  ///< In 1/100 mm, which is Windows' himetric unit
    std::uint16_t height = 0;
    std::uint32_t time_ms = 0;  ///< The client's monotonic clock; only differences matter
    std::array<contact, k_max_contacts> contacts {};
  };

  using packet = std::array<std::uint8_t, k_packet_size>;

  namespace detail {
    inline void put16(std::uint8_t *p, std::uint16_t v) {
      p[0] = (std::uint8_t) (v & 0xFF);
      p[1] = (std::uint8_t) (v >> 8);
    }

    inline void put32(std::uint8_t *p, std::uint32_t v) {
      for (int i = 0; i < 4; i++) {
        p[i] = (std::uint8_t) (v >> (8 * i));
      }
    }

    inline std::uint16_t get16(const std::uint8_t *p) {
      return (std::uint16_t) (p[0] | p[1] << 8);
    }

    inline std::uint32_t get32(const std::uint8_t *p) {
      return (std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16 | (std::uint32_t) p[3] << 24;
    }
  }  // namespace detail

  /** The packet for a frame, ready to send. Contacts past the count are written as zero. */
  inline packet encode(const frame &f) {
    packet p {};
    // The size is big-endian
    p[0] = (std::uint8_t) (k_declared_size >> 24);
    p[1] = (std::uint8_t) (k_declared_size >> 16);
    p[2] = (std::uint8_t) (k_declared_size >> 8);
    p[3] = (std::uint8_t) k_declared_size;
    detail::put32(&p[4], k_magic);
    p[8] = k_version;
    std::uint8_t count = f.count > k_max_contacts ? (std::uint8_t) k_max_contacts : f.count;
    p[9] = count;
    p[10] = f.buttons;
    p[11] = f.flags;
    detail::put16(&p[12], f.width);
    detail::put16(&p[14], f.height);
    detail::put32(&p[16], f.time_ms);
    for (std::size_t i = 0; i < count; i++) {
      std::uint8_t *c = &p[k_header_size + i * k_contact_size];
      c[0] = f.contacts[i].slot;
      c[1] = f.contacts[i].phase;
      detail::put16(&c[2], f.contacts[i].x);
      detail::put16(&c[4], f.contacts[i].y);
    }
    return p;
  }

  /**
   * Reads a packet, starting at its size field. Returns false for anything malformed: a wrong
   * size, magic or version, more than 5 contacts, a slot past 4 or used twice, an unknown phase,
   * or a pad smaller than 10 mm or larger than 400 mm.
   */
  inline bool decode(const std::uint8_t *data, std::size_t size, frame &out) {
    if (data == nullptr || size < k_packet_size) {
      return false;
    }
    std::uint32_t declared = (std::uint32_t) data[0] << 24 | (std::uint32_t) data[1] << 16 | (std::uint32_t) data[2] << 8 | data[3];
    if (declared != k_declared_size || detail::get32(&data[4]) != k_magic || data[8] != k_version) {
      return false;
    }

    frame f;
    f.count = data[9];
    f.buttons = data[10];
    f.flags = data[11];
    f.width = detail::get16(&data[12]);
    f.height = detail::get16(&data[14]);
    f.time_ms = detail::get32(&data[16]);
    if (f.count > k_max_contacts) {
      return false;
    }
    if (f.width < k_min_pad_size || f.width > k_max_pad_size || f.height < k_min_pad_size || f.height > k_max_pad_size) {
      return false;
    }

    unsigned seen = 0;
    for (std::size_t i = 0; i < f.count; i++) {
      const std::uint8_t *c = &data[k_header_size + i * k_contact_size];
      contact &k = f.contacts[i];
      k.slot = c[0];
      k.phase = c[1];
      k.x = detail::get16(&c[2]);
      k.y = detail::get16(&c[4]);
      if (k.slot >= k_max_contacts || (seen & (1u << k.slot)) != 0) {
        return false;
      }
      if (k.phase < phase_down || k.phase > phase_cancel) {
        return false;
      }
      seen |= 1u << k.slot;
    }

    out = f;
    return true;
  }

  /**
   * Whether a frame must arrive. Only frames where every finger just moves, with the button and
   * flags unchanged, may be lost, since the next frame carries the same fingers again.
   */
  inline bool is_reliable(const frame &f, std::uint8_t previous_buttons) {
    if (f.count == 0 || f.flags != 0 || f.buttons != previous_buttons) {
      return true;
    }
    for (std::size_t i = 0; i < f.count && i < k_max_contacts; i++) {
      if (f.contacts[i].phase != phase_move) {
        return true;
      }
    }
    return false;
  }

}  // namespace luztouchpad
