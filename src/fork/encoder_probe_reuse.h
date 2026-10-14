/**
 * @file src/fork/encoder_probe_reuse.h
 * @brief Fork-only: skip the full encoder probe on app launch when only the display layout changed.
 */
#pragma once

// standard includes
#include <string_view>

namespace video {
  struct encoder_t;
}

namespace fork_tweaks {
  /**
   * @brief Decide whether the current encoder probe result can be kept instead of probing all encoders again.
   * @param encoder The encoder chosen by the last full probe.
   * @return true if the probe can be skipped.
   *
   * Called when Windows reports that GPUs or displays changed, which happens on every launch that creates
   * a virtual display. The result is kept only if the last two full probes agreed, the GPUs and drivers are
   * unchanged, and a single test encode on the current display succeeds. Otherwise the full probe runs.
   */
  bool try_reuse_encoder_probe(const video::encoder_t &encoder);

  /**
   * @brief Make the next launch run a full encoder probe.
   * @param reason Logged with the warning.
   */
  void invalidate_encoder_probe(std::string_view reason);
}  // namespace fork_tweaks
