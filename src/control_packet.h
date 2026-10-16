/**
 * @file src/control_packet.h
 * @brief Bounds-checked parsing of control stream messages from clients.
 *
 * Header-only, without Boost or ENet, so tests can build it on its own. parse_control_packet() is
 * ported from LizardByte/Sunshine (GHSA-c428-87f8-rrv5); the encrypted and legacy input wrappers
 * gained the same checks here.
 */
#pragma once

// standard includes
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace stream::control_packet {
  namespace detail {
    template<typename T>
    T read_le(const char *data) {
      T value;
      std::memcpy(&value, data, sizeof(value));
      return std::endian::native == std::endian::little ? value : std::byteswap(value);
    }

    template<typename T>
    T read_be(const char *data) {
      T value;
      std::memcpy(&value, data, sizeof(value));
      return std::endian::native == std::endian::big ? value : std::byteswap(value);
    }
  }  // namespace detail

  /**
   * @brief Splits a control message into its type and payload.
   * @param data The message bytes; may be null when size is 0.
   * @param size The message size.
   * @return The type (in the wire's byte order, as the handlers are keyed) and the payload after it,
   *         or nothing when the message is too short to hold a type.
   */
  inline std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const void *data, std::size_t size) {
    if (data == nullptr || size < sizeof(std::uint16_t)) {
      return std::nullopt;
    }

    std::uint16_t type;
    std::memcpy(&type, data, sizeof(type));
    return std::pair {type, std::string_view {static_cast<const char *>(data) + sizeof(type), size - sizeof(type)}};
  }

  /** An encrypted control message (IDX_ENCRYPTED): the GCM sequence number and the tagged ciphertext. */
  struct encrypted_t {
    std::uint32_t seq;
    std::string_view tagged_cipher;  ///< 16-byte tag, then the encrypted message
  };

  /// The smallest encrypted message: sequence number, tag, and the inner type and length.
  constexpr std::size_t min_encrypted_length = 4 + 16 + 4;

  /**
   * @brief Reads an encrypted control message's header.
   * @param payload What follows the message type: u16 length (LE), u32 sequence number (LE), then
   *        length - 4 bytes of tagged ciphertext.
   * @return The sequence number and ciphertext, or nothing when the declared length is too short or
   *         runs past the end of the payload.
   */
  inline std::optional<encrypted_t> parse_encrypted(std::string_view payload) {
    constexpr std::size_t header_size = sizeof(std::uint16_t) + sizeof(std::uint32_t);
    if (payload.size() < header_size) {
      return std::nullopt;
    }

    auto length = detail::read_le<std::uint16_t>(payload.data());
    auto seq = detail::read_le<std::uint32_t>(payload.data() + sizeof(std::uint16_t));
    if (length < min_encrypted_length) {
      return std::nullopt;
    }

    // length counts the sequence number and the tagged ciphertext
    std::size_t cipher_size = std::size_t(length) - sizeof(std::uint32_t);
    if (cipher_size > payload.size() - header_size) {
      return std::nullopt;
    }
    return encrypted_t {seq, payload.substr(header_size, cipher_size)};
  }

  /**
   * @brief Reads a legacy encrypted input message (IDX_INPUT_DATA).
   * @param payload What follows the message type: i32 ciphertext length (BE), then the ciphertext.
   * @return The ciphertext, or nothing when the declared length is negative or runs past the end of
   *         the payload.
   */
  inline std::optional<std::string_view> parse_legacy_input(std::string_view payload) {
    if (payload.size() < sizeof(std::int32_t)) {
      return std::nullopt;
    }

    auto length = detail::read_be<std::int32_t>(payload.data());
    if (length < 0 || std::size_t(length) > payload.size() - sizeof(std::int32_t)) {
      return std::nullopt;
    }
    return payload.substr(sizeof(std::int32_t), std::size_t(length));
  }
}  // namespace stream::control_packet
