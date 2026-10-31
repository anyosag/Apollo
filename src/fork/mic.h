/**
 * @file src/fork/mic.h
 * @brief Fork-only: the Mac's microphone on the PC, through the Steam Streaming Microphone.
 *
 * A stream launched with luzMic=1 (fork_mic on, /serverinfo's <LuzMic>1, the client's Microphone
 * permission, an encrypted control stream, and not a display group's slot past 0) gets a
 * session_t, if no other stream has the microphone. Its thread:
 *  - finds the Steam Streaming Microphone's two endpoints, installing Steam's driver first (from
 *    Steam's folder, as Apollo does Steam Streaming Speakers) when they're missing and
 *    install_steam_audio_drivers is on, and keeping the default output what it was;
 *  - makes "Microphone (Steam Streaming Microphone)" Windows' default recording device for the
 *    stream (every role), and puts back the one each role had, if it still has the Steam one;
 *  - plays the client's FRAMEs (luzmic.h) into "Speakers (Steam Streaming Microphone)": Opus
 *    decoded, through mic_policy's jitter buffer, one 20 ms slot at a time as WASAPI asks
 *    (shared mode, event-driven, mono 48 kHz converted by Windows);
 *  - looks at the recording endpoint's audio sessions every 500 ms, and tells the client (STATE,
 *    on the control stream) when an app starts recording and 3 s after the last stops, so the Mac's
 *    microphone is live only while the PC uses it.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fork_mic {
  class session_t;

  /// fork_mic is on, and the platform can (Windows): /serverinfo's <LuzMic> and the feature flag
  bool enabled();

  /**
   * @brief The stream's microphone, from its start; nothing when another stream has it, or there's
   * no Windows audio.
   * @param send_state Queues a STATE's payload on the stream's control stream; called on the
   * microphone's thread, until stop() returns.
   */
  std::shared_ptr<session_t> start(std::function<void(std::string payload)> send_state);

  /// A FRAME's whole input packet from the session's client, on the control stream's thread
  void receive(session_t &session, const std::uint8_t *packet, std::size_t size);

  /// At the stream's end: the thread stops (quickly: a driver being installed carries on alone),
  /// the default recording device goes back, and the next stream may have the microphone. The
  /// session stays, for frames still arriving, which go nowhere
  void stop(session_t &session);
}  // namespace fork_mic
