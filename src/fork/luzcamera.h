/**
 * @file luzcamera.h
 * @brief The Mac's camera on the PC: what LuzLunar and this Apollo build send each other.
 *
 * Shared, byte for byte, by anyosag/Apollo (src/fork/luzcamera.h) and anyosag/LuzLunar
 * (app/fork/luzcamera.h). Keep both copies identical; tests/fork/luzcamera_test.cpp checks the
 * same behavior in both repos' CI. Header-only C++17 with no dependencies.
 *
 * Apollo lists a "Mac Camera" on the PC. While an app there has it open, LuzLunar captures the
 * Mac's camera at the size the app chose, encodes it as H.264 and sends it to Apollo, which decodes
 * it into the camera.
 *
 * The connection: during a stream, LuzLunar opens a TLS connection to the host's HTTPS port with
 * its paired client certificate, like any other request, and sends
 *   GET /actions/camera HTTP/1.1, with "Upgrade: luzcamera/1" and "Connection: Upgrade"
 * A host that advertises k_host_feature_flag (and <LuzCamera>1 in serverinfo) answers
 * "101 Switching Protocols" when the client has the Camera permission and its stream runs, or
 * another status with a one-word reason as the body, and closes. The client sends nothing more
 * until the 101. After it, both sides send messages: an 8-byte header and a payload. Every number
 * is little-endian.
 *   u8 type, u8 flags, u16 reserved (0), u32 payload size
 *
 * Host to client:
 *   START (0x01): u16 width, u16 height, u16 fps, u16 reserved, u32 kbps. An app on the PC opened
 *     the camera at this size: the client turns its camera on and sends VIDEO at about kbps. Sent
 *     again while running, with another size or bitrate, the client switches.
 *   STOP (0x02): no payload. The last app closed the camera a few seconds ago; the camera goes off.
 *   KEYFRAME (0x03): no payload. The host's decoder needs a keyframe: the next VIDEO is one.
 *   BITRATE (0x04): u32 kbps, the most the client sends from now on.
 * Client to host:
 *   HELLO (0x81): u16 version (k_version), u16 reserved, then the client's name in UTF-8 (at most
 *     k_max_name bytes). The client's first message.
 *   VIDEO (0x82): flags bit 0 (k_flag_keyframe): a keyframe. u64 capture time in microseconds on
 *     the client's clock, u32 frame number, u16 width, u16 height, then one H.264 access unit in
 *     Annex B (start codes), with its SPS and PPS before each keyframe.
 *   STATUS (0x83): u8 state (status_e), then a UTF-8 detail (at most k_max_detail bytes).
 * Both ways:
 *   PING (0x90): u64 the sender's clock in milliseconds. Each side sends one every
 *     k_ping_interval_ms; a side that has heard nothing for k_silence_timeout_ms closes.
 *
 * A message of an unknown type is skipped, so either side can add some; a payload above its limit
 * (k_max_video_payload for VIDEO, k_max_control_payload for the others) ends the connection.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace luzcamera {
  constexpr const char *k_path = "/actions/camera";
  constexpr const char *k_upgrade = "luzcamera/1";
  constexpr std::uint16_t k_version = 1;
  constexpr std::uint32_t k_host_feature_flag = 0x04000000;  ///< In the host's x-ss-general.featureFlags
  constexpr const char *k_launch_arg = "luzCamera";  ///< luzCamera=1 in /launch and /resume: this stream brings its camera

  constexpr std::size_t k_header_size = 8;
  constexpr std::uint32_t k_max_control_payload = 256;
  constexpr std::uint32_t k_max_video_payload = 2u << 20;  ///< 2 MiB, far above a 1080p keyframe at 5 Mb/s
  constexpr std::size_t k_max_name = 64;
  constexpr std::size_t k_max_detail = 200;
  constexpr int k_ping_interval_ms = 1000;
  constexpr int k_silence_timeout_ms = 5000;

  enum class type_e : std::uint8_t {
    start = 0x01,
    stop = 0x02,
    keyframe = 0x03,
    bitrate = 0x04,
    hello = 0x81,
    video = 0x82,
    status = 0x83,
    ping = 0x90,
  };

  constexpr std::uint8_t k_flag_keyframe = 0x01;

  /// What STATUS says of the client's camera
  enum class status_e : std::uint8_t {
    live = 0,  ///< capturing and sending VIDEO
    stopped = 1,  ///< off, as STOP asked
    denied = 2,  ///< macOS doesn't let the client use the camera (Privacy & Security › Camera)
    unavailable = 3,  ///< no camera, or it couldn't start; the detail says why
    off = 4,  ///< the user turned the camera off in the client's settings; the client closes next
  };

  struct header_t {
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t size = 0;
  };

  struct start_t {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t fps = 0;
    std::uint32_t kbps = 0;
  };

  struct hello_t {
    std::uint16_t version = 0;
    std::string name;
  };

  struct video_t {
    bool keyframe = false;
    std::uint64_t time_us = 0;
    std::uint32_t number = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    const std::uint8_t *data = nullptr;  ///< the access unit, inside the message it was read from
    std::size_t size = 0;
  };

  struct status_t {
    status_e state = status_e::stopped;
    std::string detail;
  };

  constexpr std::size_t k_start_size = 12;
  constexpr std::size_t k_video_prefix = 16;  ///< VIDEO's fields before the access unit

  namespace detail {
    inline void put16(std::uint8_t *p, std::uint16_t v) {
      p[0] = (std::uint8_t) v;
      p[1] = (std::uint8_t) (v >> 8);
    }

    inline void put32(std::uint8_t *p, std::uint32_t v) {
      for (int i = 0; i < 4; i++) {
        p[i] = (std::uint8_t) (v >> (8 * i));
      }
    }

    inline void put64(std::uint8_t *p, std::uint64_t v) {
      for (int i = 0; i < 8; i++) {
        p[i] = (std::uint8_t) (v >> (8 * i));
      }
    }

    inline std::uint16_t get16(const std::uint8_t *p) {
      return (std::uint16_t) (p[0] | (p[1] << 8));
    }

    inline std::uint32_t get32(const std::uint8_t *p) {
      std::uint32_t v = 0;
      for (int i = 3; i >= 0; i--) {
        v = (v << 8) | p[i];
      }
      return v;
    }

    inline std::uint64_t get64(const std::uint8_t *p) {
      std::uint64_t v = 0;
      for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
      }
      return v;
    }

    /// Text cut to at most max bytes, without splitting a UTF-8 character
    inline std::string cut_utf8(const std::string &text, std::size_t max) {
      if (text.size() <= max) {
        return text;
      }
      std::size_t end = max;
      while (end > 0 && (((unsigned char) text[end]) & 0xC0) == 0x80) {
        end--;
      }
      return text.substr(0, end);
    }
  }  // namespace detail

  /// The largest payload a message of this type may have
  inline std::uint32_t max_payload(std::uint8_t type) {
    return type == (std::uint8_t) type_e::video ? k_max_video_payload : k_max_control_payload;
  }

  /// Appends one message to out
  inline void append(std::vector<std::uint8_t> &out, type_e type, std::uint8_t flags, const std::uint8_t *payload, std::size_t size) {
    std::size_t at = out.size();
    out.resize(at + k_header_size + size);
    std::uint8_t *p = out.data() + at;
    p[0] = (std::uint8_t) type;
    p[1] = flags;
    p[2] = 0;
    p[3] = 0;
    detail::put32(p + 4, (std::uint32_t) size);
    if (size > 0) {
      std::memcpy(p + k_header_size, payload, size);
    }
  }

  inline std::vector<std::uint8_t> make_start(const start_t &start) {
    std::uint8_t payload[k_start_size] = {};
    detail::put16(payload, start.width);
    detail::put16(payload + 2, start.height);
    detail::put16(payload + 4, start.fps);
    detail::put32(payload + 8, start.kbps);
    std::vector<std::uint8_t> out;
    append(out, type_e::start, 0, payload, sizeof(payload));
    return out;
  }

  inline std::vector<std::uint8_t> make_empty(type_e type) {
    std::vector<std::uint8_t> out;
    append(out, type, 0, nullptr, 0);
    return out;
  }

  inline std::vector<std::uint8_t> make_bitrate(std::uint32_t kbps) {
    std::uint8_t payload[4];
    detail::put32(payload, kbps);
    std::vector<std::uint8_t> out;
    append(out, type_e::bitrate, 0, payload, sizeof(payload));
    return out;
  }

  inline std::vector<std::uint8_t> make_hello(const std::string &name) {
    std::string cut = detail::cut_utf8(name, k_max_name);
    std::vector<std::uint8_t> payload(4 + cut.size());
    detail::put16(payload.data(), k_version);
    detail::put16(payload.data() + 2, 0);
    std::memcpy(payload.data() + 4, cut.data(), cut.size());
    std::vector<std::uint8_t> out;
    append(out, type_e::hello, 0, payload.data(), payload.size());
    return out;
  }

  inline std::vector<std::uint8_t> make_status(status_e state, const std::string &text) {
    std::string cut = detail::cut_utf8(text, k_max_detail);
    std::vector<std::uint8_t> payload(1 + cut.size());
    payload[0] = (std::uint8_t) state;
    std::memcpy(payload.data() + 1, cut.data(), cut.size());
    std::vector<std::uint8_t> out;
    append(out, type_e::status, 0, payload.data(), payload.size());
    return out;
  }

  inline std::vector<std::uint8_t> make_ping(std::uint64_t now_ms) {
    std::uint8_t payload[8];
    detail::put64(payload, now_ms);
    std::vector<std::uint8_t> out;
    append(out, type_e::ping, 0, payload, sizeof(payload));
    return out;
  }

  /// A VIDEO message with the access unit au (Annex B); empty if it's too large to send
  inline std::vector<std::uint8_t> make_video(const video_t &video, const std::uint8_t *au, std::size_t size) {
    std::vector<std::uint8_t> out;
    if (k_video_prefix + size > k_max_video_payload) {
      return out;
    }
    out.resize(k_header_size + k_video_prefix + size);
    std::uint8_t *p = out.data();
    p[0] = (std::uint8_t) type_e::video;
    p[1] = video.keyframe ? k_flag_keyframe : 0;
    p[2] = 0;
    p[3] = 0;
    detail::put32(p + 4, (std::uint32_t) (k_video_prefix + size));
    p += k_header_size;
    detail::put64(p, video.time_us);
    detail::put32(p + 8, video.number);
    detail::put16(p + 12, video.width);
    detail::put16(p + 14, video.height);
    if (size > 0) {
      std::memcpy(p + k_video_prefix, au, size);
    }
    return out;
  }

  /// One message as read: its header and payload
  struct message_t {
    header_t header;
    const std::uint8_t *payload = nullptr;  ///< valid until the next feed() or next()
    std::size_t size = 0;

    type_e type() const {
      return (type_e) header.type;
    }
  };

  inline bool parse_start(const message_t &m, start_t &out) {
    if (m.size < k_start_size) {
      return false;
    }
    out.width = detail::get16(m.payload);
    out.height = detail::get16(m.payload + 2);
    out.fps = detail::get16(m.payload + 4);
    out.kbps = detail::get32(m.payload + 8);
    return out.width > 0 && out.height > 0 && out.fps > 0;
  }

  inline bool parse_bitrate(const message_t &m, std::uint32_t &kbps) {
    if (m.size < 4) {
      return false;
    }
    kbps = detail::get32(m.payload);
    return kbps > 0;
  }

  inline bool parse_hello(const message_t &m, hello_t &out) {
    if (m.size < 4) {
      return false;
    }
    out.version = detail::get16(m.payload);
    std::string name((const char *) m.payload + 4, m.size - 4);
    out.name = detail::cut_utf8(name, k_max_name);
    return true;
  }

  inline bool parse_status(const message_t &m, status_t &out) {
    if (m.size < 1) {
      return false;
    }
    out.state = (status_e) m.payload[0];
    std::string text((const char *) m.payload + 1, m.size - 1);
    out.detail = detail::cut_utf8(text, k_max_detail);
    return true;
  }

  inline bool parse_video(const message_t &m, video_t &out) {
    if (m.size < k_video_prefix) {
      return false;
    }
    out.keyframe = (m.header.flags & k_flag_keyframe) != 0;
    out.time_us = detail::get64(m.payload);
    out.number = detail::get32(m.payload + 8);
    out.width = detail::get16(m.payload + 12);
    out.height = detail::get16(m.payload + 14);
    out.data = m.payload + k_video_prefix;
    out.size = m.size - k_video_prefix;
    return out.width > 0 && out.height > 0 && out.size > 0;
  }

  /**
   * @brief Takes the bytes of the connection as they arrive and gives back whole messages.
   * A payload above its type's limit is an error, after which the connection should close.
   */
  class reader_t {
  public:
    enum class result_e {
      need_more,  ///< no whole message yet
      message,  ///< one is in the message_t
      error,  ///< too large: the connection is broken
    };

    void feed(const std::uint8_t *data, std::size_t size) {
      // What was read already goes, before the buffer grows
      if (m_At > 0 && (m_At >= m_Buffer.size() / 2 || m_At == m_Buffer.size())) {
        m_Buffer.erase(m_Buffer.begin(), m_Buffer.begin() + (std::ptrdiff_t) m_At);
        m_At = 0;
      }
      m_Buffer.insert(m_Buffer.end(), data, data + size);
    }

    result_e next(message_t &out) {
      if (m_Failed) {
        return result_e::error;
      }
      std::size_t available = m_Buffer.size() - m_At;
      if (available < k_header_size) {
        return result_e::need_more;
      }
      const std::uint8_t *p = m_Buffer.data() + m_At;
      header_t header;
      header.type = p[0];
      header.flags = p[1];
      header.size = detail::get32(p + 4);
      if (header.size > max_payload(header.type)) {
        m_Failed = true;
        return result_e::error;
      }
      if (available < k_header_size + header.size) {
        return result_e::need_more;
      }
      out.header = header;
      out.payload = p + k_header_size;
      out.size = header.size;
      m_At += k_header_size + header.size;
      return result_e::message;
    }

    /// Bytes held that aren't whole messages yet
    std::size_t pending() const {
      return m_Buffer.size() - m_At;
    }

  private:
    std::vector<std::uint8_t> m_Buffer;
    std::size_t m_At = 0;
    bool m_Failed = false;
  };

  /**
   * @brief Calls f(nal_type, nal, size) for each NAL unit of an Annex B access unit (3- or 4-byte
   * start codes), the start code left out. Returns how many there were.
   */
  template<typename F>
  std::size_t for_each_nal(const std::uint8_t *data, std::size_t size, F &&f) {
    std::size_t count = 0;
    std::size_t i = 0;
    // The first start code
    auto find_start = [&](std::size_t from, std::size_t &code) -> std::size_t {
      for (std::size_t j = from; j + 3 <= size; j++) {
        if (data[j] == 0 && data[j + 1] == 0) {
          if (data[j + 2] == 1) {
            code = 3;
            return j;
          }
          if (j + 4 <= size && data[j + 2] == 0 && data[j + 3] == 1) {
            code = 4;
            return j;
          }
        }
      }
      code = 0;
      return size;
    };
    std::size_t code = 0;
    i = find_start(0, code);
    while (i < size) {
      std::size_t begin = i + code;
      std::size_t next_code = 0;
      std::size_t next = find_start(begin, next_code);
      std::size_t end = next;
      // Trailing zero bytes belong to the next start code, not this NAL
      while (end > begin && data[end - 1] == 0 && next < size) {
        end--;
      }
      if (end > begin) {
        f((int) (data[begin] & 0x1F), data + begin, end - begin);
        count++;
      }
      i = next;
      code = next_code;
    }
    return count;
  }

  /// Whether an Annex B access unit holds an IDR slice (NAL type 5), with SPS (7) and PPS (8) before it
  inline bool is_decodable_keyframe(const std::uint8_t *data, std::size_t size) {
    bool sps = false;
    bool pps = false;
    bool idr = false;
    for_each_nal(data, size, [&](int type, const std::uint8_t *, std::size_t) {
      if (type == 7) {
        sps = true;
      } else if (type == 8) {
        pps = true;
      } else if (type == 5) {
        idr = idr || (sps && pps);
      }
    });
    return idr;
  }

  /// The bitrate a size gets unless the host says otherwise: about 2.5 Mb/s at 720p, 5 Mb/s at 1080p
  inline std::uint32_t default_kbps(std::uint32_t width, std::uint32_t height) {
    std::uint64_t pixels = (std::uint64_t) width * height;
    if (pixels >= 1920ull * 1080) {
      return 5000;
    }
    if (pixels >= 1280ull * 720) {
      return 2500;
    }
    return 1500;
  }
}  // namespace luzcamera
