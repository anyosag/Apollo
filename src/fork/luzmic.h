/**
 * @file luzmic.h
 * @brief The Mac's microphone on the PC: what anyosag/LuzLunar sends anyosag/Apollo, and back.
 *
 * Shared, byte for byte, by anyosag/Apollo (src/fork/luzmic.h) and anyosag/LuzLunar
 * (app/fork/luzmic.h). Keep both copies identical; tests/fork/luzmic_test.cpp checks the same
 * behavior in both repos' CI. Header-only C++17 with no dependencies.
 *
 * A client that lends the PC its microphone launches with luzMic=1, on a host whose /serverinfo
 * says <LuzMic> k_serverinfo_version or more and whose x-ss-general.featureFlags has
 * k_host_feature_flag. The host plays what it hears into the Steam Streaming Microphone's speakers,
 * so PC apps record it from "Microphone (Steam Streaming Microphone)", Windows' default recording
 * device while the stream runs.
 *
 * STATE (k_type_state), host → client: a control message, encrypted and reliable on ENet channel
 * k_channel, each time it changes. k_state_size bytes:
 *   u8 version (1), u8 flags (state_flag), u16 reserved (0).
 * The client records only while flag_recording says an app on the PC records, so the Mac's
 * microphone is live (and its indicator on) no longer than the PC uses it.
 *
 * FRAME (k_magic), client → host: an input packet (moonlight-common-c's LiSendForkInputPacket(),
 * LuzLunar's patch 0003), unreliable on k_channel, every k_frame_ms while the client records. The
 * 8-byte input header (u32 big-endian: the size of what follows it; u32 little-endian: k_magic),
 * then, little-endian:
 *   u8 version (1), u8 flags (frame_flag), u16 sequence (one more each frame, wrapping),
 *   u32 time: when the frame's first sample was recorded, in milliseconds on the client's clock
 *   (wrapping),
 * then one Opus packet of 1 to k_max_opus bytes: k_frame_samples mono samples at 48 kHz.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace luzmic {

  constexpr std::uint32_t k_host_feature_flag = 0x08000000;  ///< In the host's x-ss-general.featureFlags
  constexpr int k_serverinfo_version = 1;  ///< /serverinfo's <LuzMic>
  constexpr std::uint32_t k_magic = 0x55004C02;  ///< A FRAME's input packet
  constexpr std::uint16_t k_type_state = 0x4C03;  ///< In the fork's host → client range, 0x4C00 to 0x4CFF
  constexpr std::uint8_t k_channel = 0x06;  ///< Its own ENet channel, both ways
  constexpr std::uint8_t k_version = 1;
  constexpr int k_sample_rate = 48000;
  constexpr int k_frame_ms = 20;
  constexpr int k_frame_samples = k_sample_rate / 1000 * k_frame_ms;  ///< 960
  constexpr std::size_t k_input_header_size = 8;
  constexpr std::size_t k_frame_header_size = 8;
  constexpr std::size_t k_max_packet = 128;  ///< The largest input packet LiSendForkInputPacket() takes
  constexpr std::size_t k_max_opus = k_max_packet - k_input_header_size - k_frame_header_size;  ///< 112 bytes: 44.8 kb/s
  constexpr std::size_t k_state_size = 4;

  enum state_flag : std::uint8_t {
    flag_recording = 1,  ///< An app on the PC records from the Steam microphone: the client sends FRAMEs
    flag_ready = 2,  ///< The host found the Steam microphone and plays into it
  };

  enum frame_flag : std::uint8_t {
    flag_start = 1,  ///< The first frame since the client started recording: the host's timeline starts over
  };

  struct state_t {
    std::uint8_t flags = 0;

    bool recording() const {
      return (flags & flag_recording) != 0;
    }

    bool ready() const {
      return (flags & flag_ready) != 0;
    }

    bool operator==(const state_t &other) const {
      return flags == other.flags;
    }

    bool operator!=(const state_t &other) const {
      return !(*this == other);
    }
  };

  /// A FRAME; opus points into the packet it was decoded from
  struct frame_t {
    std::uint8_t flags = 0;
    std::uint16_t sequence = 0;
    std::uint32_t time_ms = 0;
    const std::uint8_t *opus = nullptr;
    std::size_t opus_size = 0;

    bool start() const {
      return (flags & flag_start) != 0;
    }
  };

  namespace detail {
    inline void put16(std::uint8_t *p, std::uint16_t v) {
      p[0] = static_cast<std::uint8_t>(v);
      p[1] = static_cast<std::uint8_t>(v >> 8);
    }

    inline void put32(std::uint8_t *p, std::uint32_t v) {
      for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
      }
    }

    inline std::uint16_t get16(const std::uint8_t *p) {
      return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }

    inline std::uint32_t get32(const std::uint8_t *p) {
      std::uint32_t v = 0;
      for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
      }
      return v;
    }

    inline std::uint32_t get32_be(const std::uint8_t *p) {
      return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }
  }  // namespace detail

  /// A STATE's payload (what follows the control message's type)
  inline std::string encode_state(const state_t &state) {
    std::string payload(k_state_size, '\0');
    payload[0] = static_cast<char>(k_version);
    payload[1] = static_cast<char>(state.flags);
    return payload;
  }

  /// False for a payload too short or of another version
  inline bool decode_state(const std::uint8_t *payload, std::size_t size, state_t &state) {
    if (payload == nullptr || size < k_state_size || payload[0] != k_version) {
      return false;
    }
    state.flags = payload[1];
    return true;
  }

  /// The whole input packet for LiSendForkInputPacket(); empty when the Opus packet is empty or too big
  inline std::vector<std::uint8_t> encode_frame(const frame_t &frame) {
    if (frame.opus == nullptr || frame.opus_size == 0 || frame.opus_size > k_max_opus) {
      return {};
    }
    const std::size_t size = k_input_header_size + k_frame_header_size + frame.opus_size;
    std::vector<std::uint8_t> packet(size);
    const auto declared = static_cast<std::uint32_t>(size - 4);
    packet[0] = static_cast<std::uint8_t>(declared >> 24);
    packet[1] = static_cast<std::uint8_t>(declared >> 16);
    packet[2] = static_cast<std::uint8_t>(declared >> 8);
    packet[3] = static_cast<std::uint8_t>(declared);
    detail::put32(&packet[4], k_magic);
    packet[8] = k_version;
    packet[9] = frame.flags;
    detail::put16(&packet[10], frame.sequence);
    detail::put32(&packet[12], frame.time_ms);
    for (std::size_t i = 0; i < frame.opus_size; ++i) {
      packet[16 + i] = frame.opus[i];
    }
    return packet;
  }

  /// Whether an input packet is a FRAME (its magic), before decode_frame() checks the rest
  inline bool is_frame(const std::uint8_t *packet, std::size_t size) {
    return packet != nullptr && size >= k_input_header_size && detail::get32(&packet[4]) == k_magic;
  }

  /// A whole input packet, its header included. False unless it's a FRAME of this version whose
  /// declared size is what's there, with an Opus packet of 1 to k_max_opus bytes
  inline bool decode_frame(const std::uint8_t *packet, std::size_t size, frame_t &frame) {
    if (!is_frame(packet, size) || size < k_input_header_size + k_frame_header_size + 1 ||
        size > k_input_header_size + k_frame_header_size + k_max_opus || detail::get32_be(packet) != size - 4 ||
        packet[8] != k_version) {
      return false;
    }
    frame.flags = packet[9];
    frame.sequence = detail::get16(&packet[10]);
    frame.time_ms = detail::get32(&packet[12]);
    frame.opus = packet + k_input_header_size + k_frame_header_size;
    frame.opus_size = size - k_input_header_size - k_frame_header_size;
    return true;
  }

}  // namespace luzmic
