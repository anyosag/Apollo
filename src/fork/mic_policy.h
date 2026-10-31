/**
 * @file src/fork/mic_policy.h
 * @brief The Mac's microphone on the PC, the parts without Windows: the jitter buffer between the
 * client's FRAMEs and the Steam microphone's clock, when the client should record, and which
 * default recording device to put back.
 *
 * Header-only, so tests/fork/header/mic_policy_test.cpp checks it without the rest of Apollo. After
 * Foundation Sunshine's mic_mixer (AlkaidLab/foundation-sunshine, GPLv3): sequence numbers unwrapped
 * on a timeline, a timeline that starts over on a jump; the delay control is the fork's own.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "luzmic.h"

namespace mic_policy {

  /// What jitter_buffer_t::pull() is told to send to the frames still waiting: one Opus frame is a slot
  constexpr int k_start_frames = 3;  ///< Playing starts with 60 ms buffered: the slot that plays and two more
  constexpr int k_min_queued = 1;  ///< A second whose fewest frames waiting after a slot was under 1 (40 ms of delay) waits a slot
  constexpr int k_max_queued = 3;  ///< A second whose fewest waiting was over 3 (80 ms) drops the extra frames, back to the start's
  constexpr int k_window = 50;  ///< Slots in that second
  constexpr int k_max_waiting = 25;  ///< More than 500 ms waiting (a burst after a stall): back to k_start_frames
  constexpr int k_max_waits = 5;  ///< 100 ms with nothing to play: silence, until FRAMEs come back
  constexpr std::int64_t k_jump_ms = 500;  ///< A FRAME this far off the time its sequence says starts a new timeline

  /// How long nobody records before the client is told to stop, so an app that reopens the
  /// microphone (a call that changes devices) doesn't turn the Mac's off and on
  constexpr std::chrono::milliseconds k_stop_delay {3000};
  constexpr std::chrono::milliseconds k_poll_interval {500};  ///< How often the host looks at who records

  /// What to play in one slot
  struct slot_t {
    enum class play_e {
      silence,  ///< Nothing: zeros, the decoder untouched
      frame,  ///< Decode opus
      fec,  ///< This slot's frame was lost: rebuild it from opus, the next frame's redundancy (decode_fec)
      conceal,  ///< No frame: the decoder's packet loss concealment
    };

    play_e play = play_e::silence;
    std::vector<std::uint8_t> opus;
    std::vector<std::uint8_t> skipped;  ///< A frame dropped to cut the delay: decode it first and throw the sound away, so the decoder's state runs on
    bool reset = false;  ///< A new timeline: reset the decoder first
  };

  struct stats_t {
    std::uint64_t frames = 0;  ///< Played
    std::uint64_t fec = 0;  ///< Lost, rebuilt from the next frame
    std::uint64_t concealed = 0;  ///< Lost, concealed
    std::uint64_t waits = 0;  ///< Slots concealed while waiting for a late frame
    std::uint64_t late = 0;  ///< Arrived after their slot played
    std::uint64_t duplicates = 0;
    std::uint64_t dropped = 0;  ///< Dropped to cut the delay
    std::uint64_t restarts = 0;  ///< New timelines (the first included)
    std::uint64_t underruns = 0;  ///< Times playing stopped for lack of frames
  };

  /**
   * The FRAMEs of one client, in order, one slot each k_frame_ms of the Steam microphone's clock.
   * Not thread-safe: the caller locks around push() (the control stream's thread) and pull() (the
   * microphone's).
   *
   * The delay follows the network: the fewest frames left waiting after a slot, over each second,
   * is the margin the jitter left. Under k_min_queued, the next slot conceals and waits (20 ms more
   * delay); over k_max_queued, it drops the frames past the start's margin at once (one skip, not
   * one a second). The two clocks drift apart the same way, slowly, so the Mac's clock running fast
   * or slow costs a frame now and then.
   */
  class jitter_buffer_t {
  public:
    /// False when the frame was thrown away: late, a duplicate, or not a frame
    bool push(const luzmic::frame_t &frame) {
      if (frame.opus == nullptr || frame.opus_size == 0) {
        return false;
      }
      std::vector<std::uint8_t> opus(frame.opus, frame.opus + frame.opus_size);

      if (!m_timeline || frame.start()) {
        restart(frame);
        m_frames.emplace(m_last, std::move(opus));
        trim_waiting();
        return true;
      }

      const auto delta = static_cast<std::uint16_t>(frame.sequence - m_last_sequence);
      const std::int32_t distance = delta < 0x8000 ? delta : static_cast<std::int32_t>(delta) - 0x10000;
      if (distance == 0) {
        ++m_stats.duplicates;
        return false;
      }

      if (distance > 0) {
        const auto time_delta = static_cast<std::uint32_t>(frame.time_ms - m_last_time);
        const std::int64_t elapsed = time_delta < 0x80000000u ? time_delta : static_cast<std::int64_t>(time_delta) - 0x100000000LL;
        const std::int64_t off = elapsed - static_cast<std::int64_t>(distance) * luzmic::k_frame_ms;
        if (elapsed < 0 || off > k_jump_ms || off < -k_jump_ms) {
          // The client paused, its clock jumped, or a long stretch was lost: start over rather than catch up
          restart(frame);
          m_frames.emplace(m_last, std::move(opus));
          trim_waiting();
          return true;
        }
        m_last += distance;
        m_last_sequence = frame.sequence;
        m_last_time = frame.time_ms;
        m_frames.emplace(m_last, std::move(opus));
        trim_waiting();
        return true;
      }

      const std::int64_t at = m_last + distance;
      if (at < m_first || (m_playing && at < m_next)) {
        ++m_stats.late;
        return false;
      }
      if (!m_frames.emplace(at, std::move(opus)).second) {
        ++m_stats.duplicates;
        return false;
      }
      return true;
    }

    /// The next slot
    slot_t pull() {
      slot_t slot;
      if (!m_playing) {
        if (m_frames.empty() || m_last - m_frames.begin()->first + 1 < k_start_frames) {
          return slot;
        }
        m_playing = true;
        m_next = m_frames.begin()->first;
        m_waits = 0;
        m_window_slots = 0;
        m_window_fewest = INT_MAX;
        m_adjust = 0;
        slot.reset = true;
      }

      for (int i = 0; i < m_adjust && m_next < m_last; ++i) {
        // Drop the frames due now (or the holes where they were lost), all at once, and play the
        // next one in their slot
        auto due = m_frames.find(m_next);
        if (due != m_frames.end()) {
          slot.skipped = std::move(due->second);
          m_frames.erase(due);
        }
        ++m_next;
        ++m_stats.dropped;
      }
      const bool wait = m_adjust < 0;
      m_adjust = 0;

      auto due = m_frames.find(m_next);
      if (wait && due != m_frames.end()) {
        // Too little margin: conceal one slot more and play this frame in the next
        slot.play = slot_t::play_e::conceal;
        ++m_stats.waits;
      } else if (due != m_frames.end()) {
        slot.play = slot_t::play_e::frame;
        slot.opus = std::move(due->second);
        m_frames.erase(due);
        ++m_next;
        m_waits = 0;
        ++m_stats.frames;
      } else if (!m_frames.empty()) {
        // A later frame came, so this one is lost
        auto after = m_frames.find(m_next + 1);
        if (after != m_frames.end()) {
          slot.play = slot_t::play_e::fec;
          slot.opus = after->second;
          ++m_stats.fec;
        } else {
          slot.play = slot_t::play_e::conceal;
          ++m_stats.concealed;
        }
        ++m_next;
        m_waits = 0;
      } else if (m_waits < k_max_waits) {
        // Nothing yet: the frame is late, or the client stopped. Wait for it, concealing
        slot.play = slot_t::play_e::conceal;
        ++m_waits;
        ++m_stats.waits;
      } else {
        // The client stopped (or the network did): silence, and the next frames start playing over
        m_playing = false;
        ++m_stats.underruns;
        return slot;
      }

      // The margin the network left this second
      m_window_fewest = std::min(m_window_fewest, waiting());
      if (++m_window_slots >= k_window) {
        if (m_window_fewest > k_max_queued) {
          m_adjust = m_window_fewest - (k_start_frames - 1);
        } else if (m_window_fewest < k_min_queued && m_waits == 0) {
          m_adjust = -1;
        }
        m_window_slots = 0;
        m_window_fewest = INT_MAX;
      }
      return slot;
    }

    /// Frames waiting for later slots, holes included (0 while not playing)
    int waiting() const {
      return m_playing ? static_cast<int>(std::max<std::int64_t>(0, m_last + 1 - m_next)) : 0;
    }

    bool playing() const {
      return m_playing;
    }

    stats_t take_stats() {
      return std::exchange(m_stats, {});
    }

  private:
    void restart(const luzmic::frame_t &frame) {
      ++m_stats.restarts;
      m_timeline = true;
      m_frames.clear();
      // A new timeline continues the old one's numbers, so nothing from the old one can land in it
      m_last += k_max_waiting * 4;
      m_first = m_last;
      m_last_sequence = frame.sequence;
      m_last_time = frame.time_ms;
      m_playing = false;
    }

    /// Keep the delay bounded: while waiting to play, the last k_start_frames; while playing, a
    /// burst past k_max_waiting goes back to k_start_frames
    void trim_waiting() {
      if (!m_playing) {
        while (!m_frames.empty() && m_last - m_frames.begin()->first + 1 > k_start_frames) {
          m_frames.erase(m_frames.begin());
        }
        return;
      }
      if (m_last + 1 - m_next <= k_max_waiting) {
        return;
      }
      const std::int64_t keep_from = m_last + 1 - k_start_frames;
      while (!m_frames.empty() && m_frames.begin()->first < keep_from) {
        m_frames.erase(m_frames.begin());
        ++m_stats.dropped;
      }
      m_next = keep_from;
    }

    std::map<std::int64_t, std::vector<std::uint8_t>> m_frames;  ///< By place on the timeline
    bool m_timeline = false;
    std::int64_t m_first = 0;  ///< The timeline's first frame: anything before is from an older one
    std::int64_t m_last = 0;  ///< The newest frame's place
    std::uint16_t m_last_sequence = 0;
    std::uint32_t m_last_time = 0;
    bool m_playing = false;
    std::int64_t m_next = 0;  ///< The next slot's place, while playing
    int m_waits = 0;  ///< Slots in a row concealed for lack of any frame
    int m_window_slots = 0;
    int m_window_fewest = INT_MAX;
    int m_adjust = 0;  ///< The next slot drops this many frames, or waits (-1)
    stats_t m_stats;
  };

  /**
   * Whether to tell the client to record, from whether an app records from the Steam microphone,
   * looked at every k_poll_interval: at once when one starts, k_stop_delay after the last stops.
   */
  class recording_t {
  public:
    bool update(bool anyone, std::chrono::steady_clock::time_point now) {
      if (anyone) {
        m_last_seen = now;
        m_recording = true;
      } else if (m_recording && now - m_last_seen >= k_stop_delay) {
        m_recording = false;
      }
      return m_recording;
    }

    bool recording() const {
      return m_recording;
    }

  private:
    bool m_recording = false;
    std::chrono::steady_clock::time_point m_last_seen {};
  };

  /**
   * The default recording device a role goes back to when the stream ends: the one it had, if it
   * still has the Steam microphone. Nothing when someone chose another meanwhile, when it had the
   * Steam microphone already, or when it had none.
   */
  inline std::optional<std::wstring> restore_default(const std::wstring &saved, const std::wstring &current, const std::wstring &steam_mic) {
    if (saved.empty() || saved == steam_mic || current != steam_mic) {
      return std::nullopt;
    }
    return saved;
  }

}  // namespace mic_policy
