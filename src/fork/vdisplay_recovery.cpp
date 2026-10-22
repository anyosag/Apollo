/**
 * @file src/fork/vdisplay_recovery.cpp
 * @brief Fork-only: brings a stream's virtual display back when Windows switches it off mid-stream
 * or the driver loses it, and puts capture back on it.
 */
// standard includes
#include <chrono>
#include <cstring>
#include <optional>

// local includes
#include "src/logging.h"
#include "vdisplay_recovery.h"
#include "vdisplay_recovery_policy.h"

#ifdef _WIN32
  #include "src/globals.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/process.h"
  #include "src/rtsp.h"
#endif

using namespace std::literals;

namespace fork_tweaks::vdisplay_recovery {
  namespace policy = vdisplay_recovery_policy;

  namespace {
    std::mutex capture_mutex;
    std::string capture_display;  ///< Under capture_mutex

#ifdef _WIN32
    long long ms_since(policy::clock::time_point then, policy::clock::time_point now) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count();
    }
#endif
  }  // namespace

  void note_capture(const std::string &display) {
    std::lock_guard lock {capture_mutex};
    capture_display = display;
  }

  void forget_capture() {
    std::lock_guard lock {capture_mutex};
    capture_display.clear();
  }

  std::string captured() {
    std::lock_guard lock {capture_mutex};
    return capture_display;
  }

  monitor_t::monitor_t(const std::array<std::uint8_t, 16> &guid):
      _guid(guid) {
#ifdef _WIN32
    _thread = std::thread(&monitor_t::run, this);
#endif
  }

  monitor_t::~monitor_t() {
    {
      std::lock_guard lock {_mutex};
      _stop = true;
    }
    _wake.notify_all();
    if (_thread.joinable()) {
      if (_thread.get_id() == std::this_thread::get_id()) {
        _thread.detach();
      } else {
        _thread.join();
      }
    }
  }

  void monitor_t::settle() {
    _settle = true;
  }

  void monitor_t::run() {
#ifdef _WIN32
    GUID guid;
    std::memcpy(&guid, _guid.data(), sizeof(guid));

    const policy::timing_t timing;
    policy::monitor_t decide(policy::clock::now(), timing);
    policy::capture_follow_t follow;

    // It's on: it was just set up
    auto last = VDISPLAY::PRESENCE::ACTIVE;
    std::optional<policy::clock::time_point> off_since;

    while (true) {
      {
        std::unique_lock lock {_mutex};
        if (_wake.wait_for(lock, timing.poll, [this]() {
              return _stop;
            })) {
          return;
        }
      }

      auto now = policy::clock::now();
      if (_settle.exchange(false)) {
        decide.settle(now);
        follow.reset();
        last = VDISPLAY::PRESENCE::ACTIVE;
        off_since.reset();
      }

      // A launch, resume or termination is changing the display: look again next time
      auto lock = proc::proc_t::try_lock_virtual_display();
      if (!lock.owns_lock()) {
        continue;
      }

      // Between streams the display is the PC user's business, and so is it while the display
      // shortcut has capture on another display
      auto preferred = proc::proc.stream_display();
      if (rtsp_stream::get_all_session_uuids().empty() || (!preferred.empty() && preferred != proc::proc.virtual_display_name())) {
        decide.step(now, policy::presence_e::unknown, false);
        follow.reset();
        continue;
      }

      std::wstring name_w;
      auto presence = VDISPLAY::getVirtualDisplayPresence(guid, name_w);
      auto name = platf::to_utf8(name_w);

      if (presence != last && presence != VDISPLAY::PRESENCE::UNKNOWN) {
        if (last == VDISPLAY::PRESENCE::ACTIVE) {
          off_since = now;
          BOOST_LOG(warning) << "vdisplay: "sv << proc::proc.virtual_display_name()
                             << (presence == VDISPLAY::PRESENCE::MISSING ? " is gone from the driver"sv : " was switched off by Windows"sv)
                             << " mid-stream"sv;
        }
        last = presence;
      }

      auto to_policy = [](VDISPLAY::PRESENCE p) {
        switch (p) {
          case VDISPLAY::PRESENCE::ACTIVE:
            return policy::presence_e::active;
          case VDISPLAY::PRESENCE::INACTIVE:
            return policy::presence_e::inactive;
          case VDISPLAY::PRESENCE::MISSING:
            return policy::presence_e::missing;
          case VDISPLAY::PRESENCE::UNKNOWN:
            break;
        }
        return policy::presence_e::unknown;
      };

      switch (decide.step(now, to_policy(presence), true)) {
        case policy::action_e::reactivate:
          BOOST_LOG(info) << "vdisplay: switching the virtual display on again ("sv << decide.reactivations() << " of "sv << timing.max_reactivations << ')';
          proc::proc.reactivate_virtual_display();
          break;
        case policy::action_e::recreate:
          BOOST_LOG(info) << "vdisplay: re-creating the virtual display ("sv << decide.recreations() << " of "sv << timing.max_recreations << ')';
          if (!proc::proc.recreate_virtual_display()) {
            BOOST_LOG(warning) << "vdisplay: re-creating the virtual display failed"sv;
          }
          break;
        case policy::action_e::back_off:
          BOOST_LOG(warning) << "vdisplay: the virtual display didn't come back; trying again in "sv
                             << std::chrono::duration_cast<std::chrono::seconds>(decide.back_off_for()).count() << " s"sv;
          break;
        case policy::action_e::give_up:
          BOOST_LOG(error) << "vdisplay: the virtual display didn't come back; leaving it for the rest of the stream"sv;
          break;
        case policy::action_e::none:
          break;
      }

      if (presence != VDISPLAY::PRESENCE::ACTIVE) {
        if (presence != VDISPLAY::PRESENCE::UNKNOWN) {
          follow.reset();
        }
        continue;
      }

      // On: as itself again, or renamed
      if (off_since || name != proc::proc.virtual_display_name()) {
        if (off_since) {
          BOOST_LOG(info) << "vdisplay: the virtual display is back as "sv << name << " after "sv << ms_since(*off_since, now) << " ms"sv;
        }
        proc::proc.virtual_display_back(name, off_since.has_value());
        off_since.reset();
      }

      // Capture stays on another display it fell back to until it restarts
      auto on = captured();
      bool should_follow = !on.empty() && proc::proc.stream_display() == name;
      switch (follow.step(now, should_follow, on == name)) {
        case policy::capture_e::restart:
          BOOST_LOG(info) << "vdisplay: capture is on "sv << on << ", not the virtual display "sv << name << "; restarting it ("sv << follow.restarts() << ')';
          mail::man->event<int>(mail::switch_display)->raise(-1);
          break;
        case policy::capture_e::confirmed:
          BOOST_LOG(info) << "vdisplay: capture is back on "sv << name;
          break;
        case policy::capture_e::stalled:
          BOOST_LOG(warning) << "vdisplay: capture stayed on "sv << on << " after restarting it; leaving it"sv;
          break;
        case policy::capture_e::waiting:
        case policy::capture_e::fine:
          break;
      }
    }
#endif
  }
}  // namespace fork_tweaks::vdisplay_recovery
