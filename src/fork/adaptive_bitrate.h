/**
 * @file src/fork/adaptive_bitrate.h
 * @brief Fork-only: changing a stream's bitrate while it runs, for LuzLunar's adaptive bitrate.
 *
 * LuzLunar watches its stream's losses and round-trip time. When the network struggles it asks for
 * a lower bitrate, and when the network recovers, for its own bitrate back (POST /actions/bitrate,
 * adaptive_bitrate.cpp). A request is in the client's units, like the bitrate it launched with:
 * it's clamped between a floor and that bitrate, then converted to the encoder's bitrate the way
 * rtsp.cpp's ANNOUNCE converts the launch bitrate (FEC, audio and overhead come off). The encode
 * thread changes NVENC's bitrate between two frames, without a keyframe (nvenc_base's
 * set_bitrate()). Other encoders can't, and the client hears so.
 *
 * Plain C++ apart from the declarations at the end, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

namespace fork_tweaks::adaptive_bitrate {
  /// The lowest bitrate a request can set, in kbps; LuzLunar keeps its own floor above it
  constexpr std::int64_t min_kbps = 1000;

  /// What the stream launched with, so requests convert the same way (rtsp.cpp's ANNOUNCE)
  struct launch_t {
    std::int64_t client_kbps = 0;  ///< The client's bitrate after `max_bitrate`: no request goes above it; 0 for none
    std::int64_t warp_factor = 1;  ///< Apollo's warp mode multiplies the bitrate by it
    int fec_percentage = 20;
    std::int64_t audio_kbps = 0;  ///< What audio takes off, audio_kbps()
  };

  /// What rtsp.cpp takes off the bitrate for audio: 256 kbps a channel in high quality, 96 otherwise
  inline std::int64_t audio_kbps(bool high_quality, int channels) {
    return (std::int64_t) (high_quality ? 256 : 96) * channels;
  }

  /**
   * @brief The encoder's bitrate for a client bitrate: rtsp.cpp's ANNOUNCE, step by step (keep the
   * two in step). Warp mode first, then FEC (unless it's above 80%), then audio (at most a fifth) and
   * 500 kbps of overhead (at most a tenth).
   */
  inline std::int64_t encoder_kbps(const launch_t &launch, std::int64_t client_kbps) {
    std::int64_t kbps = client_kbps * std::max<std::int64_t>(launch.warp_factor, 1);
    if (launch.fec_percentage <= 80) {
      kbps /= 100.f / (100 - launch.fec_percentage);
    }
    kbps -= std::min(launch.audio_kbps, kbps / 5);
    kbps -= std::min((std::int64_t) 500, kbps / 10);
    return kbps;
  }

  /// The lowest a request can go: min_kbps, or the launch bitrate if that's lower
  inline std::int64_t floor_kbps(const launch_t &launch) {
    return std::min(launch.client_kbps, min_kbps);
  }

  /// A request between the floor and the launch bitrate
  inline std::int64_t clamp(const launch_t &launch, std::int64_t client_kbps) {
    return std::clamp(client_kbps, floor_kbps(launch), launch.client_kbps);
  }

  /// POST /actions/bitrate's body, {"kbps": N} with N a positive whole number
  inline std::optional<std::int64_t> parse_kbps(const std::string &body) {
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return std::nullopt;
    }
    auto it = json.find("kbps");
    if (it == json.end() || !it->is_number_integer()) {
      return std::nullopt;
    }
    auto kbps = it->get<std::int64_t>();
    if (kbps <= 0) {
      return std::nullopt;
    }
    return kbps;
  }

  /// NVENC's VBV size in bits for a bitrate, as nvenc_base's create_encoder() sizes it: one frame,
  /// plus `nvenc_vbv_increase` percent
  inline std::uint32_t vbv_bits(std::int64_t kbps, std::uint32_t framerate, int increase_percent) {
    if (framerate == 0 || kbps <= 0) {
      return 0;
    }
    auto size = (std::uint32_t) (kbps * 1000 / framerate);
    if (increase_percent > 0) {
      size += size * increase_percent / 100;
    }
    return size;
  }

  /// A stream's bitrate while it runs: the HTTPS thread's requests, the encode thread's changes
  struct live_t {
    launch_t launch;  ///< Set before the stream starts, then only read

    std::mutex mutex;
    std::int64_t target = 0;  ///< The client bitrate asked for last; 0 before any request
    std::int64_t requested = 0;  ///< An encoder bitrate waiting for the encode thread; 0 for none
    std::int64_t in_flight = 0;  ///< The encoder bitrate the encode thread is changing to; 0 for none
    std::int64_t applied = 0;  ///< The encoder bitrate in effect; 0 while it's the launch one
    bool refused = false;  ///< The encoder can't change its bitrate while streaming
    std::atomic<bool> pending {false};  ///< `requested` waits; read by every frame without the lock
  };

  /// What became of a request
  enum class request_e {
    queued,  ///< The encode thread changes to it before the next frame
    unchanged,  ///< The stream runs at it already
    not_live,  ///< The encoder can't change its bitrate, or the stream has none to change
  };

  /**
   * @brief A client's request, on the HTTPS thread.
   * @param client_kbps In the client's units, like the bitrate it launched with; clamped.
   */
  inline request_e request(live_t &live, std::int64_t client_kbps) {
    if (live.launch.client_kbps <= 0) {
      return request_e::not_live;
    }
    std::lock_guard lg(live.mutex);
    if (live.refused) {
      return request_e::not_live;
    }
    live.target = clamp(live.launch, client_kbps);
    auto encoder = encoder_kbps(live.launch, live.target);
    // A change the encode thread is making counts as made: comparing with the bitrate before it
    // would answer "unchanged" to a request for that bitrate and lose it
    auto in_effect = live.in_flight ? live.in_flight :
                     live.applied   ? live.applied :
                                      encoder_kbps(live.launch, live.launch.client_kbps);
    if (encoder == in_effect) {
      // Back to what runs: a change still waiting has nothing left to do
      live.requested = 0;
      live.pending = false;
      return request_e::unchanged;
    }
    live.requested = encoder;
    live.pending = true;
    return request_e::queued;
  }

  /**
   * @brief The encode thread's side: the encoder bitrate to change to before the next frame, if one
   * waits. Without one it only reads an atomic.
   */
  inline std::optional<std::int64_t> take(live_t &live) {
    if (!live.pending.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    std::lock_guard lg(live.mutex);
    live.pending = false;
    auto kbps = live.requested;
    live.requested = 0;
    if (kbps <= 0) {
      return std::nullopt;
    }
    live.in_flight = kbps;
    return kbps;
  }

  /// What an encoder did with a bitrate
  enum class change_e {
    changed,
    unsupported,  ///< It can't change its bitrate while streaming, ever
    failed,  ///< Not this time; it keeps the bitrate it had, and a later request may work
  };

  /// The encode thread's side: what the encoder did with the bitrate take() gave it
  inline void done(live_t &live, std::int64_t kbps, change_e change) {
    std::lock_guard lg(live.mutex);
    live.in_flight = 0;
    switch (change) {
      case change_e::changed:
        live.applied = kbps;
        break;
      case change_e::unsupported:
        live.refused = true;
        live.requested = 0;
        live.pending = false;
        break;
      case change_e::failed:
        break;
    }
  }

  /// The bitrate an encoder starts with after a capture restart: the one changed to last, if any
  inline std::int64_t restart_kbps(live_t &live, std::int64_t launch_kbps) {
    std::lock_guard lg(live.mutex);
    return live.applied ? live.applied : launch_kbps;
  }

  /// What GET and POST /actions/bitrate report
  struct status_t {
    std::int64_t target = 0;  ///< The client bitrate the stream runs at, or is changing to
    std::int64_t encoder = 0;  ///< The encoder's bitrate in effect
    std::int64_t min = 0;  ///< The lowest a request can set
    std::int64_t max = 0;  ///< The highest: the launch bitrate
    bool live = true;  ///< The encoder can change its bitrate while streaming, as far as it's known
    bool pending = false;  ///< A change waits for the encode thread
  };

  inline status_t status(live_t &live) {
    std::lock_guard lg(live.mutex);
    status_t status;
    status.max = live.launch.client_kbps;
    status.min = floor_kbps(live.launch);
    status.target = live.target ? live.target : live.launch.client_kbps;
    status.encoder = live.applied ? live.applied : encoder_kbps(live.launch, live.launch.client_kbps);
    status.live = !live.refused && live.launch.client_kbps > 0;
    status.pending = live.pending.load();
    return status;
  }

  // adaptive_bitrate.cpp

  /// A paired client asking about its own stream
  struct client_t {
    std::string name;
    std::string uuid;
    bool permitted = false;  ///< It may view streams (View or Launch)
  };

  /// An HTTP status and a JSON body
  struct reply_t {
    int status = 200;
    std::string body;
  };

  /// `fork_adaptive_bitrate` is on
  bool supported();

  /**
   * @brief GET or POST /actions/bitrate. POST {"kbps": N} asks for N kbps in the client's units.
   * Only raises the change for the encode thread: nvhttp serves HTTPS on one thread.
   */
  reply_t handle(const client_t &client, const std::string &method, const std::string &body);

  /**
   * @brief In the encode loop, between two frames: changes the encoder to the bitrate a client asked
   * for, if one waits, and logs it.
   * @param bitrate The encode loop's config.bitrate, updated when the encoder took it.
   * @param set_bitrate The encode session's set_bitrate().
   */
  void apply(live_t &live, int &bitrate, const std::function<change_e(int kbps)> &set_bitrate);
}  // namespace fork_tweaks::adaptive_bitrate
