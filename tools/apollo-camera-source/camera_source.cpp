// Mac Camera's media source: the DLL's exports, its class factory, the activate object and the source
// (camera_source.h). After Microsoft's VirtualCamera sample (MIT, Copyright (c) Microsoft Corporation).
#include "camera_source.h"

namespace apollo_camera {
  std::atomic<long> g_objects {0};
  std::atomic<long> g_locks {0};
  HMODULE g_module = nullptr;

  namespace {
    // The sensor profile functions are mfsensorgroup.dll's; looked up rather than imported, so the
    // DLL loads wherever Media Foundation does
    using create_profile_t = HRESULT(WINAPI *)(REFGUID, UINT32, LPCWSTR, IMFSensorProfile **);
    using create_collection_t = HRESULT(WINAPI *)(IMFSensorProfileCollection **);

    class ClassFactory final: public IClassFactory {
    public:
      ClassFactory() {
        g_objects++;
      }

      ~ClassFactory() {
        g_objects--;
      }

      STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
          return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
          *ppv = static_cast<IClassFactory *>(this);
          AddRef();
          return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
      }

      STDMETHODIMP_(ULONG) AddRef() override {
        return ++m_Refs;
      }

      STDMETHODIMP_(ULONG) Release() override {
        ULONG refs = --m_Refs;
        if (refs == 0) {
          delete this;
        }
        return refs;
      }

      STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override {
        if (!ppv) {
          return E_POINTER;
        }
        *ppv = nullptr;
        if (outer) {
          return CLASS_E_NOAGGREGATION;
        }
        auto *activate = new (std::nothrow) Activate();
        if (!activate) {
          return E_OUTOFMEMORY;
        }
        HRESULT hr = activate->Initialize();
        if (SUCCEEDED(hr)) {
          hr = activate->QueryInterface(riid, ppv);
        }
        activate->Release();
        return hr;
      }

      STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
          g_locks++;
        } else {
          g_locks--;
        }
        return S_OK;
      }

    private:
      std::atomic<ULONG> m_Refs {1};
    };
  }  // namespace

  // Activate

  Activate::Activate() {
    g_objects++;
  }

  Activate::~Activate() {
    g_objects--;
  }

  STDMETHODIMP Activate::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
      *ppv = static_cast<IMFActivate *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) Activate::AddRef() {
    return ++m_Refs;
  }

  STDMETHODIMP_(ULONG) Activate::Release() {
    ULONG refs = --m_Refs;
    if (refs == 0) {
      delete this;
    }
    return refs;
  }

  HRESULT Activate::Initialize() {
    HRESULT hr = MFCreateAttributes(&m_Attributes, 3);
    if (SUCCEEDED(hr)) {
      // As VCamSample does: the source's class, for whoever looks at the activate's attributes
      m_Attributes->SetGUID(MFT_TRANSFORM_CLSID_Attribute, k_clsid);
    }
    return hr;
  }

  STDMETHODIMP Activate::ActivateObject(REFIID riid, void **ppv) {
    if (!ppv) {
      return E_POINTER;
    }
    *ppv = nullptr;
    std::lock_guard lock {m_Lock};
    if (!m_Source) {
      ComPtr<MediaSource> source;
      source.Attach(new (std::nothrow) MediaSource());
      if (!source) {
        return E_OUTOFMEMORY;
      }
      HRESULT hr = source->Initialize(m_Attributes.Get());
      if (FAILED(hr)) {
        return hr;
      }
      m_Source = source;
    }
    return m_Source->QueryInterface(riid, ppv);
  }

  STDMETHODIMP Activate::ShutdownObject() {
    return S_OK;
  }

  STDMETHODIMP Activate::DetachObject() {
    std::lock_guard lock {m_Lock};
    m_Source.Reset();
    return S_OK;
  }

  // The attributes, as the sample keeps them: in a store of Media Foundation's

  STDMETHODIMP Activate::GetItem(REFGUID guidKey, PROPVARIANT *pValue) {
    return m_Attributes->GetItem(guidKey, pValue);
  }

  STDMETHODIMP Activate::GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE *pType) {
    return m_Attributes->GetItemType(guidKey, pType);
  }

  STDMETHODIMP Activate::CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL *pbResult) {
    return m_Attributes->CompareItem(guidKey, Value, pbResult);
  }

  STDMETHODIMP Activate::Compare(IMFAttributes *pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL *pbResult) {
    return m_Attributes->Compare(pTheirs, MatchType, pbResult);
  }

  STDMETHODIMP Activate::GetUINT32(REFGUID guidKey, UINT32 *punValue) {
    return m_Attributes->GetUINT32(guidKey, punValue);
  }

  STDMETHODIMP Activate::GetUINT64(REFGUID guidKey, UINT64 *punValue) {
    return m_Attributes->GetUINT64(guidKey, punValue);
  }

  STDMETHODIMP Activate::GetDouble(REFGUID guidKey, double *pfValue) {
    return m_Attributes->GetDouble(guidKey, pfValue);
  }

  STDMETHODIMP Activate::GetGUID(REFGUID guidKey, GUID *pguidValue) {
    return m_Attributes->GetGUID(guidKey, pguidValue);
  }

  STDMETHODIMP Activate::GetStringLength(REFGUID guidKey, UINT32 *pcchLength) {
    return m_Attributes->GetStringLength(guidKey, pcchLength);
  }

  STDMETHODIMP Activate::GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32 *pcchLength) {
    return m_Attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength);
  }

  STDMETHODIMP Activate::GetAllocatedString(REFGUID guidKey, LPWSTR *ppwszValue, UINT32 *pcchLength) {
    return m_Attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength);
  }

  STDMETHODIMP Activate::GetBlobSize(REFGUID guidKey, UINT32 *pcbBlobSize) {
    return m_Attributes->GetBlobSize(guidKey, pcbBlobSize);
  }

  STDMETHODIMP Activate::GetBlob(REFGUID guidKey, UINT8 *pBuf, UINT32 cbBufSize, UINT32 *pcbBlobSize) {
    return m_Attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize);
  }

  STDMETHODIMP Activate::GetAllocatedBlob(REFGUID guidKey, UINT8 **ppBuf, UINT32 *pcbSize) {
    return m_Attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize);
  }

  STDMETHODIMP Activate::GetUnknown(REFGUID guidKey, REFIID riid, LPVOID *ppv) {
    return m_Attributes->GetUnknown(guidKey, riid, ppv);
  }

  STDMETHODIMP Activate::SetItem(REFGUID guidKey, REFPROPVARIANT Value) {
    return m_Attributes->SetItem(guidKey, Value);
  }

  STDMETHODIMP Activate::DeleteItem(REFGUID guidKey) {
    return m_Attributes->DeleteItem(guidKey);
  }

  STDMETHODIMP Activate::DeleteAllItems() {
    return m_Attributes->DeleteAllItems();
  }

  STDMETHODIMP Activate::SetUINT32(REFGUID guidKey, UINT32 unValue) {
    return m_Attributes->SetUINT32(guidKey, unValue);
  }

  STDMETHODIMP Activate::SetUINT64(REFGUID guidKey, UINT64 unValue) {
    return m_Attributes->SetUINT64(guidKey, unValue);
  }

  STDMETHODIMP Activate::SetDouble(REFGUID guidKey, double fValue) {
    return m_Attributes->SetDouble(guidKey, fValue);
  }

  STDMETHODIMP Activate::SetGUID(REFGUID guidKey, REFGUID guidValue) {
    return m_Attributes->SetGUID(guidKey, guidValue);
  }

  STDMETHODIMP Activate::SetString(REFGUID guidKey, LPCWSTR wszValue) {
    return m_Attributes->SetString(guidKey, wszValue);
  }

  STDMETHODIMP Activate::SetBlob(REFGUID guidKey, const UINT8 *pBuf, UINT32 cbBufSize) {
    return m_Attributes->SetBlob(guidKey, pBuf, cbBufSize);
  }

  STDMETHODIMP Activate::SetUnknown(REFGUID guidKey, IUnknown *pUnknown) {
    return m_Attributes->SetUnknown(guidKey, pUnknown);
  }

  STDMETHODIMP Activate::LockStore() {
    return m_Attributes->LockStore();
  }

  STDMETHODIMP Activate::UnlockStore() {
    return m_Attributes->UnlockStore();
  }

  STDMETHODIMP Activate::GetCount(UINT32 *pcItems) {
    return m_Attributes->GetCount(pcItems);
  }

  STDMETHODIMP Activate::GetItemByIndex(UINT32 unIndex, GUID *pguidKey, PROPVARIANT *pValue) {
    return m_Attributes->GetItemByIndex(unIndex, pguidKey, pValue);
  }

  STDMETHODIMP Activate::CopyAllItems(IMFAttributes *pDest) {
    return m_Attributes->CopyAllItems(pDest);
  }

  // MediaSource

  MediaSource::MediaSource() {
    g_objects++;
  }

  MediaSource::~MediaSource() {
    g_objects--;
  }

  STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx)) {
      *ppv = static_cast<IMFMediaSourceEx *>(this);
    } else if (riid == __uuidof(IMFGetService)) {
      *ppv = static_cast<IMFGetService *>(this);
    } else if (riid == __uuidof(IKsControl)) {
      *ppv = static_cast<IKsControl *>(this);
    } else if (riid == __uuidof(IMFSampleAllocatorControl)) {
      *ppv = static_cast<IMFSampleAllocatorControl *>(this);
    } else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) MediaSource::AddRef() {
    return ++m_Refs;
  }

  STDMETHODIMP_(ULONG) MediaSource::Release() {
    ULONG refs = --m_Refs;
    if (refs == 0) {
      delete this;
    }
    return refs;
  }

  HRESULT MediaSource::Initialize(IMFAttributes *activate_attributes) {
    std::lock_guard lock {m_Lock};
    if (m_State != state_e::invalid) {
      return MF_E_ALREADY_INITIALIZED;
    }
    HRESULT hr = MFCreateAttributes(&m_Attributes, 4);
    if (FAILED(hr)) {
      return hr;
    }
    if (activate_attributes) {
      activate_attributes->CopyAllItems(m_Attributes.Get());
    }
    // The legacy profile is required, so apps that don't know profiles still get the camera
    AddSensorProfiles();

    hr = MFCreateEventQueue(&m_Queue);
    if (FAILED(hr)) {
      return hr;
    }
    m_Stream.Attach(new (std::nothrow) MediaStream());
    if (!m_Stream) {
      return E_OUTOFMEMORY;
    }
    hr = m_Stream->Initialize(this, 0);
    if (FAILED(hr)) {
      return hr;
    }
    ComPtr<IMFStreamDescriptor> descriptor;
    hr = m_Stream->GetStreamDescriptor(&descriptor);
    if (FAILED(hr)) {
      return hr;
    }
    IMFStreamDescriptor *descriptors[] {descriptor.Get()};
    hr = MFCreatePresentationDescriptor(1, descriptors, &m_Descriptor);
    if (FAILED(hr)) {
      return hr;
    }
    m_State = state_e::stopped;
    return S_OK;
  }

  HRESULT MediaSource::AddSensorProfiles() {
    HMODULE sensors = GetModuleHandleW(L"mfsensorgroup.dll");
    if (!sensors) {
      sensors = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!sensors) {
      return E_NOTIMPL;
    }
    auto create_collection = reinterpret_cast<create_collection_t>(GetProcAddress(sensors, "MFCreateSensorProfileCollection"));
    auto create_profile = reinterpret_cast<create_profile_t>(GetProcAddress(sensors, "MFCreateSensorProfile"));
    if (!create_collection || !create_profile) {
      return E_NOTIMPL;
    }
    ComPtr<IMFSensorProfileCollection> collection;
    HRESULT hr = create_collection(&collection);
    if (FAILED(hr)) {
      return hr;
    }
    ComPtr<IMFSensorProfile> profile;
    hr = create_profile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile);
    if (FAILED(hr)) {
      return hr;
    }
    // Stream 0, any resolution, up to 30 fps, any subtype
    hr = profile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
    if (SUCCEEDED(hr)) {
      hr = collection->AddProfile(profile.Get());
    }
    if (SUCCEEDED(hr)) {
      hr = m_Attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection.Get());
    }
    return hr;
  }

  HRESULT MediaSource::CheckShutdown() const {
    if (m_State == state_e::shutdown) {
      return MF_E_SHUTDOWN;
    }
    if (!m_Queue) {
      return E_UNEXPECTED;
    }
    return S_OK;
  }

  STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Queue->BeginGetEvent(pCallback, punkState);
  }

  STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Queue->EndGetEvent(pResult, ppEvent);
  }

  STDMETHODIMP MediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) {
    // It can wait for long, so not while holding the lock
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

  STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT *pvValue) {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
  }

  STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor **ppPresentationDescriptor) {
    if (!ppPresentationDescriptor) {
      return E_POINTER;
    }
    *ppPresentationDescriptor = nullptr;
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_Descriptor->Clone(ppPresentationDescriptor);
  }

  STDMETHODIMP MediaSource::GetCharacteristics(DWORD *pdwCharacteristics) {
    if (!pdwCharacteristics) {
      return E_POINTER;
    }
    *pdwCharacteristics = 0;
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
  }

  STDMETHODIMP MediaSource::Pause() {
    return MF_E_INVALID_STATE_TRANSITION;
  }

  STDMETHODIMP MediaSource::Shutdown() {
    ComPtr<MediaStream> stream;
    {
      std::lock_guard lock {m_Lock};
      m_State = state_e::shutdown;
      m_Attributes.Reset();
      m_Descriptor.Reset();
      if (m_Queue) {
        m_Queue->Shutdown();
        m_Queue.Reset();
      }
      stream = std::move(m_Stream);
    }
    // Outside the lock: the stream stops its thread
    if (stream) {
      stream->Shutdown();
    }
    return S_OK;
  }

  HRESULT MediaSource::ValidatePresentationDescriptor(IMFPresentationDescriptor *descriptor) {
    DWORD count = 0;
    HRESULT hr = descriptor->GetStreamDescriptorCount(&count);
    if (FAILED(hr)) {
      return hr;
    }
    if (count != 1) {
      return E_INVALIDARG;
    }
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> stream;
    hr = descriptor->GetStreamDescriptorByIndex(0, &selected, &stream);
    if (FAILED(hr)) {
      return hr;
    }
    DWORD id = 0;
    hr = stream->GetStreamIdentifier(&id);
    if (FAILED(hr)) {
      return hr;
    }
    return id == m_Stream->Id() ? S_OK : E_INVALIDARG;
  }

  STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor *pPresentationDescriptor, const GUID *pguidTimeFormat, const PROPVARIANT *pvarStartPosition) {
    if (!pPresentationDescriptor || !pvarStartPosition) {
      return E_INVALIDARG;
    }
    if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL) {
      return MF_E_UNSUPPORTED_TIME_FORMAT;
    }
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (m_State == state_e::invalid) {
      return MF_E_INVALID_STATE_TRANSITION;
    }
    hr = ValidatePresentationDescriptor(pPresentationDescriptor);
    if (FAILED(hr)) {
      return hr;
    }

    PROPVARIANT start_time;
    hr = InitPropVariantFromInt64(MFGetSystemTime(), &start_time);
    if (FAILED(hr)) {
      return hr;
    }

    // The one stream: MENewStream or MEUpdatedStream when it's selected, then it starts (and sends
    // MEStreamStarted); a stream that was selected and isn't any more stops
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> descriptor;
    hr = pPresentationDescriptor->GetStreamDescriptorByIndex(0, &selected, &descriptor);
    BOOL was_selected = FALSE;
    ComPtr<IMFStreamDescriptor> ours;
    if (SUCCEEDED(hr)) {
      hr = m_Descriptor->GetStreamDescriptorByIndex(0, &was_selected, &ours);
    }
    if (SUCCEEDED(hr) && selected) {
      hr = m_Descriptor->SelectStream(0);
      ComPtr<IMFMediaTypeHandler> handler;
      ComPtr<IMFMediaType> type;
      if (SUCCEEDED(hr)) {
        hr = descriptor->GetMediaTypeHandler(&handler);
      }
      if (SUCCEEDED(hr)) {
        hr = handler->GetCurrentMediaType(&type);
      }
      if (SUCCEEDED(hr)) {
        ComPtr<IUnknown> stream;
        m_Stream->QueryInterface(IID_PPV_ARGS(stream.GetAddressOf()));
        hr = m_Queue->QueueEventParamUnk(was_selected ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, stream.Get());
      }
      if (SUCCEEDED(hr)) {
        hr = m_Stream->Start(type.Get());
      }
    } else if (SUCCEEDED(hr) && was_selected) {
      hr = m_Descriptor->DeselectStream(0);
      if (SUCCEEDED(hr)) {
        hr = m_Stream->Stop(false);
      }
    }
    if (SUCCEEDED(hr)) {
      hr = m_Queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &start_time);
    }
    PropVariantClear(&start_time);
    if (FAILED(hr)) {
      return hr;
    }
    m_State = state_e::started;
    return S_OK;
  }

  STDMETHODIMP MediaSource::Stop() {
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (m_State != state_e::started) {
      return MF_E_INVALID_STATE_TRANSITION;
    }
    m_State = state_e::stopped;
    PROPVARIANT stop_time;
    hr = InitPropVariantFromInt64(MFGetSystemTime(), &stop_time);
    if (FAILED(hr)) {
      return hr;
    }
    hr = m_Stream->Stop(true);
    if (SUCCEEDED(hr)) {
      hr = m_Descriptor->DeselectStream(0);
    }
    if (SUCCEEDED(hr)) {
      hr = m_Queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &stop_time);
    }
    PropVariantClear(&stop_time);
    return hr;
  }

  STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes **ppAttributes) {
    if (!ppAttributes) {
      return E_POINTER;
    }
    *ppAttributes = nullptr;
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    // The source's own store, as a video capture source must return
    return m_Attributes.CopyTo(ppAttributes);
  }

  STDMETHODIMP MediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes **ppAttributes) {
    if (!ppAttributes) {
      return E_POINTER;
    }
    *ppAttributes = nullptr;
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (dwStreamIdentifier != m_Stream->Id()) {
      return MF_E_NOT_FOUND;
    }
    return m_Stream->CopyAttributes(ppAttributes);
  }

  STDMETHODIMP MediaSource::SetD3DManager(IUnknown *) {
    // Best effort, and ignored: the frames are made on the CPU
    return E_NOTIMPL;
  }

  STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID *ppvObject) {
    if (!ppvObject) {
      return E_POINTER;
    }
    *ppvObject = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
  }

  STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID, ULONG, ULONG *) {
    if (!Property || PropertyLength < sizeof(KSPROPERTY)) {
      return E_INVALIDARG;
    }
    // What the driver framework answers for an operation a driver doesn't handle
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }

  STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }

  STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }

  STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown *pAllocator) {
    if (!pAllocator) {
      return E_POINTER;
    }
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (dwOutputStreamID != m_Stream->Id()) {
      return MF_E_NOT_FOUND;
    }
    ComPtr<IMFVideoSampleAllocator> allocator;
    hr = pAllocator->QueryInterface(IID_PPV_ARGS(allocator.GetAddressOf()));
    if (FAILED(hr)) {
      return hr;
    }
    return m_Stream->SetAllocator(allocator.Get());
  }

  STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD *pdwInputStreamID, MFSampleAllocatorUsage *peUsage) {
    if (!pdwInputStreamID || !peUsage) {
      return E_POINTER;
    }
    std::lock_guard lock {m_Lock};
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
      return hr;
    }
    if (dwOutputStreamID != m_Stream->Id()) {
      return MF_E_NOT_FOUND;
    }
    *pdwInputStreamID = dwOutputStreamID;
    *peUsage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
  }
}  // namespace apollo_camera

using namespace apollo_camera;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}

STDAPI DllCanUnloadNow() {
  return g_objects.load() == 0 && g_locks.load() == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
  if (!ppv) {
    return E_POINTER;
  }
  *ppv = nullptr;
  if (rclsid != k_clsid) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  auto *factory = new (std::nothrow) ClassFactory();
  if (!factory) {
    return E_OUTOFMEMORY;
  }
  HRESULT hr = factory->QueryInterface(riid, ppv);
  factory->Release();
  return hr;
}
