/**
 * @file src/fork/adaptive_bitrate.cpp
 * @brief Fork-only: LuzLunar's requests to change its stream's bitrate, and the encode thread's
 * changes (adaptive_bitrate.h).
 */
// standard includes
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "adaptive_bitrate.h"
#include "fork_config.h"
#include "src/logging.h"
// As rtsp.cpp has them: network.h's Boost.Asio before rtsp.h's windows.h, which would bring in
// WinSock 1 first, and rtsp.h before stream.h, which uses its launch_session_t
#include "src/network.h"
#include "src/rtsp.h"
#include "src/stream.h"

using namespace std::literals;

namespace fork_tweaks::adaptive_bitrate {
  namespace {
    reply_t error_reply(int status, std::string_view error) {
      return {status, nlohmann::json {{"error", std::string {error}}}.dump()};
    }

    std::string status_json(const status_t &status) {
      return nlohmann::json {
        {"target", status.target},
        {"encoder", status.encoder},
        {"min", status.min},
        {"max", status.max},
        {"live", status.live},
        {"known", status.known},
        {"pending", status.pending},
      }
        .dump();
    }
  }  // namespace

  bool supported() {
    return live_bitrate;
  }

  reply_t handle(const client_t &client, const std::string &method, const std::string &body) {
    if (!client.permitted) {
      BOOST_LOG(debug) << "fork: adaptive bitrate: client ["sv << client.name << "] may not view streams"sv;
      return error_reply(401, "permission");
    }
    if (!supported()) {
      return error_reply(404, "unsupported");
    }
    if (method != "GET" && method != "POST") {
      return error_reply(400, "method");
    }

    // Only the client's own stream, by its certificate
    auto session = rtsp_stream::find_session(client.uuid);
    if (!session) {
      return error_reply(403, "not_streaming");
    }
    auto live = stream::session::fork_bitrate(*session);
    if (!live) {
      return error_reply(409, "not_live");
    }

    if (method == "GET") {
      return {200, status_json(status(*live))};
    }

    auto kbps = parse_kbps(body);
    if (!kbps) {
      return error_reply(400, "bad_request");
    }
    switch (request(*live, *kbps)) {
      case request_e::queued:
        {
          auto now = status(*live);
          BOOST_LOG(info) << "fork: adaptive bitrate: client ["sv << client.name << "] asks for "sv << *kbps << " kbps; "sv << now.target << " kbps, "sv << encoder_kbps(live->launch, now.target) << " kbps for the encoder"sv;
          return {202, status_json(now)};
        }
      case request_e::unchanged:
        return {200, status_json(status(*live))};
      case request_e::not_live:
        break;
    }
    return {409, status_json(status(*live))};
  }

  void apply(live_t &live, int &bitrate, const std::function<change_e(int kbps)> &set_bitrate) {
    auto kbps = take(live);
    if (!kbps) {
      return;
    }
    auto change = set_bitrate((int) *kbps);
    done(live, *kbps, change);
    switch (change) {
      case change_e::changed:
        BOOST_LOG(info) << "fork: adaptive bitrate: the encoder runs at "sv << *kbps << " kbps (was "sv << bitrate << ')';
        bitrate = (int) *kbps;
        break;
      case change_e::failed:
        BOOST_LOG(warning) << "fork: adaptive bitrate: the encoder didn't take "sv << *kbps << " kbps; it stays at "sv << bitrate << " kbps until the next request"sv;
        break;
      case change_e::unsupported:
        BOOST_LOG(warning) << "fork: adaptive bitrate: this encoder can't change its bitrate while streaming; it stays at "sv << bitrate << " kbps"sv;
        break;
    }
  }

  void ready(live_t &live, bool can_change) {
    encoder_ready(live, can_change);
    if (!can_change && live.launch.client_kbps > 0) {
      BOOST_LOG(info) << "fork: adaptive bitrate: this encoder can't change its bitrate while streaming; requests get 409"sv;
    }
  }
}  // namespace fork_tweaks::adaptive_bitrate
