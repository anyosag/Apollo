/**
 * @file src/fork/udp_send_win.cpp
 * @brief The stream's UDP sends on Windows, retried while Windows is briefly out of send buffers.
 */
#ifdef _WIN32
  // platform includes
  #include <winsock2.h>
  #include <windows.h>

  // standard includes
  #include <algorithm>
  #include <chrono>
  #include <memory>

  // local includes
  #include "src/logging.h"
  #include "src/platform/common.h"
  #include "udp_send_retry.h"
  #include "udp_send_win.h"

using namespace std::literals;

namespace fork_tweaks::udp_send {
  namespace {
    /// WSASendMsg, retried while Windows is out of send buffers; 0 or the Winsock error
    int send_with_retry(SOCKET socket, WSAMSG &msg) {
      auto result = udp_send_retry::send(
        [&]() {
          DWORD bytes_sent;
          return WSASendMsg(socket, &msg, 0, &bytes_sent, nullptr, nullptr) == SOCKET_ERROR ? WSAGetLastError() : 0;
        },
        [&](int error, std::chrono::steady_clock::duration remaining) {
          // Until the socket takes more, at most 1 ms at a time
          fd_set writable;
          FD_ZERO(&writable);
          FD_SET(socket, &writable);
          auto wait_us = std::clamp<long long>(std::chrono::duration_cast<std::chrono::microseconds>(remaining).count(), 1, 1000);
          timeval timeout {0, static_cast<long>(wait_us)};
          auto ready = select(0, nullptr, &writable, nullptr, &timeout);
          if (ready == SOCKET_ERROR) {
            return WSAGetLastError();
          }
          // Out of buffers can leave the socket writable, so back off a little before trying again
          if (ready > 0 && error == WSAENOBUFS) {
            thread_local auto timer = platf::create_high_precision_timer();
            if (timer && *timer) {
              timer->sleep_for(std::min<std::chrono::nanoseconds>(100us, remaining));
            }
          }
          return 0;
        },
        []() {
          return std::chrono::steady_clock::now();
        }
      );
      return result.error;
    }
  }  // namespace

  bool send_batch(std::uintptr_t socket, _WSAMSG &msg, std::size_t block_count, std::size_t datagram_size, bool ipv6) {
    auto error = send_with_retry((SOCKET) socket, msg);
    switch (udp_send_retry::batch_outcome(error, block_count > 1, datagram_size, ipv6)) {
      case udp_send_retry::batch_e::sent:
        return true;
      case udp_send_retry::batch_e::one_by_one:
        // No segmentation offload here, as on some network adapters: the caller sends one by one
        return false;
      case udp_send_retry::batch_e::dropped:
        break;
    }

    thread_local udp_send_retry::log_limiter_t limiter;
    if (auto failures = limiter.due(std::chrono::steady_clock::now())) {
      BOOST_LOG(warning) << "WSASendMsg() batch of "sv << block_count << " packets failed: "sv << error << "; batches dropped since the last report: "sv << failures;
    }
    return true;
  }

  bool send_one(std::uintptr_t socket, _WSAMSG &msg) {
    auto error = send_with_retry((SOCKET) socket, msg);
    if (error == 0) {
      return true;
    }

    thread_local udp_send_retry::log_limiter_t limiter;
    if (auto failures = limiter.due(std::chrono::steady_clock::now())) {
      BOOST_LOG(warning) << "WSASendMsg() failed: "sv << error << "; failures since the last report: "sv << failures;
    }
    return false;
  }
}  // namespace fork_tweaks::udp_send
#endif
