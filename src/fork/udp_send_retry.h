/**
 * @file src/fork/udp_send_retry.h
 * @brief Sending the stream's UDP packets when Windows is briefly out of send buffers, apart from
 * any I/O: how long a send is retried, which errors mean a batch should go packet by packet
 * instead, and how often a failure is logged.
 *
 * Windows answers WSAENOBUFS (or WSAEWOULDBLOCK) when its send buffers are full for a moment, as
 * at the start of a large keyframe. A send is then retried for up to 2 ms. A batch that still
 * can't go is dropped, as a lost packet would be, rather than sent again packet by packet: that
 * would meet the same full buffers dozens of times over. Only a batch Windows refuses to segment
 * (no UDP segmentation offload) goes packet by packet, as it always did. After Vibepollo's
 * src/platform/windows/udp_send_retry.h.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace udp_send_retry {
  using clock = std::chrono::steady_clock;

  /// How long one send is retried while Windows is out of send buffers
  constexpr auto budget = std::chrono::milliseconds(2);

  /// How often a failing send is logged
  constexpr auto log_interval = std::chrono::seconds(5);

  /// Winsock's error numbers (winerror.h), so this builds without it
  namespace wsa {
    constexpr int interrupted = 10004;  ///< WSAEINTR
    constexpr int invalid = 10022;  ///< WSAEINVAL
    constexpr int would_block = 10035;  ///< WSAEWOULDBLOCK
    constexpr int message_size = 10040;  ///< WSAEMSGSIZE
    constexpr int no_protocol_option = 10042;  ///< WSAENOPROTOOPT
    constexpr int not_supported = 10045;  ///< WSAEOPNOTSUPP
    constexpr int no_buffers = 10055;  ///< WSAENOBUFS
  }  // namespace wsa

  /// Windows is out of send buffers for the moment: worth trying again
  inline bool pressure(int error) {
    return error == wsa::no_buffers || error == wsa::would_block;
  }

  /**
   * @brief Windows refused to segment a batch (no UDP segmentation offload), so its packets should
   * go one by one.
   * @param segmented The batch holds more than one packet.
   * @param datagram_size One packet's size, header included.
   */
  inline bool offload_refused(int error, bool segmented, std::size_t datagram_size, bool ipv6) {
    if (!segmented) {
      return false;
    }
    if (error == wsa::invalid || error == wsa::not_supported || error == wsa::no_protocol_option) {
      return true;
    }
    // A large segmented message can be refused even when each packet in it fits
    return error == wsa::message_size && datagram_size <= (ipv6 ? 65527u : 65507u);
  }

  /// What a batch's send came to
  enum class batch_e {
    sent,
    one_by_one,  ///< Windows refused to segment it: send its packets one by one
    dropped,  ///< It couldn't go; logged, like a lost packet
  };

  inline batch_e batch_outcome(int error, bool segmented, std::size_t datagram_size, bool ipv6) {
    if (error == 0) {
      return batch_e::sent;
    }
    return offload_refused(error, segmented, datagram_size, ipv6) ? batch_e::one_by_one : batch_e::dropped;
  }

  struct result_t {
    int error = 0;  ///< 0 once it went, else the last Winsock error
    bool timed_out = false;  ///< Still out of buffers when the budget ran out
    unsigned retries = 0;
  };

  /**
   * @brief Sends, and while Windows is out of send buffers waits and sends again, for up to the
   * budget. Nothing is sent again after the deadline, even if the wait returns at once.
   * @param attempt Sends once; returns 0 or a Winsock error.
   * @param wait Given the error and the time left, waits until a send might go; returns 0 or a
   * Winsock error, which ends the retries.
   * @param now The current time.
   */
  template<class Attempt, class Wait, class Now>
  result_t send(Attempt &&attempt, Wait &&wait, Now &&now) {
    const auto deadline = now() + budget;
    result_t result;
    for (;;) {
      result.error = attempt();
      if (!pressure(result.error)) {
        return result;
      }
      const auto remaining = deadline - now();
      if (remaining <= clock::duration::zero()) {
        result.timed_out = true;
        return result;
      }
      if (const auto wait_error = wait(result.error, remaining); wait_error != 0) {
        result.error = wait_error;
        return result;
      }
      if (now() >= deadline) {
        result.timed_out = true;
        return result;
      }
      ++result.retries;
    }
  }

  /// Lets one report through every log_interval, counting the failures in between
  class log_limiter_t {
  public:
    /// @return 0 to stay quiet, else how many failures this report covers, this one included
    std::uint64_t due(clock::time_point now) {
      ++held_;
      if (started_ && now < next_) {
        return 0;
      }
      started_ = true;
      next_ = now + log_interval;
      auto count = held_;
      held_ = 0;
      return count;
    }

  private:
    bool started_ = false;
    clock::time_point next_ {};
    std::uint64_t held_ = 0;
  };
}  // namespace udp_send_retry
