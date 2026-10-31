// Mac Camera's media source: the COM object Windows' Frame Server loads for the virtual camera that
// tools/apollo-camera.cpp registers (src/fork/camera.h). It offers NV12 at 1280x720 and 1920x1080,
// 30 fps, and serves the newest frame Apollo put in the shared memory (src/fork/camera_policy.h),
// or its own camera icon while Apollo isn't there.
//
// Built with Visual Studio and the Windows SDK (scripts/fork/build-camera.ps1). Its structure follows
// Microsoft's VirtualCamera sample (github.com/microsoft/Windows-Camera, Samples/VirtualCamera,
// VirtualCameraMediaSource: SimpleMediaSource, SimpleMediaStream, VirtualCameraMediaSourceActivate;
// MIT, Copyright (c) Microsoft Corporation), with plain COM objects instead of C++/WinRT and WIL.
#pragma once

// platform includes, in the sample's order
#include <unknwn.h>
#include <windows.h>
#include <propvarutil.h>
#include <ole2.h>
#include <initguid.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <wrl/client.h>

// standard includes
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

// local includes
#include "src/fork/camera_policy.h"

namespace apollo_camera {
  using Microsoft::WRL::ComPtr;

  // camera_policy::k_source_clsid: {5C0E6C3D-1E7A-4B8F-9A52-4C55A8F0C1A1}
  inline constexpr CLSID k_clsid = {0x5c0e6c3d, 0x1e7a, 0x4b8f, {0x9a, 0x52, 0x4c, 0x55, 0xa8, 0xf0, 0xc1, 0xa1}};

  /// COM objects alive and IClassFactory::LockServer locks, for DllCanUnloadNow
  extern std::atomic<long> g_objects;
  extern std::atomic<long> g_locks;

  /// The DLL itself
  extern HMODULE g_module;

  class MediaSource;

