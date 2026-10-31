/**
 * @file src/fork/mic.cpp
 * @brief Fork-only: the Mac's microphone on the PC, through the Steam Streaming Microphone (mic.h).
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include "fork_config.h"
#include "luzmic.h"
#include "mic.h"
#include "mic_policy.h"
#include "src/config.h"
#include "src/logging.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <audioclient.h>
  #include <audiopolicy.h>
  #include <avrt.h>
  #include <mmdeviceapi.h>
  #include <newdev.h>
  #include <propidl.h>

  #include <opus/opus.h>

  // Must be the last included file (it redefines DEFINE_GUID)
  // clang-format off
  #include "src/platform/windows/PolicyConfig.h"
  // clang-format on
#endif

using namespace std::literals;

namespace fork_mic {

#ifdef _WIN32
  namespace {
    constexpr auto k_adapter_name = L"Steam Streaming Microphone";
    constexpr auto k_driver_path = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\x64\\SteamStreamingMicrophone.inf";
    constexpr REFERENCE_TIME k_buffer_duration = 400000;  // 40 ms, in 100 ns
    constexpr ERole k_roles[] = {eConsole, eMultimedia, eCommunications};

    std::string hex(HRESULT value) {
      char text[16];
      std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(value));
      return text;
    }

    /// PKEY_DeviceInterface_FriendlyName: the adapter's name, as audio.cpp matches Steam Streaming Speakers
    const PROPERTYKEY k_interface_friendly_name = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

    template<class T>
    class com_t {
    public:
      com_t() = default;

      ~com_t() {
        reset();
      }

      com_t(const com_t &) = delete;
      com_t &operator=(const com_t &) = delete;

      com_t(com_t &&other) noexcept:
          m_p {std::exchange(other.m_p, nullptr)} {
      }

      com_t &operator=(com_t &&other) noexcept {
        if (this != &other) {
          reset();
          m_p = std::exchange(other.m_p, nullptr);
        }
        return *this;
      }

      void reset() {
        if (m_p) {
          m_p->Release();
          m_p = nullptr;
        }
      }

      T **put() {
        reset();
        return &m_p;
      }

      void **put_void() {
        return reinterpret_cast<void **>(put());
      }

      T *get() const {
        return m_p;
      }

      T *operator->() const {
        return m_p;
      }

      explicit operator bool() const {
        return m_p != nullptr;
      }

    private:
      T *m_p = nullptr;
    };

    std::wstring device_id(IMMDevice *device) {
      LPWSTR id = nullptr;
      if (!device || FAILED(device->GetId(&id)) || !id) {
        return {};
      }
      std::wstring result {id};
      CoTaskMemFree(id);
      return result;
    }

    std::wstring default_id(IMMDeviceEnumerator *devices, EDataFlow flow, ERole role) {
      com_t<IMMDevice> device;
      if (FAILED(devices->GetDefaultAudioEndpoint(flow, role, device.put()))) {
        return {};
      }
      return device_id(device.get());
    }

    /// The active endpoint of the Steam Streaming Microphone for flow, if Windows has it
    com_t<IMMDevice> find_endpoint(IMMDeviceEnumerator *devices, EDataFlow flow) {
      com_t<IMMDeviceCollection> collection;
      if (FAILED(devices->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()))) {
        return {};
      }
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        com_t<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) {
          continue;
        }
        com_t<IPropertyStore> properties;
        if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
          continue;
        }
        PROPVARIANT name;
        PropVariantInit(&name);
        const bool match = SUCCEEDED(properties->GetValue(k_interface_friendly_name, &name)) && name.vt == VT_LPWSTR && name.pwszVal &&
                           std::wcscmp(name.pwszVal, k_adapter_name) == 0;
        PropVariantClear(&name);
        if (match) {
          return device;
        }
      }
      return {};
    }

    com_t<IMMDeviceEnumerator> make_enumerator() {
      com_t<IMMDeviceEnumerator> devices;
      if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), devices.put_void()))) {
        BOOST_LOG(warning) << "fork: microphone: no audio device enumerator"sv;
      }
      return devices;
    }

    com_t<IPolicyConfig> make_policy() {
      com_t<IPolicyConfig> policy;
      if (FAILED(CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, policy.put_void()))) {
        BOOST_LOG(warning) << "fork: microphone: no audio policy config, so the default recording device stays as it is"sv;
      }
      return policy;
    }

    struct defaults_t {
      std::wstring render[3];
      std::wstring capture[3];
    };

    defaults_t save_defaults(IMMDeviceEnumerator *devices) {
      defaults_t saved;
      for (int i = 0; i < 3; ++i) {
        saved.render[i] = default_id(devices, eRender, k_roles[i]);
        saved.capture[i] = default_id(devices, eCapture, k_roles[i]);
      }
      return saved;
    }

    /// Installing Steam's microphone driver, once per run of Apollo, on a thread of its own: it can
    /// take longer than a stream's end may wait
    enum class install_e {
      not_tried,
      running,
      done,
    };

    std::atomic<install_e> g_install {install_e::not_tried};

    void install_driver() {
      const auto co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      auto done = [&]() {
        if (SUCCEEDED(co)) {
          CoUninitialize();
        }
        g_install = install_e::done;
      };

      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        BOOST_LOG(warning) << "fork: microphone: newdev.dll failed to load"sv;
        done();
        return;
      }
      // MinGW's libnewdev.a lacks DiInstallDriverW(), as audio.cpp says
      auto fn_install = reinterpret_cast<decltype(DiInstallDriverW) *>(reinterpret_cast<void *>(GetProcAddress(newdev, "DiInstallDriverW")));
      if (!fn_install) {
        FreeLibrary(newdev);
        done();
        return;
      }

      auto devices = make_enumerator();
      auto policy = make_policy();
      std::optional<defaults_t> saved;
      if (devices) {
        saved = save_defaults(devices.get());
      }

      WCHAR path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(k_driver_path, path, ARRAYSIZE(path));
      BOOST_LOG(info) << "fork: microphone: installing the Steam Streaming Microphone"sv;
      if (fn_install(nullptr, path, 0, nullptr)) {
        BOOST_LOG(info) << "fork: microphone: installed the Steam Streaming Microphone"sv;
        // As for Steam Streaming Speakers: let Windows set the new endpoints up, then put the
        // defaults back, so its speakers never play the PC's sound and nothing else records it
        Sleep(5000);
        if (saved && policy) {
          for (int i = 0; i < 3; ++i) {
            if (!saved->render[i].empty() && default_id(devices.get(), eRender, k_roles[i]) != saved->render[i]) {
              policy->SetDefaultEndpoint(saved->render[i].c_str(), k_roles[i]);
            }
            if (!saved->capture[i].empty() && default_id(devices.get(), eCapture, k_roles[i]) != saved->capture[i]) {
              policy->SetDefaultEndpoint(saved->capture[i].c_str(), k_roles[i]);
            }
          }
        }
      } else {
        switch (auto err = GetLastError()) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "fork: microphone: installing the Steam Streaming Microphone needs administrator privileges"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(warning) << "fork: microphone: Steam's microphone driver isn't on this PC (is Steam installed?), so LuzLunar's Mac microphone is off"sv;
            break;
          default:
            BOOST_LOG(warning) << "fork: microphone: installing the Steam Streaming Microphone failed: "sv << err;
            break;
        }
      }
      FreeLibrary(newdev);
      done();
    }

    std::mutex g_owner_mutex;
    std::weak_ptr<session_t> g_owner;
  }  // namespace

  class session_t {
  public:
    explicit session_t(std::function<void(std::string)> send):
        send_state {std::move(send)},
        stop_event {CreateEventW(nullptr, TRUE, FALSE, nullptr)} {
    }

    ~session_t() {
      if (thread.joinable()) {
        SetEvent(stop_event);
        thread.join();
      }
      if (stop_event) {
        CloseHandle(stop_event);
      }
    }

    void run();

    std::function<void(std::string)> send_state;
    HANDLE stop_event;
    std::thread thread;

    std::mutex jitter_mutex;
    mic_policy::jitter_buffer_t jitter;
    std::atomic<std::uint64_t> refused {0};  ///< Frames that weren't 20 ms of Opus

  private:
    bool stopping(DWORD wait_ms = 0) const {
      return WaitForSingleObject(stop_event, wait_ms) == WAIT_OBJECT_0;
    }

    void tell(luzmic::state_t state) {
      if (!m_told || *m_told != state) {
        m_told = state;
        send_state(luzmic::encode_state(state));
      }
    }

    bool find_endpoints(IMMDeviceEnumerator *devices);
    bool open_render();
    void close_render();
    void fill();
    void decode(const mic_policy::slot_t &slot, float *out);
    bool anyone_recording();
    void log_stats(const char *when);

    std::optional<luzmic::state_t> m_told;
    com_t<IMMDevice> m_render_device;
    com_t<IMMDevice> m_capture_device;
    std::wstring m_capture_id;
    com_t<IAudioClient> m_client;
    com_t<IAudioRenderClient> m_render;
    com_t<IAudioSessionManager2> m_sessions;
    HANDLE m_render_event = nullptr;
    UINT32 m_buffer_frames = 0;
    UINT32 m_target_frames = 0;  ///< How much to keep queued: two periods
    bool m_float = true;
    OpusDecoder *m_decoder = nullptr;
    std::vector<float> m_pcm;  ///< Decoded, not yet written
    std::size_t m_pcm_at = 0;
    std::vector<float> m_scratch = std::vector<float>(luzmic::k_frame_samples);
    std::uint64_t m_decode_errors = 0;
    bool m_sessions_failed = false;
  };

  bool session_t::find_endpoints(IMMDeviceEnumerator *devices) {
    m_render_device = find_endpoint(devices, eRender);
    m_capture_device = find_endpoint(devices, eCapture);
    if (!m_render_device || !m_capture_device) {
      m_render_device.reset();
      m_capture_device.reset();
      return false;
    }
    m_capture_id = device_id(m_capture_device.get());
    return true;
  }

  bool session_t::open_render() {
    if (FAILED(m_render_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, m_client.put_void()))) {
      return false;
    }

    // Mono 48 kHz, the frames' own format; Windows converts it to the endpoint's
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    WAVEFORMATEX format {WAVE_FORMAT_IEEE_FLOAT, 1, luzmic::k_sample_rate, luzmic::k_sample_rate * 4, 4, 32, 0};
    m_float = true;
    auto status = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, k_buffer_duration, 0, &format, nullptr);
    if (FAILED(status)) {
      // A fresh client for the second try: Initialize() can't run twice
      if (FAILED(m_render_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, m_client.put_void()))) {
        return false;
      }
      format = {WAVE_FORMAT_PCM, 1, luzmic::k_sample_rate, luzmic::k_sample_rate * 2, 2, 16, 0};
      m_float = false;
      status = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, k_buffer_duration, 0, &format, nullptr);
    }
    if (FAILED(status)) {
      BOOST_LOG(warning) << "fork: microphone: the Steam Streaming Microphone's speakers won't play mono 48 kHz: 0x"sv << hex(status);
      m_client.reset();
      return false;
    }

    REFERENCE_TIME period = 0;
    m_client->GetDevicePeriod(&period, nullptr);
    m_client->GetBufferSize(&m_buffer_frames);
    const auto period_frames = static_cast<UINT32>(period * luzmic::k_sample_rate / 10000000);
    m_target_frames = std::min(m_buffer_frames, std::max<UINT32>(2 * period_frames, luzmic::k_frame_samples / 2));

    if (!m_render_event) {
      m_render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (FAILED(m_client->SetEventHandle(m_render_event)) || FAILED(m_client->GetService(__uuidof(IAudioRenderClient), m_render.put_void()))) {
      close_render();
      return false;
    }

    // Start with silence, so the first slots have time to come
    BYTE *data = nullptr;
    if (SUCCEEDED(m_render->GetBuffer(m_target_frames, &data))) {
      m_render->ReleaseBuffer(m_target_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (FAILED(m_client->Start())) {
      close_render();
      return false;
    }
    return true;
  }

  void session_t::close_render() {
    if (m_client) {
      m_client->Stop();
    }
    m_render.reset();
    m_client.reset();
  }

  void session_t::decode(const mic_policy::slot_t &slot, float *out) {
    using play_e = mic_policy::slot_t::play_e;
    if (slot.reset) {
      opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
    }
    if (!slot.skipped.empty()) {
      opus_decode_float(m_decoder, slot.skipped.data(), static_cast<opus_int32>(slot.skipped.size()), m_scratch.data(), luzmic::k_frame_samples, 0);
    }

    int decoded = luzmic::k_frame_samples;
    switch (slot.play) {
      case play_e::silence:
        std::fill(out, out + luzmic::k_frame_samples, 0.0f);
        return;
      case play_e::frame:
        decoded = opus_decode_float(m_decoder, slot.opus.data(), static_cast<opus_int32>(slot.opus.size()), out, luzmic::k_frame_samples, 0);
        break;
      case play_e::fec:
        decoded = opus_decode_float(m_decoder, slot.opus.data(), static_cast<opus_int32>(slot.opus.size()), out, luzmic::k_frame_samples, 1);
        break;
      case play_e::conceal:
        decoded = opus_decode_float(m_decoder, nullptr, 0, out, luzmic::k_frame_samples, 0);
        break;
    }
    if (decoded != luzmic::k_frame_samples) {
      ++m_decode_errors;
      if (decoded < 0) {
        opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
      }
      std::fill(out + std::max(decoded, 0), out + luzmic::k_frame_samples, 0.0f);
    }
  }

  void session_t::fill() {
    UINT32 padding = 0;
    auto status = m_client->GetCurrentPadding(&padding);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "fork: microphone: the Steam Streaming Microphone's speakers went away: 0x"sv << hex(status);
      close_render();
      return;
    }
    if (padding >= m_target_frames) {
      return;
    }
    const UINT32 frames = m_target_frames - padding;

    while (m_pcm.size() - m_pcm_at < frames) {
      if (m_pcm_at > 0) {
        m_pcm.erase(m_pcm.begin(), m_pcm.begin() + static_cast<std::ptrdiff_t>(m_pcm_at));
        m_pcm_at = 0;
      }
      mic_policy::slot_t slot;
      {
        std::lock_guard lock {jitter_mutex};
        slot = jitter.pull();
      }
      const auto at = m_pcm.size();
      m_pcm.resize(at + luzmic::k_frame_samples);
      decode(slot, m_pcm.data() + at);
    }

    BYTE *data = nullptr;
    status = m_render->GetBuffer(frames, &data);
    if (FAILED(status)) {
      if (status != AUDCLNT_E_BUFFER_TOO_LARGE) {
        close_render();
      }
      return;
    }
    const float *pcm = m_pcm.data() + m_pcm_at;
    if (m_float) {
      std::copy(pcm, pcm + frames, reinterpret_cast<float *>(data));
    } else {
      auto out = reinterpret_cast<std::int16_t *>(data);
      for (UINT32 i = 0; i < frames; ++i) {
        out[i] = static_cast<std::int16_t>(std::clamp(pcm[i], -1.0f, 1.0f) * 32767.0f);
      }
    }
    m_render->ReleaseBuffer(frames, 0);
    m_pcm_at += frames;
  }

  bool session_t::anyone_recording() {
    if (!m_sessions && FAILED(m_capture_device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, m_sessions.put_void()))) {
      if (!m_sessions_failed) {
        m_sessions_failed = true;
        BOOST_LOG(warning) << "fork: microphone: can't see who records from the Steam Streaming Microphone"sv;
      }
      return false;
    }
    com_t<IAudioSessionEnumerator> sessions;
    if (FAILED(m_sessions->GetSessionEnumerator(sessions.put()))) {
      return false;
    }
    int count = 0;
    sessions->GetCount(&count);
    const auto own = GetCurrentProcessId();
    for (int i = 0; i < count; ++i) {
      com_t<IAudioSessionControl> control;
      if (FAILED(sessions->GetSession(i, control.put()))) {
        continue;
      }
      AudioSessionState state = AudioSessionStateInactive;
      if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive) {
        continue;
      }
      com_t<IAudioSessionControl2> control2;
      if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), control2.put_void()))) {
        DWORD pid = 0;
        control2->GetProcessId(&pid);
        if (control2->IsSystemSoundsSession() == S_OK || pid == own) {
          continue;
        }
        BOOST_LOG(debug) << "fork: microphone: process "sv << pid << " records from the Steam Streaming Microphone"sv;
      }
      return true;
    }
    return false;
  }

  void session_t::log_stats(const char *when) {
    mic_policy::stats_t stats;
    {
      std::lock_guard lock {jitter_mutex};
      stats = jitter.take_stats();
    }
    if (stats.frames == 0 && stats.restarts == 0) {
      return;
    }
    BOOST_LOG(info) << "fork: microphone ("sv << when << "): "sv << stats.frames << " frames played, "sv << stats.fec << " rebuilt, "sv
                    << stats.concealed << " concealed, "sv << stats.waits << " waits, "sv << stats.late << " late, "sv << stats.dropped
                    << " dropped, "sv << stats.underruns << " underruns, "sv << refused.exchange(0) << " refused, "sv << m_decode_errors
                    << " decode errors"sv;
    m_decode_errors = 0;
  }

  void session_t::run() {
    const auto co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co)) {
      BOOST_LOG(warning) << "fork: microphone: COM failed: 0x"sv << hex(co);
      return;
    }

    auto devices = make_enumerator();
    auto policy = make_policy();
    int opus_status = 0;
    m_decoder = opus_decoder_create(luzmic::k_sample_rate, 1, &opus_status);

    // The Steam Streaming Microphone, installed first when it's missing
    bool found = devices && m_decoder && find_endpoints(devices.get());
    while (!found && devices && m_decoder && config::audio.install_steam_drivers) {
      auto expected = install_e::not_tried;
      if (g_install.compare_exchange_strong(expected, install_e::running)) {
        std::thread {install_driver}.detach();
      } else if (expected == install_e::done) {
        found = find_endpoints(devices.get());
        break;
      }
      if (stopping(500)) {
        break;
      }
      found = find_endpoints(devices.get());
    }

    std::optional<defaults_t> saved;
    if (found && !stopping()) {
      // Never into the PC's own speakers: the stream would send the Mac its own voice
      const auto render_id = device_id(m_render_device.get());
      if (default_id(devices.get(), eRender, eConsole) == render_id) {
        BOOST_LOG(error) << "fork: microphone: Windows plays its sound through \"Speakers (Steam Streaming Microphone)\"; choose other speakers to use LuzLunar's Mac microphone"sv;
        found = false;
      }
    }
    if (found && !stopping() && open_render()) {
      // Apps that record from the default recording device hear the Mac
      saved = save_defaults(devices.get());
      if (policy) {
        for (auto role : k_roles) {
          policy->SetDefaultEndpoint(m_capture_id.c_str(), role);
        }
      }
      BOOST_LOG(info) << "fork: microphone: LuzLunar's Mac microphone plays into the Steam Streaming Microphone, the default recording device for the stream"sv;

      DWORD task_index = 0;
      auto mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
      if (!mmcss) {
        BOOST_LOG(debug) << "fork: microphone: no Pro Audio thread priority"sv;
      }

      mic_policy::recording_t recording;
      auto next_poll = std::chrono::steady_clock::now();
      auto next_reopen = next_poll;
      tell({luzmic::flag_ready});
      HANDLE events[] = {stop_event, m_render_event};
      while (true) {
        const auto woke = WaitForMultipleObjects(2, events, FALSE, 100);
        if (woke == WAIT_OBJECT_0) {
          break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (m_client) {
          fill();
        } else if (now >= next_reopen) {
          // The endpoint went (disabled, or the driver reinstalled): try again every 2 s
          next_reopen = now + 2s;
          if (find_endpoints(devices.get()) && open_render()) {
            m_sessions.reset();
            BOOST_LOG(info) << "fork: microphone: the Steam Streaming Microphone is back"sv;
          }
        }
        if (now >= next_poll) {
          next_poll = now + mic_policy::k_poll_interval;
          const bool was = recording.recording();
          const bool is = recording.update(m_capture_device && m_client && anyone_recording(), now);
          if (is != was) {
            BOOST_LOG(info) << (is ? "fork: microphone: an app records from the Steam Streaming Microphone: the Mac's microphone is on"sv :
                                     "fork: microphone: nothing records from the Steam Streaming Microphone: the Mac's microphone is off"sv);
            if (!is) {
              log_stats("a recording ended");
            }
          }
          tell({static_cast<std::uint8_t>(luzmic::flag_ready | (is ? luzmic::flag_recording : 0))});
        }
      }
      log_stats("the stream ended");
      if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
      }
    } else if (!stopping()) {
      if (found) {
        BOOST_LOG(warning) << "fork: microphone: the Steam Streaming Microphone's speakers won't open, so LuzLunar's Mac microphone is off"sv;
      } else if (!config::audio.install_steam_drivers) {
        BOOST_LOG(info) << "fork: microphone: no Steam Streaming Microphone, and install_steam_audio_drivers is off, so LuzLunar's Mac microphone is off"sv;
      }
      tell({});
    }

    close_render();
    if (saved && policy && devices) {
      for (int i = 0; i < 3; ++i) {
        const auto current = default_id(devices.get(), eCapture, k_roles[i]);
        if (auto back = mic_policy::restore_default(saved->capture[i], current, m_capture_id)) {
          policy->SetDefaultEndpoint(back->c_str(), k_roles[i]);
        }
      }
    }
    m_sessions.reset();
    m_render_device.reset();
    m_capture_device.reset();
    policy.reset();
    devices.reset();
    if (m_render_event) {
      CloseHandle(m_render_event);
      m_render_event = nullptr;
    }
    if (m_decoder) {
      opus_decoder_destroy(m_decoder);
      m_decoder = nullptr;
    }
    CoUninitialize();
  }

  bool enabled() {
    return fork_tweaks::mac_microphone;
  }

  std::shared_ptr<session_t> start(std::function<void(std::string payload)> send_state) {
    if (!enabled()) {
      return nullptr;
    }
    std::lock_guard lock {g_owner_mutex};
    if (!g_owner.expired()) {
      BOOST_LOG(info) << "fork: microphone: another stream has the Steam Streaming Microphone"sv;
      return nullptr;
    }
    auto session = std::make_shared<session_t>(std::move(send_state));
    if (!session->stop_event) {
      return nullptr;
    }
    session->thread = std::thread {[raw = session.get()]() {
      raw->run();
    }};
    g_owner = session;
    return session;
  }

  void receive(session_t &session, const std::uint8_t *packet, std::size_t size) {
    luzmic::frame_t frame;
    if (!luzmic::decode_frame(packet, size, frame) ||
        opus_packet_get_nb_samples(frame.opus, static_cast<opus_int32>(frame.opus_size), luzmic::k_sample_rate) != luzmic::k_frame_samples) {
      ++session.refused;
      return;
    }
    std::lock_guard lock {session.jitter_mutex};
    session.jitter.push(frame);
  }

  void stop(session_t &session) {
    SetEvent(session.stop_event);
    if (session.thread.joinable()) {
      session.thread.join();
    }
    std::lock_guard lock {g_owner_mutex};
    if (g_owner.lock().get() == &session) {
      g_owner.reset();
    }
  }

#else

  class session_t {};

  bool enabled() {
    return false;
  }

  std::shared_ptr<session_t> start(std::function<void(std::string payload)>) {
    return nullptr;
  }

  void receive(session_t &, const std::uint8_t *, std::size_t) {
  }

  void stop(session_t &) {
  }

#endif
}  // namespace fork_mic
