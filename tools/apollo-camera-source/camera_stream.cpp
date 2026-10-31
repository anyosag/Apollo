// Mac Camera's media stream (camera_source.h). After Microsoft's VirtualCamera sample's
// SimpleMediaStream (MIT, Copyright (c) Microsoft Corporation).
#include "camera_source.h"

#include <chrono>

namespace apollo_camera {
  namespace {
    namespace shared = camera_policy::shared;
    namespace nv12 = camera_policy::nv12;
    using steady = std::chrono::steady_clock;

    /// A frame's time at 30 fps, in Media Foundation's 100 ns units
    constexpr LONGLONG k_frame_duration = 10'000'000 / camera_policy::k_fps;

    /// Apollo's side of the shared memory (camera_policy.h), opened while the stream runs
    struct apollo_t {
      HANDLE frames_map = nullptr;
      HANDLE control_map = nullptr;
      HANDLE frame_event = nullptr;
      HANDLE state_event = nullptr;
      std::uint8_t *frames = nullptr;
      std::uint8_t *control = nullptr;

      ~apollo_t() {
        close();
      }

      bool open() {
        close();
        frames_map = OpenFileMappingW(FILE_MAP_READ, FALSE, shared::k_frames_name);
        control_map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, shared::k_control_name);
        frame_event = OpenEventW(SYNCHRONIZE, FALSE, shared::k_frame_event_name);
        state_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, shared::k_state_event_name);
        if (!frames_map || !control_map || !frame_event || !state_event) {
          close();
          return false;
        }
        frames = static_cast<std::uint8_t *>(MapViewOfFile(frames_map, FILE_MAP_READ, 0, 0, shared::k_frames_bytes));
        control = static_cast<std::uint8_t *>(MapViewOfFile(control_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, shared::k_control_bytes));
        if (!frames || !control || !shared::frames_valid(frames)) {
          close();
          return false;
        }
        return true;
      }

