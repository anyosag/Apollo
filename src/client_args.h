/**
 * @file src/client_args.h
 * @brief Checks for values clients send in /launch, /resume and /pair.
 *
 * Header-only, without Boost, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace nvhttp::client_args {
  /// Hex digits in a launch key: AES-128, 16 bytes
  constexpr std::size_t rikey_hex_length = 32;

  /// The longest surroundParams a client sends: a few counts and up to 8 channel mapping digits
  constexpr std::size_t max_surround_params_length = 32;

  /// The longest device name kept from a client
  constexpr std::size_t max_device_name_length = 64;

  /**
   * @brief Whether rikey is exactly 16 bytes of hex, the AES-128 key the streams are encrypted with.
   * A shorter key would have AES read past it.
   */
  inline bool valid_rikey(std::string_view rikey) {
    return rikey.size() == rikey_hex_length && std::all_of(rikey.begin(), rikey.end(), [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
  }

  /**
   * @brief Whether surroundParams is only digits, as Moonlight sends it (channel count, streams,
   * coupled streams, then the channel mapping). It's passed on to prep commands' environment.
   */
  inline bool valid_surround_params(std::string_view params) {
    return params.size() <= max_surround_params_length && std::all_of(params.begin(), params.end(), [](char c) {
             return c >= '0' && c <= '9';
           });
  }

  /**
   * @brief A client's device name, safe to show and to pass to commands' environment: control
   * characters (including newlines) and the characters shells and cmd.exe treat specially are
   * replaced with spaces, ( and ) become [ and ] as before, and the result is trimmed and capped.
   * UTF-8 letters are kept.
   */
  inline std::string sanitize_device_name(std::string_view name) {
    std::string out;
    out.reserve(std::min(name.size(), max_device_name_length));
    for (char c : name) {
      auto u = static_cast<unsigned char>(c);
      if (c == '(') {
        out.push_back('[');
      } else if (c == ')') {
        out.push_back(']');
      } else if (u < 0x20 || u == 0x7F || std::string_view {"\"%&<>^|`$;!\\"}.find(c) != std::string_view::npos) {
        out.push_back(' ');
      } else {
        out.push_back(c);
      }
    }

    auto begin = out.find_first_not_of(' ');
    if (begin == std::string::npos) {
      return {};
    }
    out = out.substr(begin, out.find_last_not_of(' ') - begin + 1);

    if (out.size() > max_device_name_length) {
      // Don't cut a UTF-8 character in half
      std::size_t end = max_device_name_length;
      while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80) {
        end--;
      }
      out.resize(end);
    }
    return out;
  }
}  // namespace nvhttp::client_args
