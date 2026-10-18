/**
 * @file src/input_validation.h
 * @brief Size checks for input packets from clients, before any field is read.
 *
 * Ported from LizardByte/Sunshine (src/input.cpp, validate_input_packet; GHSA-26q2-58j6-qmvv and
 * GHSA-6w33-pjh7-p77c). Header-only, without Boost, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

// lib includes
#include <moonlight-common-c/src/Input.h>

// local includes
#include "fork/luztouchpad.h"  // [fork] trackpad passthrough frames

namespace input {
  namespace validation_detail {
    inline std::uint32_t from_big_endian(std::uint32_t value) {
      return std::endian::native == std::endian::big ? value : std::byteswap(value);
    }

    inline std::uint32_t from_little_endian(std::uint32_t value) {
      return std::endian::native == std::endian::little ? value : std::byteswap(value);
    }

    /**
     * @brief Whether a fixed-size packet declares, and has, exactly its structure's size.
     * @tparam Packet Protocol packet structure.
     * @param packet Raw packet bytes.
     * @param declared_size Size declared in the header, after the size field.
     */
    template<typename Packet>
    bool fixed_size_ok(std::span<const std::uint8_t> packet, std::uint32_t declared_size) {
      constexpr auto expected_size = static_cast<std::uint32_t>(sizeof(Packet) - sizeof(std::uint32_t));
      return declared_size == expected_size && packet.size() >= sizeof(Packet);
    }
  }  // namespace validation_detail

  /**
   * @brief Validates an input packet before any typed access or batching.
   * @param packet Raw packet bytes, as decrypted from the control stream.
   * @param parsed_header Receives a copy of the header when the packet has one; may be null.
   * @return True when the packet holds its declared size and every field its type reads.
   */
  inline bool validate_input_packet(std::span<const std::uint8_t> packet, NV_INPUT_HEADER *parsed_header = nullptr) {
    using namespace validation_detail;

    if (packet.size() < sizeof(NV_INPUT_HEADER)) {
      return false;
    }

    NV_INPUT_HEADER header {};
    std::memcpy(&header, packet.data(), sizeof(header));
    if (parsed_header != nullptr) {
      *parsed_header = header;
    }

    const auto declared_size = from_big_endian(header.size);
    if (declared_size < sizeof(header.magic) || declared_size > packet.size() - sizeof(header.size)) {
      return false;
    }

    switch (from_little_endian(header.magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return fixed_size_ok<NV_REL_MOUSE_MOVE_PACKET>(packet, declared_size);
      case MOUSE_MOVE_ABS_MAGIC:
        return fixed_size_ok<NV_ABS_MOUSE_MOVE_PACKET>(packet, declared_size);
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        return fixed_size_ok<NV_MOUSE_BUTTON_PACKET>(packet, declared_size);
      case SCROLL_MAGIC_GEN5:
        return fixed_size_ok<NV_SCROLL_PACKET>(packet, declared_size);
      case SS_HSCROLL_MAGIC:
        return fixed_size_ok<SS_HSCROLL_PACKET>(packet, declared_size);
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        return fixed_size_ok<NV_KEYBOARD_PACKET>(packet, declared_size);
      case UTF8_TEXT_EVENT_MAGIC:
        // The text's length comes from the declared size, which is checked against the packet above
        return declared_size - sizeof(header.magic) <= UTF8_TEXT_EVENT_MAX_COUNT;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return fixed_size_ok<NV_MULTI_CONTROLLER_PACKET>(packet, declared_size);
      case SS_TOUCH_MAGIC:
        return fixed_size_ok<SS_TOUCH_PACKET>(packet, declared_size);
      case SS_PEN_MAGIC:
        return fixed_size_ok<SS_PEN_PACKET>(packet, declared_size);
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        return fixed_size_ok<SS_CONTROLLER_ARRIVAL_PACKET>(packet, declared_size);
      case SS_CONTROLLER_TOUCH_MAGIC:
        return fixed_size_ok<SS_CONTROLLER_TOUCH_PACKET>(packet, declared_size);
      case SS_CONTROLLER_MOTION_MAGIC:
        return fixed_size_ok<SS_CONTROLLER_MOTION_PACKET>(packet, declared_size);
      case SS_CONTROLLER_BATTERY_MAGIC:
        return fixed_size_ok<SS_CONTROLLER_BATTERY_PACKET>(packet, declared_size);
      case luztouchpad::k_magic:
        // [fork] LuzLunar's trackpad passthrough frame (src/fork/luztouchpad.h)
        return declared_size == luztouchpad::k_declared_size && packet.size() >= luztouchpad::k_packet_size;
      default:
        // Unknown types are dropped by the dispatcher without reading past the header
        return true;
    }
  }
}  // namespace input
