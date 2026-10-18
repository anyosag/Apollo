/**
 * @file src/fork/touchpad_win.cpp
 * @brief Trackpad passthrough on Windows; see touchpad_win.h.
 */
#ifdef _WIN32
  // The synthetic pointer API is Windows 10 and later, like in platform/windows/input.cpp
  #define WINVER 0x0A00

// standard includes
  #include <atomic>
  #include <chrono>
  #include <cstdint>

// platform includes
  #include <windows.h>

// local includes
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/misc.h"
  #include "touchpad.h"
  #include "touchpad_win.h"
  #include "touchpad_win_api.h"

using namespace std::literals;

// touchpad.h keeps Windows' values as plain numbers so its tests build anywhere
static_assert(POINTER_FLAG_INRANGE == fork_touchpad::flag_inrange);
static_assert(POINTER_FLAG_INCONTACT == fork_touchpad::flag_incontact);
static_assert(POINTER_FLAG_FIRSTBUTTON == fork_touchpad::flag_firstbutton);
static_assert(POINTER_FLAG_CONFIDENCE == fork_touchpad::flag_confidence);
static_assert(POINTER_FLAG_CANCELED == fork_touchpad::flag_canceled);
static_assert(POINTER_CHANGE_NONE == (int) fork_touchpad::button_change_e::none);
static_assert(POINTER_CHANGE_FIRSTBUTTON_DOWN == (int) fork_touchpad::button_change_e::down);
static_assert(POINTER_CHANGE_FIRSTBUTTON_UP == (int) fork_touchpad::button_change_e::up);

namespace fork_touchpad {
  namespace {
    /// How often the timer looks at a client that's holding fingers down
    constexpr auto tick_interval = 100ms;

    /// The least time between two warnings about failed injections
    constexpr std::int64_t failure_log_interval_us = 5'000'000;

    /**
     * The host's clock in microseconds, from the performance counter, and Windows' millisecond
     * time (GetTickCount) for a point on it. Both start when the clock is first used.
     */
    class host_clock_t {
    public:
      host_clock_t() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&base_counter);
        base_tick = GetTickCount64();
      }

      std::int64_t now_us() const {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        // Split the division so the product can't overflow, however long Apollo runs
        std::int64_t elapsed = counter.QuadPart - base_counter.QuadPart;
        std::int64_t seconds = elapsed / frequency.QuadPart;
        std::int64_t rest = elapsed % frequency.QuadPart;
        return seconds * 1'000'000 + rest * 1'000'000 / frequency.QuadPart;
      }

      DWORD tick_ms(std::int64_t time_us) const {
        return (DWORD) (base_tick + (ULONGLONG) (time_us / 1000));
      }

