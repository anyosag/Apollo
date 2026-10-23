/**
 * @file src/fork/cursor_pulse.cpp
 * @brief Fork-only: the cursor pulse running now, between the HTTPS thread and the capture
 * thread (cursor_pulse.h).
 */
// standard includes
#include <atomic>
#include <mutex>

// local includes
#include "cursor_pulse.h"

namespace fork_tweaks::cursor_pulse {
  namespace {
    std::atomic<bool> capture_capable {false};
    std::mutex state_mutex;
    state_t state;
  }  // namespace

  void set_capable(bool capable) {
    capture_capable = capable;
  }

  bool capable() {
    return capture_capable;
  }

  void start(const request_t &request) {
    std::lock_guard lock {state_mutex};
    state.start(request, state_t::clock::now());
  }

  look_t look() {
    std::lock_guard lock {state_mutex};
    return state.look(state_t::clock::now());
  }

  bool animating() {
    std::lock_guard lock {state_mutex};
    return state.animating(state_t::clock::now());
  }
}  // namespace fork_tweaks::cursor_pulse
