/**
 * @file src/fork/udp_send_win.h
 * @brief The stream's UDP sends on Windows, retried while Windows is briefly out of send buffers
 * (udp_send_retry.h). Called by src/platform/windows/misc.cpp's send_batch() and send().
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>

struct _WSAMSG;

namespace fork_tweaks::udp_send {
  /**
   * @brief Sends a batch of packets with one WSASendMsg.
   * @param block_count The packets in the batch; more than one means it's segmented.
   * @param datagram_size One packet's size, header included.
   * @return False only when Windows refused to segment the batch, so the caller sends the packets
   * one by one. A batch that couldn't go for any other reason is logged and dropped, like a lost
   * packet.
   */
  bool send_batch(std::uintptr_t socket, _WSAMSG &msg, std::size_t block_count, std::size_t datagram_size, bool ipv6);

  /**
   * @brief Sends one packet.
   * @return Whether it went. Failures are logged at most every 5 seconds.
   */
  bool send_one(std::uintptr_t socket, _WSAMSG &msg);
}  // namespace fork_tweaks::udp_send
