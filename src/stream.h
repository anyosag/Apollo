/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);
    bool uuid_match(const session_t& session, const std::string_view& uuid);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void graceful_stop(session_t& session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);

    /// [fork] The session's bitrate changes while it runs (fork/adaptive_bitrate.h); null for none
    std::shared_ptr<fork_tweaks::adaptive_bitrate::live_t> fork_bitrate(session_t &session);

    /// [fork] How many sessions stream now, without taking any lock (fork/vdisplay_recovery.cpp)
    unsigned running_count();

    /// [fork] The display group slot the session streams (fork/display_group.h); -1 outside a group
    int fork_slot(const session_t &session);

    /// [fork] The session's pointer drawn by its client (fork/client_cursor.h); null for none
    std::shared_ptr<fork_tweaks::client_cursor::session_t> fork_cursor(session_t &session);

    /**
     * @brief [fork] Sends the client one of the fork's own control messages, a type from 0x4C00 to
     * 0x4CFF (fork/luzcursor.h), which LuzLunar's moonlight-common-c patch 0004 hands to it:
     * encrypted, reliable, on ENet channel `channel` (moonlight-common-c leaves 0x06 to 0x0F free).
     * Thread safe: it's queued, and the control stream thread sends it at its next turn, within
     * 150 ms (5 ms while the client draws the pointer), in the order queued. False when it won't go:
     * a type outside the range, a channel past 0x2F, or a control stream that isn't encrypted.
     */
    bool send_fork_message(session_t &session, std::uint16_t type, std::string payload, std::uint8_t channel);
  }  // namespace session
}  // namespace stream