      void close() {
        if (frames) {
          UnmapViewOfFile(frames);
          frames = nullptr;
        }
        if (control) {
          UnmapViewOfFile(control);
          control = nullptr;
        }
        for (HANDLE *handle : {&frames_map, &control_map, &frame_event, &state_event}) {
          if (*handle) {
            CloseHandle(*handle);
            *handle = nullptr;
          }
        }
      }
    };

    HRESULT make_type(camera_policy::frame_size_t size, IMFMediaType **out) {
      ComPtr<IMFMediaType> type;
      HRESULT hr = MFCreateMediaType(&type);
      if (FAILED(hr)) {
        return hr;
      }
      type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
      type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
      MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, size.width, size.height);
      MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, camera_policy::k_fps, 1);
      MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
      type->SetUINT32(MF_MT_DEFAULT_STRIDE, size.width);
      type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32) (camera_policy::nv12_bytes(size.width, size.height) * 8 * camera_policy::k_fps));
      // What the Mac's camera and Apollo's placeholder are
      type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
      type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
      *out = type.Detach();
      return S_OK;
    }
  }  // namespace

  MediaStream::MediaStream() {
    g_objects++;
    m_Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }

  MediaStream::~MediaStream() {
    if (m_Pump.joinable()) {
      m_PumpStop->store(true);
      SetEvent(m_Wake);
      m_Pump.join();
    }
    if (m_Wake) {
      CloseHandle(m_Wake);
    }
    g_objects--;
  }

  STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2)) {
      *ppv = static_cast<IMFMediaStream2 *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) MediaStream::AddRef() {
    return ++m_Refs;
  }

  STDMETHODIMP_(ULONG) MediaStream::Release() {
    ULONG refs = --m_Refs;
    if (refs == 0) {
      delete this;
    }
    return refs;
  }

  HRESULT MediaStream::Initialize(IMFMediaSource *source, DWORD id) {
    std::lock_guard lock {m_Lock};
    if (!source || !m_Wake) {
      return E_INVALIDARG;
    }
    m_Parent = source;
    m_Id = id;

    constexpr DWORD count = (DWORD) std::size(camera_policy::k_sizes);
    IMFMediaType *types[count] {};
    HRESULT hr = S_OK;
    for (DWORD i = 0; i < count && SUCCEEDED(hr); i++) {
      hr = make_type(camera_policy::k_sizes[i], &types[i]);
    }
    if (SUCCEEDED(hr)) {
      hr = MFCreateAttributes(&m_Attributes, 4);
    }
    if (SUCCEEDED(hr)) {
      hr = SetStreamAttributes(m_Attributes.Get());
    }
    if (SUCCEEDED(hr)) {
      hr = MFCreateEventQueue(&m_Queue);
    }
    if (SUCCEEDED(hr)) {
      hr = MFCreateStreamDescriptor(m_Id, count, types, &m_Descriptor);
    }
    ComPtr<IMFMediaTypeHandler> handler;
    if (SUCCEEDED(hr)) {
      hr = m_Descriptor->GetMediaTypeHandler(&handler);
    }
    if (SUCCEEDED(hr)) {
      // 720p unless the app asks for 1080p
      hr = handler->SetCurrentMediaType(types[0]);
    }
    if (SUCCEEDED(hr)) {
      hr = SetStreamAttributes(m_Descriptor.Get());
    }
    for (IMFMediaType *type : types) {
      if (type) {
        type->Release();
      }
    }
    return hr;
  }

  HRESULT MediaStream::SetStreamAttributes(IMFAttributes *attributes) {
    HRESULT hr = attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(hr)) {
      hr = attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_Id);
    }
    if (SUCCEEDED(hr)) {
      // Apps share the stream through Frame Server
      hr = attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    }
    if (SUCCEEDED(hr)) {
      hr = attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    }
    return hr;
  }

  HRESULT MediaStream::CheckShutdown() const {
    if (m_Shutdown) {
      return MF_E_SHUTDOWN;
    }
    if (!m_Queue) {
      return E_UNEXPECTED;
    }
    return S_OK;
  }

  HRESULT MediaStream::CopyAttributes(IMFAttributes **attributes) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Attributes.CopyTo(attributes);
  }

  STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Queue->BeginGetEvent(pCallback, punkState);
  }

  STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Queue->EndGetEvent(pResult, ppEvent);
  }

  STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) {
    ComPtr<IMFMediaEventQueue> queue;
    {
      std::lock_guard lock {m_Lock};
      HRESULT hr = CheckShutdown();
      if (FAILED(hr)) {
        return hr;
      }
      queue = m_Queue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
  }

  STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT *pvValue) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
  }

  STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource **ppMediaSource) {
    if (!ppMediaSource) {
      return E_POINTER;
    }
    *ppMediaSource = nullptr;
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Parent.CopyTo(ppMediaSource);
  }

  STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor **ppStreamDescriptor) {
    if (!ppStreamDescriptor) {
      return E_POINTER;
    }
    *ppStreamDescriptor = nullptr;
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Descriptor.CopyTo(ppStreamDescriptor);
  }

  STDMETHODIMP MediaStream::RequestSample(IUnknown *pToken) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (m_State != MF_STREAM_STATE_RUNNING) {
      return MF_E_INVALIDREQUEST;
    }
    m_Requests.emplace_back(pToken);
    // Frame Server has only so many samples; more requests than that are stale
    while (m_Requests.size() > 32) {
      m_Requests.pop_front();
    }
    SetEvent(m_Wake);
    return S_OK;
  }

  STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::unique_lock lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (m_State == state) {
      return S_OK;
    }
    switch (state) {
      case MF_STREAM_STATE_PAUSED:
        if (m_State != MF_STREAM_STATE_RUNNING) {
          return MF_E_INVALID_STATE_TRANSITION;
        }
        m_State = MF_STREAM_STATE_PAUSED;
        return S_OK;
      case MF_STREAM_STATE_RUNNING:
        return StartLocked(false, nullptr, lock);
      case MF_STREAM_STATE_STOPPED:
        return StopLocked(false, lock);
      default:
        return MF_E_INVALID_STATE_TRANSITION;
    }
  }

  STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE *pState) {
    if (!pState) {
      return E_INVALIDARG;
    }
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    *pState = m_State;
    return S_OK;
  }

  HRESULT MediaStream::SetAllocator(IMFVideoSampleAllocator *allocator) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (m_State == MF_STREAM_STATE_RUNNING) {
      return MF_E_INVALIDREQUEST;
    }
    if (m_OwnAllocator && m_Allocator) {
      m_Allocator->UninitializeSampleAllocator();
    }
    m_Allocator = allocator;
    m_OwnAllocator = false;
    return S_OK;
  }

  HRESULT MediaStream::Start(IMFMediaType *type) {
    if (!type) {
      return E_INVALIDARG;
    }
    std::unique_lock lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : StartLocked(true, type, lock);
  }

  HRESULT MediaStream::Stop(bool send_event) {
    std::unique_lock lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : StopLocked(send_event, lock);
  }

  HRESULT MediaStream::StartLocked(bool send_event, IMFMediaType *type, std::unique_lock<std::mutex> &lock) {
    bool changed = false;
    if (type) {
      BOOL same = FALSE;
      if (!m_Type || FAILED(m_Type->Compare(type, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &same)) || !same) {
        m_Type = type;
        changed = true;
      }
    }
    if (!m_Type) {
      return MF_E_INVALIDREQUEST;
    }

    if (changed || !m_Pump.joinable()) {
      // Another size: the old pump goes first
      if (m_Pump.joinable()) {
        m_PumpStop->store(true);
        SetEvent(m_Wake);
        std::thread pump = std::move(m_Pump);
        lock.unlock();
        pump.join();
        lock.lock();
        if (m_Shutdown) {
          return MF_E_SHUTDOWN;
        }
      }
      UINT32 width = 0;
      UINT32 height = 0;
      HRESULT hr = MFGetAttributeSize(m_Type.Get(), MF_MT_FRAME_SIZE, &width, &height);
      if (FAILED(hr)) {
        return hr;
      }
      camera_policy::frame_size_t size {width, height};
      bool offered = false;
      for (const auto &offer : camera_policy::k_sizes) {
        offered = offered || offer == size;
      }
      if (!offered) {
        return MF_E_INVALIDMEDIATYPE;
      }
      // Frame Server gives its allocator (SetDefaultAllocator); a caller that doesn't gets one of ours
      if (!m_Allocator) {
        hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(m_Allocator.GetAddressOf()));
        if (FAILED(hr)) {
          return hr;
        }
        m_OwnAllocator = true;
      } else if (m_OwnAllocator) {
        m_Allocator->UninitializeSampleAllocator();
      }
      hr = m_Allocator->InitializeSampleAllocator(10, m_Type.Get());
      if (FAILED(hr)) {
        return hr;
      }
      m_Size = size;
      m_PumpStop = std::make_shared<std::atomic_bool>(false);
      m_Pump = std::thread(&MediaStream::Pump, this, m_PumpStop, size);
    }

    m_State = MF_STREAM_STATE_RUNNING;
    if (send_event) {
      return m_Queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    }
    return S_OK;
  }

  HRESULT MediaStream::StopLocked(bool send_event, std::unique_lock<std::mutex> &lock) {
    m_State = MF_STREAM_STATE_STOPPED;
    // Requests never answered go with the stream's samples
    m_Requests.clear();
    if (m_Pump.joinable()) {
      m_PumpStop->store(true);
      SetEvent(m_Wake);
      std::thread pump = std::move(m_Pump);
      lock.unlock();
      pump.join();
      lock.lock();
    }
    if (m_Shutdown || !m_Queue) {
      return S_OK;
    }
    if (send_event) {
      return m_Queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    return S_OK;
  }

  HRESULT MediaStream::Shutdown() {
    std::unique_lock lock {m_Lock};
    m_Shutdown = true;
    m_State = MF_STREAM_STATE_STOPPED;
    m_Requests.clear();
    if (m_Pump.joinable()) {
      m_PumpStop->store(true);
      SetEvent(m_Wake);
      std::thread pump = std::move(m_Pump);
      lock.unlock();
      pump.join();
      lock.lock();
    }
    if (m_Queue) {
      m_Queue->Shutdown();
      m_Queue.Reset();
    }
    if (m_OwnAllocator && m_Allocator) {
      m_Allocator->UninitializeSampleAllocator();
    }
    m_Allocator.Reset();
    m_Attributes.Reset();
    m_Descriptor.Reset();
    m_Type.Reset();
    // The source goes last, outside the lock
    ComPtr<IMFMediaSource> parent = std::move(m_Parent);
    lock.unlock();
    return S_OK;
  }

  void MediaStream::Pump(std::shared_ptr<std::atomic_bool> stop, camera_policy::frame_size_t size) {
    apollo_t apollo;
    shared::apps_t apps;
    apps.streaming = true;
    apps.size = size;
    apps.fps = camera_policy::k_fps;
    apps.pid = GetCurrentProcessId();
    bool told = false;  // the control block says this stream streams
    std::uint64_t last_try = 0;
    std::uint64_t last_beat = 0;
    std::uint64_t delivered = UINT64_MAX;  // the newest of Apollo's frames answered with
    auto last_delivery = steady::now() - std::chrono::seconds(1);

    while (!stop->load()) {
      std::uint64_t tick = GetTickCount64();
      // Apollo may start after the camera: look for it every second
      if (!apollo.frames && tick - last_try >= 1000) {
        last_try = tick;
        told = false;
        apollo.open();
      }
      if (apollo.control && !told) {
        shared::write_control(apollo.control, apps, tick);
        SetEvent(apollo.state_event);
        told = true;
        last_beat = tick;
      } else if (apollo.control && tick - last_beat >= camera_policy::heartbeat_interval_ms) {
        shared::beat(apollo.control, tick);
        last_beat = tick;
      }

      // A request, and something new to answer it with: Apollo's next frame, or after a frame's
      // time, the same again
      bool pending;
      {
        std::lock_guard lock {m_Lock};
        pending = !m_Requests.empty() && m_State == MF_STREAM_STATE_RUNNING;
      }
      auto now = steady::now();
      std::uint64_t published = apollo.frames ? shared::published(apollo.frames) : 0;
      if (pending && (published != delivered || now - last_delivery >= std::chrono::milliseconds(33))) {
        if (Deliver(apollo.frames, tick)) {
          delivered = published;
          last_delivery = now;
        }
      }

      auto since = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - last_delivery).count();
      DWORD timeout = (DWORD) (since >= 33 ? 1 : 33 - since);
      HANDLE handles[2] {m_Wake, apollo.frame_event};
      WaitForMultipleObjects(apollo.frame_event ? 2 : 1, handles, FALSE, timeout);
    }

    if (apollo.control && told) {
      // Unless a newer pump of this stream (another size) has taken over
      std::lock_guard lock {m_Lock};
      if (m_PumpStop == stop || !m_Pump.joinable()) {
        apps.streaming = false;
        shared::write_control(apollo.control, apps, GetTickCount64());
        SetEvent(apollo.state_event);
      }
    }
  }

  bool MediaStream::Deliver(std::uint8_t *frames, std::uint64_t now_ms) {
    ComPtr<IMFSample> sample;
    ComPtr<IUnknown> token;
    camera_policy::frame_size_t size;
    {
      std::lock_guard lock {m_Lock};
      if (m_Requests.empty() || !m_Allocator || !m_Queue || m_State != MF_STREAM_STATE_RUNNING) {
        return false;
      }
      // None free: the app still holds them all; this tries again
      if (FAILED(m_Allocator->AllocateSample(&sample))) {
        return false;
      }
      token = m_Requests.front();
      m_Requests.pop_front();
      size = m_Size;
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) {
      return false;
    }
    BYTE *scanline = nullptr;
    LONG pitch = 0;
    BYTE *start = nullptr;
    DWORD length = 0;
    ComPtr<IMF2DBuffer2> buffer_2d;
    bool locked_2d = SUCCEEDED(buffer.As(&buffer_2d)) && SUCCEEDED(buffer_2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length));
    if (!locked_2d) {
      DWORD max_length = 0;
      if (FAILED(buffer->Lock(&start, &max_length, &length))) {
        return false;
      }
      scanline = start;
      pitch = (LONG) size.width;
      length = max_length;
    }
    std::size_t needed = (std::size_t) pitch * size.height + (std::size_t) pitch * ((size.height + 1) / 2);
    bool fits = pitch >= (LONG) size.width && length >= needed;
    if (fits) {
      bool drawn = false;
      if (frames && shared::frames_alive(frames, now_ms)) {
        // A copy Apollo wrote over meanwhile is read again
        for (int attempt = 0; attempt < 3 && !drawn; attempt++) {
          drawn = shared::read_latest(frames, [&](const shared::frame_view_t &view) {
            nv12::copy_scaled(view.nv12, view.width, view.height, scanline, size.width, size.height, (std::size_t) pitch);
          });
        }
      }
      if (!drawn) {
        // No Apollo: the camera's own icon
        nv12::fill(scanline, size.width, size.height, (std::size_t) pitch, nv12::k_background);
        nv12::draw_camera_icon(scanline, size.width, size.height, (std::size_t) pitch);
      }
    }
    if (locked_2d) {
      buffer_2d->Unlock2D();
    } else {
      buffer->Unlock();
      buffer->SetCurrentLength((DWORD) needed);
    }
    if (!fits) {
      return false;
    }

    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(k_frame_duration);
    if (token) {
      sample->SetUnknown(MFSampleExtension_Token, token.Get());
    }
    std::lock_guard lock {m_Lock};
    if (!m_Queue || m_State != MF_STREAM_STATE_RUNNING) {
      return false;
    }
    return SUCCEEDED(m_Queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get()));
  }
}  // namespace apollo_camera
