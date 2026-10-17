/**
 * @file src/fork/frame_loss.h
 * @brief What the client reports about the video it lost: its per-frame FEC status messages, the
 * reference frames it asks the encoder to stop using, and its keyframe requests, counted per stream.
 *
 * Moonlight clients send an `SS_FRAME_FEC_STATUS` message (type 0x5502, the same number as the
 * host's "Set RGB LED") for each frame that needed repair from FEC parity packets or couldn't be
 * repaired. Upstream had no handler for it, so the reports only reached the debug log as unknown
 * messages. A stream with no losses sends none.
 *
 * Header-only, without Boost, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace frame_loss {
  /// The client's per-frame FEC status, in moonlight-common-c's `SS_FRAME_FEC_STATUS` (Video.h)
  struct fec_status_t {
    uint32_t frame_index;
    uint16_t highest_received_sequence;
    uint16_t next_contiguous_sequence;
    uint16_t missing_before_highest;
    uint16_t total_data;
    uint16_t total_parity;
    uint16_t received_data;
    uint16_t received_parity;
    uint8_t fec_percentage;
    uint8_t block_index;
    uint8_t block_count;
  };

  /// The message's packed size: one 32-bit field, seven 16-bit ones and three bytes
  constexpr std::size_t fec_status_size = 4 + 7 * 2 + 3;

  /**
   * @brief Reads a FEC status message, whose fields are big-endian.
   * @return Nothing if the payload is too short.
   */
  inline std::optional<fec_status_t> parse_fec_status(std::string_view payload) {
    if (payload.size() < fec_status_size) {
      return std::nullopt;
    }

    auto byte = [&](std::size_t i) {
      return (uint32_t) (uint8_t) payload[i];
    };
    auto be16 = [&](std::size_t i) {
      return (uint16_t) (byte(i) << 8 | byte(i + 1));
    };

    fec_status_t s;
    s.frame_index = byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
    s.highest_received_sequence = be16(4);
    s.next_contiguous_sequence = be16(6);
    s.missing_before_highest = be16(8);
    s.total_data = be16(10);
    s.total_parity = be16(12);
    s.received_data = be16(14);
    s.received_parity = be16(16);
    s.fec_percentage = (uint8_t) byte(18);
    s.block_index = (uint8_t) byte(19);
    s.block_count = (uint8_t) byte(20);
    return s;
  }

  /**
   * @brief Whether the client had enough packets to rebuild this FEC block: any mix of data and
   * parity packets as many as the data packets will do. The client reports a block either after
   * repairing it or just before dropping it, and this tells the two apart.
   */
  inline bool repairable(const fec_status_t &s) {
    return (uint32_t) s.received_data + s.received_parity >= s.total_data;
  }

  /// What a stream lost over some time
  struct counts_t {
    uint64_t frames_repaired = 0;  ///< Frames that needed FEC parity, all of whose blocks were rebuilt
    uint64_t frames_lost = 0;  ///< Frames with a block that couldn't be rebuilt
    uint64_t data_packets_missing = 0;  ///< Data packets missing from the reported blocks
    uint64_t data_packets = 0;  ///< Data packets in the reported blocks
    uint64_t invalidations = 0;  ///< Requests to stop referencing a range of frames
    uint64_t keyframe_requests = 0;

    /// Lost or repaired video. Keyframe requests alone don't count: a client asks for one or two
    /// when every stream starts and one when it ends.
    bool losses() const {
      return frames_repaired || frames_lost || invalidations;
    }

    void add(const counts_t &other) {
      frames_repaired += other.frames_repaired;
      frames_lost += other.frames_lost;
      data_packets_missing += other.data_packets_missing;
      data_packets += other.data_packets;
      invalidations += other.invalidations;
      keyframe_requests += other.keyframe_requests;
    }
  };

  /**
   * @brief Counts one stream's loss reports, for a log line every so often while there are any and
   * a total when the stream ends.
   *
   * Not thread-safe: Apollo calls it from the control stream's thread only.
   */
  class tracker_t {
  public:
    using clock = std::chrono::steady_clock;

    void add_fec_status(const fec_status_t &s, clock::time_point now) {
      start(now);

      // A large frame is sent as up to four FEC blocks, each reported on its own; blocks of one
      // frame arrive one after another
      if (!_frame || *_frame != s.frame_index) {
        finish_frame();
        _frame = s.frame_index;
      }
      _frame_lost = _frame_lost || !repairable(s);

      if (s.total_data > s.received_data) {
        _window.data_packets_missing += s.total_data - s.received_data;
      }
      _window.data_packets += s.total_data;
      _fec_percentage = s.fec_percentage;
    }

    void add_invalidation(clock::time_point now) {
      start(now);
      _window.invalidations++;
    }

    void add_keyframe_request(clock::time_point now) {
      start(now);
      _window.keyframe_requests++;
    }

    /**
     * @brief The line for the time since the last one, once `every` has passed and the client
     * reported lost or repaired video. Nothing otherwise.
     */
    std::optional<std::string> take_summary(clock::time_point now, clock::duration every) {
      start(now);
      if (now - _window_start < every) {
        return std::nullopt;
      }

      finish_frame();
      auto seconds = std::chrono::duration<double>(now - _window_start).count();
      std::optional<std::string> line;
      if (_window.losses()) {
        line = "in the last " + format_seconds(seconds) + ", " + describe(_window);
      }

      _total.add(_window);
      _window = {};
      _window_start = now;
      return line;
    }

    /**
     * @brief The whole stream's line, also when nothing was lost.
     */
    std::string take_total(clock::time_point now) {
      start(now);
      finish_frame();
      _total.add(_window);
      _window = {};

      auto seconds = std::chrono::duration<double>(now - _stream_start).count();
      return "over " + format_seconds(seconds) + ", " + describe(_total);
    }

    const counts_t &total() const {
      return _total;
    }

    /// Whether anything has been counted or asked for yet, i.e. the stream got under way
    bool started() const {
      return _started;
    }

  private:
    void start(clock::time_point now) {
      if (!_started) {
        _started = true;
        _stream_start = now;
        _window_start = now;
      }
    }

    void finish_frame() {
      if (_frame) {
        (_frame_lost ? _window.frames_lost : _window.frames_repaired)++;
        _frame.reset();
        _frame_lost = false;
      }
    }

    static std::string format_seconds(double seconds) {
      char buffer[32];
      if (seconds >= 120) {
        std::snprintf(buffer, sizeof(buffer), "%.1f min", seconds / 60);
      } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f s", seconds);
      }
      return buffer;
    }

    std::string describe(const counts_t &c) const {
      auto plural = [](uint64_t n, const char *one, const char *many) {
        return std::to_string(n) + ' ' + (n == 1 ? one : many);
      };

      std::string text = plural(c.frames_repaired, "frame", "frames") + " repaired with FEC, " +
                         plural(c.frames_lost, "frame", "frames") + " lost";
      if (c.data_packets) {
        text += " (" + std::to_string(c.data_packets_missing) + " of " + std::to_string(c.data_packets) +
                " data packets missing in those frames)";
      }
      text += ", " + plural(c.invalidations, "reference invalidation", "reference invalidations") + ", " +
              plural(c.keyframe_requests, "keyframe request", "keyframe requests");
      if (_fec_percentage) {
        text += "; FEC " + std::to_string(*_fec_percentage) + '%';
      }
      return text;
    }

    bool _started = false;
    clock::time_point _stream_start;
    clock::time_point _window_start;
    counts_t _window;
    counts_t _total;
    std::optional<uint32_t> _frame;
    bool _frame_lost = false;
    std::optional<uint8_t> _fec_percentage;
  };
}  // namespace frame_loss