  /**
   * @brief The camera's one stream (SimpleMediaStream). Samples come from a thread of the stream's
   * own while it runs: RequestSample only queues the request, and the thread answers it with the
   * newest frame from Apollo as soon as there is one, or the same frame again after a frame's time.
   */
  class MediaStream final: public IMFMediaStream2 {
  public:
    MediaStream();
    ~MediaStream();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT *pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource **ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor **ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown *pToken) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE *pState) override;

    HRESULT Initialize(IMFMediaSource *source, DWORD id);
    HRESULT Start(IMFMediaType *type);
    HRESULT Stop(bool send_event);
    HRESULT Shutdown();
    HRESULT SetAllocator(IMFVideoSampleAllocator *allocator);
    HRESULT CopyAttributes(IMFAttributes **attributes);

    DWORD Id() const {
      return m_Id;
    }

  private:
    HRESULT CheckShutdown() const;
    HRESULT StartLocked(bool send_event, IMFMediaType *type, std::unique_lock<std::mutex> &lock);
    HRESULT StopLocked(bool send_event, std::unique_lock<std::mutex> &lock);
    HRESULT SetStreamAttributes(IMFAttributes *attributes);

    /// The thread that answers requests while the stream runs
    void Pump(std::shared_ptr<std::atomic_bool> stop, camera_policy::frame_size_t size);

    /// Answers the oldest request with a frame; false when there's none or no sample to put it in
    bool Deliver(std::uint8_t *frames, std::uint64_t now_ms);

    std::atomic<ULONG> m_Refs {1};
    std::mutex m_Lock;
    DWORD m_Id = 0;
    bool m_Shutdown = false;
    MF_STREAM_STATE m_State = MF_STREAM_STATE_STOPPED;
    ComPtr<IMFMediaSource> m_Parent;  // a cycle with the source, broken by Shutdown(), as in the sample
    ComPtr<IMFMediaEventQueue> m_Queue;
    ComPtr<IMFAttributes> m_Attributes;
    ComPtr<IMFStreamDescriptor> m_Descriptor;
    ComPtr<IMFVideoSampleAllocator> m_Allocator;
    bool m_OwnAllocator = false;  // made here, as nobody gave one
    ComPtr<IMFMediaType> m_Type;
    camera_policy::frame_size_t m_Size;
    std::deque<ComPtr<IUnknown>> m_Requests;  // tokens, null for a request without one
    HANDLE m_Wake = nullptr;  // a request came, or the pump should stop
    std::thread m_Pump;
    std::shared_ptr<std::atomic_bool> m_PumpStop;
  };

  /**
   * @brief The camera's media source (SimpleMediaSource): one stream, live, with the legacy sensor
   * profile, no controls (IKsControl answers ERROR_SET_NOT_FOUND, like a driver without them), and
   * Frame Server's sample allocator.
   */
  class MediaSource final: public IMFMediaSourceEx, public IMFGetService, public IKsControl, public IMFSampleAllocatorControl {
  public:
    MediaSource();
    ~MediaSource();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT *pvValue) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor **ppPresentationDescriptor) override;
    STDMETHODIMP GetCharacteristics(DWORD *pdwCharacteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor *pPresentationDescriptor, const GUID *pguidTimeFormat, const PROPVARIANT *pvarStartPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes **ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes **ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown *pManager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG *BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG *BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG *BytesReturned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown *pAllocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD dwOutputStreamID, DWORD *pdwInputStreamID, MFSampleAllocatorUsage *peUsage) override;

    HRESULT Initialize(IMFAttributes *activate_attributes);

  private:
    enum class state_e {
      invalid,
      stopped,
      started,
      shutdown,
    };

    HRESULT CheckShutdown() const;
    HRESULT ValidatePresentationDescriptor(IMFPresentationDescriptor *descriptor);
    HRESULT AddSensorProfiles();

    std::atomic<ULONG> m_Refs {1};
    std::mutex m_Lock;
    state_e m_State = state_e::invalid;
    ComPtr<IMFMediaEventQueue> m_Queue;
    ComPtr<IMFAttributes> m_Attributes;
    ComPtr<IMFPresentationDescriptor> m_Descriptor;
    ComPtr<MediaStream> m_Stream;
  };

  /**
   * @brief What COM creates for the camera's CLSID (VirtualCameraMediaSourceActivate): an
   * IMFActivate whose attributes Frame Server fills in, and that makes the source when activated.
   */
  class Activate final: public IMFActivate {
  public:
    Activate();
    ~Activate();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFAttributes, kept in an attribute store of Media Foundation's
    STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT *pValue) override;
    STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE *pType) override;
    STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL *pbResult) override;
    STDMETHODIMP Compare(IMFAttributes *pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL *pbResult) override;
    STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32 *punValue) override;
    STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64 *punValue) override;
    STDMETHODIMP GetDouble(REFGUID guidKey, double *pfValue) override;
    STDMETHODIMP GetGUID(REFGUID guidKey, GUID *pguidValue) override;
    STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32 *pcchLength) override;
    STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32 *pcchLength) override;
    STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR *ppwszValue, UINT32 *pcchLength) override;
    STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32 *pcbBlobSize) override;
    STDMETHODIMP GetBlob(REFGUID guidKey, UINT8 *pBuf, UINT32 cbBufSize, UINT32 *pcbBlobSize) override;
    STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8 **ppBuf, UINT32 *pcbSize) override;
    STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID *ppv) override;
    STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override;
    STDMETHODIMP DeleteItem(REFGUID guidKey) override;
    STDMETHODIMP DeleteAllItems() override;
    STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override;
    STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override;
    STDMETHODIMP SetDouble(REFGUID guidKey, double fValue) override;
    STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override;
    STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR wszValue) override;
    STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8 *pBuf, UINT32 cbBufSize) override;
    STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown *pUnknown) override;
    STDMETHODIMP LockStore() override;
    STDMETHODIMP UnlockStore() override;
    STDMETHODIMP GetCount(UINT32 *pcItems) override;
    STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID *pguidKey, PROPVARIANT *pValue) override;
    STDMETHODIMP CopyAllItems(IMFAttributes *pDest) override;

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void **ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    HRESULT Initialize();

  private:
    std::atomic<ULONG> m_Refs {1};
    std::mutex m_Lock;
    ComPtr<IMFAttributes> m_Attributes;
    ComPtr<MediaSource> m_Source;
  };
}  // namespace apollo_camera
