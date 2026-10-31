/**
 * @file src/fork/camera_policy.h
 * @brief The Mac's camera as a Windows webcam, apart from any I/O: what Apollo tells the Mac and
 * when, the shared memory the camera's media source reads its frames from, and the NV12 drawing
 * both sides need.
 *
 * Apollo registers "Mac Camera", a Windows 11 virtual camera whose media source is
 * tools/apollo-camera-source (a DLL Windows' Frame Server loads). When an app opens the camera, the
 * source says so, with the size the app chose, in the control block; Apollo tells LuzLunar to
 * start its camera (START, luzcamera.h), decodes what comes back and puts each frame in the frames
 * section, where the source takes the newest. When the last app closes the camera, Apollo waits
 * stop_delay before telling the Mac to stop, so an app that reopens it at once (many do, to
 * switch formats) doesn't turn the Mac's camera off and on. Without a Mac, the source shows a
 * placeholder Apollo draws, or its own icon while Apollo isn't there at all.
 *
 * Shared memory: two named sections and two events in the Global namespace, made by Apollo.
 * frames (frames_header_t, then k_slot_count slots of slot_header_t and an NV12 frame): Apollo
 * writes, the source only reads. control (control_t): the source writes what the apps want, and
 * Apollo reads it. A slot is written under a sequence lock (odd while it's being written), and the
 * header's latest points at the newest whole one, so the source never waits for Apollo.
 *
 * Header-only, without Boost or Windows, so tests (and the camera's DLL) can build it on their own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

// local includes
#include "luzcamera.h"

namespace camera_policy {
  using clock = std::chrono::steady_clock;

  /// The camera's name in Windows
  inline constexpr const wchar_t *k_friendly_name = L"Mac Camera";

  /// The media source's COM class, {5C0E6C3D-1E7A-4B8F-9A52-4C55A8F0C1A1}
  inline constexpr const wchar_t *k_source_clsid = L"{5C0E6C3D-1E7A-4B8F-9A52-4C55A8F0C1A1}";

  /// How long after the last app closes the camera the Mac is told to stop
  constexpr auto stop_delay = std::chrono::seconds(4);

  /// A control block whose source hasn't written for this long belongs to a source that's gone
  constexpr std::uint64_t heartbeat_timeout_ms = 3000;

  /// How often the source writes its heartbeat while it streams
  constexpr std::uint64_t heartbeat_interval_ms = 1000;

  /// Keyframe requests while the decoder waits for one: at most one this often
  constexpr auto keyframe_retry = std::chrono::milliseconds(1000);

  /// Frames waiting for the decoder; more and they're dropped until a keyframe
  constexpr std::size_t max_queued_frames = 8;

  /// The sizes the camera offers, at k_fps
  struct frame_size_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const frame_size_t &) const = default;
  };

  inline constexpr frame_size_t k_sizes[] = {{1280, 720}, {1920, 1080}};
  constexpr std::uint32_t k_fps = 30;
  constexpr std::uint32_t k_max_width = 1920;
  constexpr std::uint32_t k_max_height = 1080;

  /// The offered size nearest to a requested one (by pixel count)
  inline frame_size_t supported_size(std::uint32_t width, std::uint32_t height) {
    std::uint64_t pixels = (std::uint64_t) width * height;
    frame_size_t best = k_sizes[0];
    std::uint64_t best_distance = UINT64_MAX;
    for (const auto &size : k_sizes) {
      std::uint64_t p = (std::uint64_t) size.width * size.height;
      std::uint64_t distance = p > pixels ? p - pixels : pixels - p;
      if (distance < best_distance) {
        best = size;
        best_distance = distance;
      }
    }
    return best;
  }

  /// NV12's size, tightly packed: a luma byte per pixel, then a half-height plane of U,V pairs
  constexpr std::size_t nv12_bytes(std::uint32_t width, std::uint32_t height) {
    return (std::size_t) width * height + (std::size_t) width * ((height + 1) / 2);
  }

  /**
   * @brief What Apollo tells the Mac, from what the camera's apps want.
   *
   * update() takes what the control block says now and whether a Mac's camera connection is open,
   * and says what to send. A connection that opens starts with nothing sent: the camera is off
   * until START. Apps going away leave the camera on for stop_delay; apps coming back within it, at
   * the same size, change nothing.
   */
  class start_stop_t {
  public:
    enum class action_e {
      none,
      start,  ///< send START with start()
      stop,  ///< send STOP
    };

    action_e update(bool apps_streaming, frame_size_t size, bool connected, clock::time_point now) {
      if (!connected) {
        m_Sent = sent_e::nothing;
        m_StopAt.reset();
        return action_e::none;
      }
      if (apps_streaming) {
        m_StopAt.reset();
        if (m_Sent != sent_e::started || !(m_Size == size)) {
          m_Sent = sent_e::started;
          m_Size = size;
          return action_e::start;
        }
        return action_e::none;
      }
      if (m_Sent != sent_e::started) {
        return action_e::none;
      }
      if (!m_StopAt) {
        m_StopAt = now + stop_delay;
        return action_e::none;
      }
      if (now >= *m_StopAt) {
        m_StopAt.reset();
        m_Sent = sent_e::stopped;
        return action_e::stop;
      }
      return action_e::none;
    }

    /// When the pending stop is due, for the camera thread's wait; none without one
    std::optional<clock::time_point> deadline() const {
      return m_StopAt;
    }

    /// The Mac was told to start, and not yet to stop
    bool started() const {
      return m_Sent == sent_e::started;
    }

    /// START for the size update() last started
    luzcamera::start_t start() const {
      luzcamera::start_t start;
      start.width = (std::uint16_t) m_Size.width;
      start.height = (std::uint16_t) m_Size.height;
      start.fps = (std::uint16_t) k_fps;
      start.kbps = luzcamera::default_kbps(m_Size.width, m_Size.height);
      return start;
    }

    frame_size_t size() const {
      return m_Size;
    }

  private:
    enum class sent_e {
      nothing,
      started,
      stopped,
    };

    sent_e m_Sent = sent_e::nothing;
    frame_size_t m_Size;
    std::optional<clock::time_point> m_StopAt;
  };

  /**
   * @brief Which received frames go to the decoder. After a reset (a new connection, START, a
   * decoder error) and whenever frames had to be dropped, only a keyframe with its SPS and PPS can
   * start decoding again; until one comes, a keyframe is asked for at most every keyframe_retry.
   */
  class frame_gate_t {
  public:
    enum class verdict_e {
      decode,
      drop,
      drop_and_ask,  ///< drop it, and send KEYFRAME
    };

    /// Decoding starts over: wait for a keyframe. A request is due at once if ask is set.
    void reset(clock::time_point now, bool ask) {
      m_Waiting = true;
      m_LastAsk = ask ? std::optional<clock::time_point>() : std::optional<clock::time_point>(now);
    }

    verdict_e frame(bool decodable_keyframe, frame_size_t size, clock::time_point now) {
      if (!(size == m_Size)) {
        // A new size starts with a keyframe of its own
        m_Size = size;
        m_Waiting = true;
      }
      if (m_Waiting && decodable_keyframe) {
        m_Waiting = false;
      }
      if (!m_Waiting) {
        return verdict_e::decode;
      }
      if (!m_LastAsk || now - *m_LastAsk >= keyframe_retry) {
        m_LastAsk = now;
        return verdict_e::drop_and_ask;
      }
      return verdict_e::drop;
    }

    bool waiting() const {
      return m_Waiting;
    }

  private:
    bool m_Waiting = true;
    frame_size_t m_Size;
    std::optional<clock::time_point> m_LastAsk;
  };

  /// What the placeholder says, from what Apollo knows of the Mac
  enum class placeholder_e {
    no_stream,  ///< nobody streams with LuzLunar's camera on
    connecting,  ///< a LuzLunar stream brings its camera, but it hasn't connected yet
    starting,  ///< connected and asked to start; no frame yet
    denied,  ///< macOS doesn't let LuzLunar use the camera
    unavailable,  ///< the Mac has no camera it can use
    off,  ///< turned off in LuzLunar's settings
  };

  /// The placeholder's text, in UTF-8; name is the Mac's
  inline std::string placeholder_text(placeholder_e state, const std::string &name, const std::string &detail = {}) {
    std::string mac = name.empty() ? std::string("the Mac") : name;
    switch (state) {
      case placeholder_e::no_stream:
        return "Start a LuzLunar stream to use your Mac's camera";
      case placeholder_e::connecting:
        return "Connecting to the camera of " + mac + "\xE2\x80\xA6";
      case placeholder_e::starting:
        return "Starting the camera of " + mac + "\xE2\x80\xA6";
      case placeholder_e::denied:
        return "Allow LuzLunar to use the camera on " + mac + ": System Settings \xE2\x80\xBA Privacy & Security \xE2\x80\xBA Camera";
      case placeholder_e::unavailable:
        return "The camera of " + mac + " isn't available" + (detail.empty() ? std::string() : ": " + detail);
      case placeholder_e::off:
        return "The camera of " + mac + " is off in LuzLunar's settings";
    }
    return {};
  }

  /// The placeholder for what a client's STATUS said; none for live
  inline std::optional<placeholder_e> placeholder_for(luzcamera::status_e status) {
    switch (status) {
      case luzcamera::status_e::live:
        return std::nullopt;
      case luzcamera::status_e::stopped:
        return placeholder_e::starting;
      case luzcamera::status_e::denied:
        return placeholder_e::denied;
      case luzcamera::status_e::unavailable:
        return placeholder_e::unavailable;
      case luzcamera::status_e::off:
        return placeholder_e::off;
    }
    return placeholder_e::unavailable;
  }

  namespace shared {
    /// The Global objects; v1 is the layout below, so an older source never reads a newer layout
    inline constexpr const wchar_t *k_frames_name = L"Global\\LuzCamera.v1.Frames";
    inline constexpr const wchar_t *k_control_name = L"Global\\LuzCamera.v1.Control";
    inline constexpr const wchar_t *k_frame_event_name = L"Global\\LuzCamera.v1.FrameReady";
    inline constexpr const wchar_t *k_state_event_name = L"Global\\LuzCamera.v1.StateChanged";

    /// Who may use them: SYSTEM (Apollo, Frame Server Monitor) everything; LOCAL SERVICE (Frame
    /// Server) reads frames, writes the control block, waits for frames and signals a state change.
    /// Write-restricted services get the same through WR, should Frame Server be one. SYSTEM owns
    /// them, so Apollo can tell its own objects (kept by Frame Server across a restart) from
    /// someone else's.
    inline constexpr const wchar_t *k_frames_sddl = L"O:SYD:P(A;;GA;;;SY)(A;;GR;;;LS)";
    inline constexpr const wchar_t *k_control_sddl = L"O:SYD:P(A;;GA;;;SY)(A;;GRGW;;;LS)(A;;GRGW;;;WR)";
    inline constexpr const wchar_t *k_frame_event_sddl = L"O:SYD:P(A;;GA;;;SY)(A;;0x00100000;;;LS)";
    inline constexpr const wchar_t *k_state_event_sddl = L"O:SYD:P(A;;GA;;;SY)(A;;0x00100002;;;LS)(A;;0x00100002;;;WR)";

    constexpr std::uint32_t k_magic = 0x4D41434Cu;  // "LCAM"
    constexpr std::uint32_t k_layout_version = 1;
    constexpr std::uint32_t k_slot_count = 3;
    constexpr std::uint32_t k_no_slot = 0xFFFFFFFFu;

    /// What a slot shows
    enum class kind_e : std::uint32_t {
      camera = 1,  ///< a frame from the Mac
      placeholder = 2,  ///< Apollo's placeholder
    };

    struct alignas(64) frames_header_t {
      std::uint32_t magic;
      std::uint32_t version;
      std::uint32_t slot_count;
      std::uint32_t slot_stride;  ///< bytes from one slot to the next
      std::uint32_t data_offset;  ///< where slot 0 starts
      std::uint32_t latest;  ///< the newest whole slot, or k_no_slot; atomic
      std::uint64_t published;  ///< frames published so far; atomic
      std::uint64_t heartbeat_ms;  ///< Apollo's GetTickCount64() every second while it runs; atomic
    };

    struct alignas(64) slot_header_t {
      std::uint32_t seq;  ///< odd while the slot is written; atomic
      std::uint32_t width;
      std::uint32_t height;
      std::uint32_t kind;  ///< kind_e
      std::uint64_t number;  ///< the frame's number among those published
      std::uint64_t time_us;  ///< when Apollo had it, on its steady clock
    };

    struct alignas(64) control_t {
      std::uint32_t magic;
      std::uint32_t version;
      std::uint32_t seq;  ///< bumped by the source after each change; atomic
      std::uint32_t streaming;  ///< 1 while the camera's apps stream; atomic
      std::uint32_t width;
      std::uint32_t height;
      std::uint32_t fps;
      std::uint32_t pid;  ///< the source's process
      std::uint64_t heartbeat_ms;  ///< the source's GetTickCount64() while it streams; atomic
    };

    constexpr std::size_t align_up(std::size_t value, std::size_t to) {
      return (value + to - 1) / to * to;
    }

    constexpr std::size_t k_data_offset = align_up(sizeof(frames_header_t), 4096);
    constexpr std::size_t k_slot_stride = align_up(sizeof(slot_header_t) + nv12_bytes(k_max_width, k_max_height), 4096);
    constexpr std::size_t k_frames_bytes = k_data_offset + k_slot_count * k_slot_stride;
    constexpr std::size_t k_control_bytes = 4096;

    static_assert(sizeof(control_t) <= k_control_bytes);
    static_assert(offsetof(frames_header_t, published) % 8 == 0);
    static_assert(offsetof(frames_header_t, heartbeat_ms) % 8 == 0);
    static_assert(offsetof(control_t, heartbeat_ms) % 8 == 0);

    template<typename T>
    std::atomic_ref<T> atomic(T &value) {
      return std::atomic_ref<T>(value);
    }

    inline frames_header_t *frames_header(std::uint8_t *base) {
      return reinterpret_cast<frames_header_t *>(base);
    }

    inline slot_header_t *slot_header(std::uint8_t *base, std::uint32_t slot) {
      return reinterpret_cast<slot_header_t *>(base + k_data_offset + slot * k_slot_stride);
    }

    inline std::uint8_t *slot_pixels(std::uint8_t *base, std::uint32_t slot) {
      return base + k_data_offset + slot * k_slot_stride + sizeof(slot_header_t);
    }

    /// Apollo, on a new frames section (k_frames_bytes, zeroed)
    inline void init_frames(std::uint8_t *base) {
      frames_header_t *header = frames_header(base);
      header->magic = k_magic;
      header->version = k_layout_version;
      header->slot_count = k_slot_count;
      header->slot_stride = (std::uint32_t) k_slot_stride;
      header->data_offset = (std::uint32_t) k_data_offset;
      atomic(header->published).store(0, std::memory_order_relaxed);
      atomic(header->latest).store(k_no_slot, std::memory_order_release);
    }

    /// Apollo, on a new control block (k_control_bytes, zeroed)
    inline void init_control(std::uint8_t *base) {
      control_t *control = reinterpret_cast<control_t *>(base);
      control->version = k_layout_version;
      atomic(control->magic).store(k_magic, std::memory_order_release);
    }

    /// Whether a mapped frames section has the layout this code knows
    inline bool frames_valid(const std::uint8_t *base) {
      const frames_header_t *header = reinterpret_cast<const frames_header_t *>(base);
      return header->magic == k_magic && header->version == k_layout_version && header->slot_count == k_slot_count &&
             header->slot_stride == k_slot_stride && header->data_offset == k_data_offset;
    }

    /**
     * @brief Apollo: puts a tightly packed NV12 frame in the slot after the newest one, and makes
     * it the newest. False for a size the slots can't hold.
     */
    inline bool publish(std::uint8_t *base, std::uint32_t width, std::uint32_t height, kind_e kind, std::uint64_t time_us, const std::uint8_t *nv12) {
      if (width == 0 || height == 0 || width > k_max_width || height > k_max_height || (width & 1) || (height & 1)) {
        return false;
      }
      frames_header_t *header = frames_header(base);
      std::uint32_t latest = atomic(header->latest).load(std::memory_order_relaxed);
      std::uint32_t slot = latest >= k_slot_count ? 0 : (latest + 1) % k_slot_count;
      slot_header_t *h = slot_header(base, slot);

      std::uint32_t seq = atomic(h->seq).load(std::memory_order_relaxed);
      atomic(h->seq).store(seq | 1u, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      h->width = width;
      h->height = height;
      h->kind = (std::uint32_t) kind;
      h->number = atomic(header->published).load(std::memory_order_relaxed) + 1;
      h->time_us = time_us;
      std::memcpy(slot_pixels(base, slot), nv12, nv12_bytes(width, height));
      atomic(h->seq).store((seq | 1u) + 1u, std::memory_order_release);

      atomic(header->latest).store(slot, std::memory_order_release);
      atomic(header->published).fetch_add(1, std::memory_order_release);
      return true;
    }

    /// The newest frame, as read
    struct frame_view_t {
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      kind_e kind = kind_e::placeholder;
      std::uint64_t number = 0;
      const std::uint8_t *nv12 = nullptr;  ///< in the section: copy it in the callback
    };

    /**
     * @brief The source: calls copy(view) with the newest whole frame, and returns whether the
     * copy is good (false too when there's none yet, or Apollo wrote over it meanwhile).
     */
    template<typename F>
    bool read_latest(std::uint8_t *base, F &&copy) {
      frames_header_t *header = frames_header(base);
      std::uint32_t slot = atomic(header->latest).load(std::memory_order_acquire);
      if (slot >= k_slot_count) {
        return false;
      }
      slot_header_t *h = slot_header(base, slot);
      std::uint32_t before = atomic(h->seq).load(std::memory_order_acquire);
      if (before & 1u) {
        return false;
      }
      frame_view_t view;
      view.width = h->width;
      view.height = h->height;
      view.kind = (kind_e) h->kind;
      view.number = h->number;
      view.nv12 = slot_pixels(base, slot);
      if (view.width == 0 || view.height == 0 || view.width > k_max_width || view.height > k_max_height) {
        return false;
      }
      copy(view);
      std::atomic_thread_fence(std::memory_order_acquire);
      std::uint32_t after = atomic(h->seq).load(std::memory_order_relaxed);
      return before == after;
    }

    /// The number of the newest frame (0 for none), to tell whether there's a new one
    inline std::uint64_t published(std::uint8_t *base) {
      return atomic(frames_header(base)->published).load(std::memory_order_acquire);
    }

    /// Apollo: still there, so its frames and placeholder are current
    inline void beat_frames(std::uint8_t *base, std::uint64_t now_ms) {
      atomic(frames_header(base)->heartbeat_ms).store(now_ms, std::memory_order_release);
    }

    /**
     * @brief The source: whether Apollo still writes the frames section. One that stopped beating
     * for heartbeat_timeout_ms is gone (the service stopped, or crashed) and its last frame stale,
     * so the source shows its own icon.
     */
    inline bool frames_alive(std::uint8_t *base, std::uint64_t now_ms) {
      std::uint64_t beat = atomic(frames_header(base)->heartbeat_ms).load(std::memory_order_acquire);
      return beat != 0 && beat <= now_ms + heartbeat_timeout_ms && now_ms <= beat + heartbeat_timeout_ms;
    }

    /// What the source last wrote in the control block
    struct apps_t {
      bool streaming = false;
      frame_size_t size;
      std::uint32_t fps = 0;
      std::uint32_t pid = 0;
    };

    /// The source: what the camera's apps want now, and the heartbeat that says it's still there
    inline void write_control(std::uint8_t *base, const apps_t &apps, std::uint64_t now_ms) {
      control_t *control = reinterpret_cast<control_t *>(base);
      control->width = apps.size.width;
      control->height = apps.size.height;
      control->fps = apps.fps;
      control->pid = apps.pid;
      atomic(control->heartbeat_ms).store(now_ms, std::memory_order_relaxed);
      atomic(control->streaming).store(apps.streaming ? 1u : 0u, std::memory_order_release);
      atomic(control->seq).fetch_add(1, std::memory_order_release);
    }

    /// The source: still streaming
    inline void beat(std::uint8_t *base, std::uint64_t now_ms) {
      control_t *control = reinterpret_cast<control_t *>(base);
      atomic(control->heartbeat_ms).store(now_ms, std::memory_order_release);
    }

    /**
     * @brief Apollo: what the apps want. A streaming source that stopped beating for
     * heartbeat_timeout_ms is gone (Frame Server ended, or crashed), so nothing streams.
     */
    inline apps_t read_control(std::uint8_t *base, std::uint64_t now_ms) {
      control_t *control = reinterpret_cast<control_t *>(base);
      apps_t apps;
      if (atomic(control->magic).load(std::memory_order_acquire) != k_magic) {
        return apps;
      }
      apps.streaming = atomic(control->streaming).load(std::memory_order_acquire) == 1u;
      apps.size = {control->width, control->height};
      apps.fps = control->fps;
      apps.pid = control->pid;
      std::uint64_t beat = atomic(control->heartbeat_ms).load(std::memory_order_acquire);
      if (apps.streaming && (beat > now_ms + heartbeat_timeout_ms || now_ms > beat + heartbeat_timeout_ms)) {
        apps.streaming = false;
      }
      if (apps.streaming) {
        apps.size = supported_size(apps.size.width, apps.size.height);
      }
      return apps;
    }
  }  // namespace shared

  namespace nv12 {
    /// Video-range BT.709 for a color given as 8-bit sRGB-ish RGB; what the camera's media types say
    struct yuv_t {
      std::uint8_t y;
      std::uint8_t u;
      std::uint8_t v;
    };

    constexpr yuv_t from_rgb(int r, int g, int b) {
      // Rec. 709 limited range, in 1/1024ths
      int y = 16 + ((187 * r + 629 * g + 63 * b + 512) >> 10);
      int u = 128 + ((-103 * r - 346 * g + 450 * b + 512) >> 10);
      int v = 128 + ((450 * r - 409 * g - 41 * b + 512) >> 10);
      auto clamp = [](int value) {
        return (std::uint8_t) (value < 0 ? 0 : value > 255 ? 255 : value);
      };
      return {clamp(y), clamp(u), clamp(v)};
    }

    /// Fills a frame with one color; pitch is the bytes per row of either plane, and the UV
    /// plane starts pitch * height bytes after the Y plane
    inline void fill(std::uint8_t *dst, std::uint32_t width, std::uint32_t height, std::size_t pitch, yuv_t color) {
      for (std::uint32_t row = 0; row < height; row++) {
        std::memset(dst + row * pitch, color.y, width);
      }
      std::uint8_t *uv = dst + pitch * height;
      for (std::uint32_t row = 0; row < (height + 1) / 2; row++) {
        std::uint8_t *line = uv + row * pitch;
        for (std::uint32_t x = 0; x + 1 < width; x += 2) {
          line[x] = color.u;
          line[x + 1] = color.v;
        }
      }
    }

    /**
     * @brief Copies a tightly packed NV12 frame into one with a pitch, scaling it (nearest pixel)
     * when the sizes differ, which only happens for a frame or two while the size switches.
     */
    inline void copy_scaled(const std::uint8_t *src, std::uint32_t src_width, std::uint32_t src_height, std::uint8_t *dst, std::uint32_t width, std::uint32_t height, std::size_t pitch) {
      const std::uint8_t *src_uv = src + (std::size_t) src_width * src_height;
      std::uint8_t *uv = dst + pitch * height;
      if (src_width == width && src_height == height) {
        for (std::uint32_t row = 0; row < height; row++) {
          std::memcpy(dst + row * pitch, src + (std::size_t) row * width, width);
        }
        for (std::uint32_t row = 0; row < (height + 1) / 2; row++) {
          std::memcpy(uv + row * pitch, src_uv + (std::size_t) row * width, width);
        }
        return;
      }
      for (std::uint32_t row = 0; row < height; row++) {
        std::uint32_t sy = (std::uint32_t) ((std::uint64_t) row * src_height / height);
        const std::uint8_t *in = src + (std::size_t) sy * src_width;
        std::uint8_t *out = dst + row * pitch;
        for (std::uint32_t x = 0; x < width; x++) {
          out[x] = in[(std::uint64_t) x * src_width / width];
        }
      }
      std::uint32_t src_chroma_width = src_width / 2;
      std::uint32_t src_chroma_height = (src_height + 1) / 2;
      std::uint32_t chroma_width = width / 2;
      std::uint32_t chroma_height = (height + 1) / 2;
      for (std::uint32_t row = 0; row < chroma_height; row++) {
        std::uint32_t sy = (std::uint32_t) ((std::uint64_t) row * src_chroma_height / chroma_height);
        const std::uint8_t *in = src_uv + (std::size_t) sy * src_width;
        std::uint8_t *out = uv + row * pitch;
        for (std::uint32_t x = 0; x < chroma_width; x++) {
          std::uint32_t sx = (std::uint32_t) ((std::uint64_t) x * src_chroma_width / chroma_width);
          out[2 * x] = in[2 * sx];
          out[2 * x + 1] = in[2 * sx + 1];
        }
      }
    }

    /**
     * @brief Copies the visible part of a decoded frame (the decoder's rows may be taller, e.g.
     * 1088 for 1080) into a tightly packed one. The source's UV plane starts at uv_offset.
     */
    inline void crop(const std::uint8_t *src, std::size_t src_pitch, std::size_t uv_offset, std::uint32_t width, std::uint32_t height, std::uint8_t *dst) {
      for (std::uint32_t row = 0; row < height; row++) {
        std::memcpy(dst + (std::size_t) row * width, src + row * src_pitch, width);
      }
      std::uint8_t *uv = dst + (std::size_t) width * height;
      for (std::uint32_t row = 0; row < (height + 1) / 2; row++) {
        std::memcpy(uv + (std::size_t) row * width, src + uv_offset + row * src_pitch, width);
      }
    }

    /// 32-bit BGRA (as GDI draws it) to tightly packed NV12, video-range BT.709
    inline void from_bgra(const std::uint8_t *bgra, std::size_t stride, std::uint32_t width, std::uint32_t height, std::uint8_t *dst) {
      std::uint8_t *uv = dst + (std::size_t) width * height;
      for (std::uint32_t row = 0; row < height; row++) {
        const std::uint8_t *in = bgra + row * stride;
        std::uint8_t *out = dst + (std::size_t) row * width;
        for (std::uint32_t x = 0; x < width; x++) {
          out[x] = from_rgb(in[4 * x + 2], in[4 * x + 1], in[4 * x]).y;
        }
      }
      for (std::uint32_t row = 0; row < height / 2; row++) {
        const std::uint8_t *a = bgra + (2 * row) * stride;
        const std::uint8_t *b = bgra + (2 * row + 1) * stride;
        std::uint8_t *out = uv + (std::size_t) row * width;
        for (std::uint32_t x = 0; x + 1 < width; x += 2) {
          int r = (a[4 * x + 2] + a[4 * x + 6] + b[4 * x + 2] + b[4 * x + 6] + 2) / 4;
          int g = (a[4 * x + 1] + a[4 * x + 5] + b[4 * x + 1] + b[4 * x + 5] + 2) / 4;
          int bl = (a[4 * x] + a[4 * x + 4] + b[4 * x] + b[4 * x + 4] + 2) / 4;
          yuv_t c = from_rgb(r, g, bl);
          out[x] = c.u;
          out[x + 1] = c.v;
        }
      }
    }

    /// The placeholder's colors
    inline constexpr yuv_t k_background = from_rgb(28, 28, 30);
    inline constexpr yuv_t k_icon = from_rgb(142, 142, 147);

    /**
     * @brief A camera icon in the middle of the frame, a little above center, sized to it: a
     * rounded body with a lens, and a viewfinder bump. Drawn over what's there; the frame has a
     * pitch, its UV plane pitch * height bytes after the Y plane.
     */
    inline void draw_camera_icon(std::uint8_t *dst, std::uint32_t width, std::uint32_t height, std::size_t pitch, yuv_t color = k_icon, yuv_t background = k_background) {
      // Body: a quarter of the height tall, 1.4 times as wide
      double body_h = height * 0.22;
      double body_w = body_h * 1.45;
      double cx = width / 2.0;
      double cy = height * 0.40;
      double left = cx - body_w / 2;
      double top = cy - body_h / 2;
      double radius = body_h * 0.18;
      double lens_outer = body_h * 0.34;
      double lens_inner = body_h * 0.22;
      double bump_w = body_w * 0.30;
      double bump_h = body_h * 0.14;

      // At a pixel's center: 1 the icon, 0 the background (the ring around the lens), -1 outside it
      auto paint = [&](double x, double y) -> int {
        double dx = x - cx;
        double dy = y - cy;
        double d2 = dx * dx + dy * dy;
        if (d2 <= lens_inner * lens_inner) {
          return 1;  // the lens's glass
        }
        if (d2 <= lens_outer * lens_outer) {
          return 0;  // the ring around it
        }
        // The viewfinder bump on top
        if (y >= top - bump_h && y < top && x >= cx - bump_w / 2 && x <= cx + bump_w / 2) {
          return 1;
        }
        if (x < left || x > left + body_w || y < top || y > top + body_h) {
          return -1;
        }
        // Rounded corners
        double ix = std::clamp(x, left + radius, left + body_w - radius);
        double iy = std::clamp(y, top + radius, top + body_h - radius);
        double rx = x - ix;
        double ry = y - iy;
        return rx * rx + ry * ry <= radius * radius ? 1 : -1;
      };

      std::uint32_t x0 = (std::uint32_t) std::max(0.0, left - 2);
      std::uint32_t x1 = (std::uint32_t) std::min((double) width, left + body_w + 2);
      std::uint32_t y0 = (std::uint32_t) std::max(0.0, top - bump_h - 2);
      std::uint32_t y1 = (std::uint32_t) std::min((double) height, top + body_h + 2);
      for (std::uint32_t y = y0; y < y1; y++) {
        std::uint8_t *row = dst + y * pitch;
        for (std::uint32_t x = x0; x < x1; x++) {
          int p = paint(x + 0.5, y + 0.5);
          if (p >= 0) {
            row[x] = p ? color.y : background.y;
          }
        }
      }
      std::uint8_t *uv = dst + pitch * height;
      for (std::uint32_t y = y0 / 2; y < (y1 + 1) / 2 && y < (height + 1) / 2; y++) {
        std::uint8_t *row = uv + y * pitch;
        for (std::uint32_t x = x0 / 2; x < (x1 + 1) / 2 && 2 * x + 1 < width; x++) {
          int p = paint(2 * x + 1.0, 2 * y + 1.0);
          if (p >= 0) {
            row[2 * x] = p ? color.u : background.u;
            row[2 * x + 1] = p ? color.v : background.v;
          }
        }
      }
    }
  }  // namespace nv12
}  // namespace camera_policy