    private:
      LARGE_INTEGER frequency {};
      LARGE_INTEGER base_counter {};
      ULONGLONG base_tick = 0;
    };

    const host_clock_t &host_clock() {
      static const host_clock_t clock;
      return clock;
    }
  }  // namespace

  class session_t {
  public:
    ~session_t() {
      if (timer) {
        task_pool.cancel(timer);
      }
      if (device) {
        win::api().destroy(device);
        BOOST_LOG(info) << "fork: removed LuzLunar's synthetic touchpad"sv;
      }
    }

    host_t host;
    HSYNTHETICPOINTERDEVICE device = nullptr;
    bool unsupported = false;  ///< Windows wouldn't make the device, so this client's frames are dropped
    thread_pool_util::ThreadPool::task_id_t timer = nullptr;
    POINTER_TYPE_INFO contacts[luztouchpad::k_max_contacts] {};  // Not "info": BOOST_LOG(info) would find it
    HDESK last_desktop = nullptr;
    std::uint32_t logged_adopted = 0;
    std::int64_t last_failure_log_us = -failure_log_interval_us;
  };

  namespace {
    bool create_device(session_t &s, std::uint16_t width, std::uint16_t height) {
      auto params = win::touchpad_params(width, height);
      s.device = win::api().create(&params);
      if (!s.device) {
        auto err = GetLastError();
        BOOST_LOG(warning) << "fork: Windows wouldn't make a synthetic touchpad (error "sv << err << "), so LuzLunar's trackpad frames are ignored for this stream"sv;
        s.unsupported = true;
        return false;
      }
      s.host.set_device_size(width, height);
      BOOST_LOG(info) << "fork: made a "sv << width / 100.0 << " x "sv << height / 100.0 << " mm synthetic touchpad for LuzLunar"sv;
      return true;
    }

    /// Injects, following the input desktop (the lock screen, a UAC prompt) and retrying once if it moved
    bool inject(session_t &s, const injection_t &injection) {
      DWORD time = host_clock().tick_ms(injection.time_us);
      for (std::size_t i = 0; i < injection.count; i++) {
        const auto &e = injection.entries[i];
        s.contacts[i] = win::touchpad_contact(e.pointer_id, e.flags, (int) e.button_change, e.x, e.y, time);
      }

      auto &api = win::api();
      if (api.inject(s.device, s.contacts, (UINT32) injection.count)) {
        return true;
      }
      HDESK desktop = platf::syncThreadDesktop();
      if (desktop != s.last_desktop) {
        s.last_desktop = desktop;
        return api.inject(s.device, s.contacts, (UINT32) injection.count);
      }
      return false;
    }

    void deliver(session_t &s, const injection_t &injection, std::int64_t now_us) {
      if (injection.count == 0 || !s.device) {
        return;
      }
      if (!inject(s, injection)) {
        auto err = GetLastError();
        if (now_us - s.last_failure_log_us >= failure_log_interval_us) {
          BOOST_LOG(warning) << "fork: couldn't inject touchpad input (error "sv << err << ')';
          s.last_failure_log_us = now_us;
        }
      }
      s.host.injected(now_us);
    }

    void tick(std::weak_ptr<session_t> weak);

    /// Starts the timer while fingers are held
    void schedule(const std::shared_ptr<session_t> &session) {
      if (session->timer || !session->host.active()) {
        return;
      }
      session->timer = task_pool.pushDelayed(tick, tick_interval, std::weak_ptr<session_t>(session)).task_id;
    }

    void tick(std::weak_ptr<session_t> weak) {
      auto session = weak.lock();
      if (!session) {
        return;
      }
      auto &s = *session;
      s.timer = nullptr;

      auto now = host_clock().now_us();
      switch (s.host.tick(now)) {
        case tick_e::idle:
          return;
        case tick_e::wait:
          break;
        case tick_e::repeat:
          deliver(s, s.host.repeat(now), now);
          break;
        case tick_e::cancel:
          BOOST_LOG(info) << "fork: LuzLunar's trackpad went quiet with fingers down; lifting them"sv;
          deliver(s, s.host.cancel_all(now), now);
          break;
      }
      schedule(session);
    }
  }  // namespace

  bool available() {
    // 1 once a touchpad was made; -1 when the API is missing
    static std::atomic<int> state {0};
    static std::atomic<bool> logged_failure {false};

    int known = state.load();
    if (known != 0) {
      return known > 0;
    }

    auto &api = win::api();
    if (!api) {
      BOOST_LOG(info) << "fork: this Windows has no synthetic touchpads (CreateSyntheticPointerDevice2), so trackpad passthrough is off"sv;
      state = -1;
      return false;
    }

    auto params = win::touchpad_params(10000, 6500);
    HSYNTHETICPOINTERDEVICE device = api.create(&params);
    if (!device) {
      auto err = GetLastError();
      if (!logged_failure.exchange(true)) {
        BOOST_LOG(warning) << "fork: Windows wouldn't make a synthetic touchpad (error "sv << err << "), so trackpad passthrough isn't offered; trying again with the next stream"sv;
      }
      return false;
    }
    api.destroy(device);
    if (state.exchange(1) == 0) {
      BOOST_LOG(info) << "fork: synthetic touchpads work, so LuzLunar can send its trackpad"sv;
    }
    return true;
  }

  std::shared_ptr<session_t> make_session() {
    return std::make_shared<session_t>();
  }

  void frame(const std::shared_ptr<session_t> &session, const luztouchpad::frame &frame) {
    auto &s = *session;
    if (s.unsupported) {
      return;
    }

    bool wants_device = frame.count > 0 && !(frame.flags & luztouchpad::flag_cancel_all);
    if (!s.device) {
      // Nothing to lift on a device that isn't there yet
      if (!wants_device || !create_device(s, frame.width, frame.height)) {
        return;
      }
    } else if (wants_device && s.host.needs_recreate(frame.width, frame.height)) {
      win::api().destroy(s.device);
      s.device = nullptr;
      BOOST_LOG(info) << "fork: LuzLunar's trackpad changed size; making the touchpad again"sv;
      if (!create_device(s, frame.width, frame.height)) {
        return;
      }
    }

    auto now = host_clock().now_us();
    deliver(s, s.host.apply(frame, now), now);
    if (s.host.adopted() != s.logged_adopted) {
      BOOST_LOG(debug) << "fork: a trackpad frame moved a finger the host never saw go down (a lost frame?)"sv;
      s.logged_adopted = s.host.adopted();
    }
    schedule(session);
  }

  void cancel(const std::shared_ptr<session_t> &session) {
    auto now = host_clock().now_us();
    deliver(*session, session->host.cancel_all(now), now);
  }
}  // namespace fork_touchpad

#endif
