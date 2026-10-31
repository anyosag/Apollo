// Registers Mac Camera, the virtual camera whose media source is tools/apollo-camera-source (src/fork/camera.h),
// with Windows 11, and removes it. The installer runs it, and Apollo at each start, as SYSTEM.
//   apollo-camera.exe register     COM registration of this build's source DLL, and the camera (system lifetime)
//   apollo-camera.exe unregister   the camera and the COM registration go
//   apollo-camera.exe selftest     the source in this process, as Frame Server would use it, without Apollo
// Each line it prints is "<level>: <message>", which Apollo copies into its log (user_helper::log_line).
// Exit codes: 0 done, 1 failed, 2 this Windows has no virtual cameras (before Windows 11), 77 test skipped.
//
// Built with Visual Studio and the Windows SDK (scripts/fork/build-camera.ps1), which names the source DLL
// in camera_build.h. After Microsoft's VirtualCamera sample (VirtualCameraManager_WinRT's registrar and
// VirtualCameraTest's VCamUtils; MIT, Copyright (c) Microsoft Corporation).
#include <windows.h>
#include <aclapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "camera_build.h"  // APOLLO_CAMERA_SOURCE_DLL
#include "src/fork/camera_policy.h"

using Microsoft::WRL::ComPtr;

namespace {
  // camera_policy::k_source_clsid
  constexpr CLSID k_clsid = {0x5c0e6c3d, 0x1e7a, 0x4b8f, {0x9a, 0x52, 0x4c, 0x55, 0xa8, 0xf0, 0xc1, 0xa1}};

  // DEVPKEY_DeviceInterface_VCamCreate_SourceId, _FriendlyName, _Lifetime and _Access, as the sample's
  // VCamUtils names them: what a camera was made with, which this tool stores on it to find it again
  constexpr GUID k_vcam_create = {0x6ac1fbf7, 0x45f7, 0x4e06, {0xbd, 0xa7, 0xf8, 0x17, 0xeb, 0xfa, 0x04, 0xd1}};
  constexpr DEVPROPKEY k_vcam_source_id = {k_vcam_create, 4};
  constexpr DEVPROPKEY k_vcam_friendly_name = {k_vcam_create, 5};
  constexpr DEVPROPKEY k_vcam_lifetime = {k_vcam_create, 6};
  constexpr DEVPROPKEY k_vcam_access = {k_vcam_create, 7};

  void say(const char *level, const std::wstring &message) {
    int size = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int) message.size(), nullptr, 0, nullptr, nullptr);
    std::string text(size > 0 ? size : 0, '\0');
    if (size > 0) {
      WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int) message.size(), text.data(), size, nullptr, nullptr);
    }
    std::printf("%s: %s\n", level, text.c_str());
    std::fflush(stdout);
  }

  std::wstring hex(HRESULT hr) {
    wchar_t text[16];
    swprintf_s(text, L"0x%08X", (unsigned) hr);
    return text;
  }

  std::filesystem::path exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
  }

  std::filesystem::path source_dll() {
    return exe_dir() / APOLLO_CAMERA_SOURCE_DLL;
  }

  /// MFCreateVirtualCamera, Windows 11's (mfsensorgroup.dll), looked up so this runs on Windows 10 too
  decltype(&MFCreateVirtualCamera) create_virtual_camera() {
    HMODULE sensors = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return sensors ? reinterpret_cast<decltype(&MFCreateVirtualCamera)>(GetProcAddress(sensors, "MFCreateVirtualCamera")) : nullptr;
  }

  /// Frame Server (LOCAL SERVICE) and apps that load the source in their own process (packaged apps)
  /// read the DLL; Apollo's folder only lets SYSTEM, Administrators and Users in
  bool grant_read(const std::filesystem::path &file) {
    PACL old_acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl, nullptr, &descriptor) != ERROR_SUCCESS) {
      return false;
    }
    BYTE local_service[SECURITY_MAX_SID_SIZE];
    BYTE packages[SECURITY_MAX_SID_SIZE];
    DWORD size1 = sizeof(local_service);
    DWORD size2 = sizeof(packages);
    bool ok = CreateWellKnownSid(WinLocalServiceSid, nullptr, local_service, &size1) && CreateWellKnownSid(WinBuiltinAnyPackageSid, nullptr, packages, &size2);
    PACL new_acl = nullptr;
    if (ok) {
      EXPLICIT_ACCESSW access[2] {};
      for (int i = 0; i < 2; i++) {
        access[i].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        access[i].grfAccessMode = SET_ACCESS;
        access[i].grfInheritance = NO_INHERITANCE;
        access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[i].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
      }
      access[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(local_service);
      access[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(packages);
      ok = SetEntriesInAclW(2, access, old_acl, &new_acl) == ERROR_SUCCESS &&
           SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_acl, nullptr) == ERROR_SUCCESS;
    }
    if (new_acl) {
      LocalFree(new_acl);
    }
    LocalFree(descriptor);
    return ok;
  }

  const std::wstring k_clsid_key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + camera_policy::k_source_clsid;

  /// The source's COM registration, in HKLM: Frame Server and its monitor load it there
  bool register_com(const std::filesystem::path &dll) {
    HKEY key = nullptr;
    std::wstring server_key = k_clsid_key + L"\\InprocServer32";
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, server_key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
      return false;
    }
    std::wstring path = dll.wstring();
    const wchar_t threading[] = L"Both";
    bool ok = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(path.c_str()), (DWORD) ((path.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS &&
              RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE *>(threading), sizeof(threading)) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (ok && RegCreateKeyExW(HKEY_LOCAL_MACHINE, k_clsid_key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
      const wchar_t name[] = L"Mac Camera (Apollo)";
      RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(name), sizeof(name));
      RegCloseKey(key);
    }
    return ok;
  }

  /// Older builds' source DLLs, which Frame Server may still hold; those it holds go next time
  void remove_old_dlls(const std::filesystem::path &current) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(current.parent_path(), ec)) {
      auto name = entry.path().filename().wstring();
      if (name != current.filename().wstring() && name.rfind(L"apollo-camera-source-", 0) == 0 && entry.path().extension() == L".dll") {
        std::filesystem::remove(entry.path(), ec);
      }
    }
  }

  /// Present cameras made from this source: the creation properties this tool adds, as the sample's do
  int count_cameras() {
    ULONG size = 0;
    GUID category = KSCATEGORY_VIDEO_CAMERA;
    if (CM_Get_Device_Interface_List_SizeW(&size, &category, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || size <= 1) {
      return 0;
    }
    std::vector<wchar_t> list(size);
    if (CM_Get_Device_Interface_ListW(&category, nullptr, list.data(), size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
      return 0;
    }
    int found = 0;
    for (const wchar_t *iface = list.data(); *iface; iface += wcslen(iface) + 1) {
      DEVPROPTYPE type = 0;
      wchar_t value[64] {};
      ULONG bytes = sizeof(value);
      if (CM_Get_Device_Interface_PropertyW(iface, &k_vcam_source_id, &type, reinterpret_cast<BYTE *>(value), &bytes, 0) == CR_SUCCESS &&
          type == DEVPROP_TYPE_STRING && _wcsicmp(value, camera_policy::k_source_clsid) == 0) {
        found++;
      }
    }
    return found;
  }

  HRESULT make_camera(decltype(&MFCreateVirtualCamera) create, IMFVirtualCamera **camera) {
    return create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers,
                  camera_policy::k_friendly_name, camera_policy::k_source_clsid, nullptr, 0, camera);
  }

  int do_register() {
    auto dll = source_dll();
    if (!std::filesystem::exists(dll)) {
      say("error", L"the camera's source isn't here: " + dll.wstring());
      return 1;
    }
    auto create = create_virtual_camera();
    if (!create) {
      say("info", L"this Windows has no virtual cameras (they came with Windows 11)");
      return 2;
    }
    if (!grant_read(dll)) {
      say("warning", L"couldn't let Frame Server read " + dll.filename().wstring() + L": error " + std::to_wstring(GetLastError()));
    }
    if (!register_com(dll)) {
      say("error", L"couldn't register the camera's source in HKLM (needs an administrator or SYSTEM)");
      return 1;
    }
    remove_old_dlls(dll);
    if (count_cameras() > 0) {
      say("info", L"Mac Camera is registered, with " + dll.filename().wstring());
      return 0;
    }

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
      say("error", L"Media Foundation didn't start: " + hex(hr));
      return 1;
    }
    ComPtr<IMFVirtualCamera> camera;
    hr = make_camera(create, &camera);
    if (SUCCEEDED(hr)) {
      // What it was made with, for finding it again (count_cameras()), as the sample's VCamUtils does
      std::wstring source = camera_policy::k_source_clsid;
      std::wstring name = camera_policy::k_friendly_name;
      INT32 lifetime = MFVirtualCameraLifetime_System;
      INT32 access = MFVirtualCameraAccess_AllUsers;
      camera->AddProperty(&k_vcam_source_id, DEVPROP_TYPE_STRING, reinterpret_cast<const BYTE *>(source.c_str()), (ULONG) ((source.size() + 1) * sizeof(wchar_t)));
      camera->AddProperty(&k_vcam_friendly_name, DEVPROP_TYPE_STRING, reinterpret_cast<const BYTE *>(name.c_str()), (ULONG) ((name.size() + 1) * sizeof(wchar_t)));
      camera->AddProperty(&k_vcam_lifetime, DEVPROP_TYPE_INT32, reinterpret_cast<const BYTE *>(&lifetime), sizeof(lifetime));
      camera->AddProperty(&k_vcam_access, DEVPROP_TYPE_INT32, reinterpret_cast<const BYTE *>(&access), sizeof(access));
      // Registered only now; with the system lifetime, it stays after this program ends
      hr = camera->Start(nullptr);
    }
    camera.Reset();
    MFShutdown();
    if (FAILED(hr)) {
      say("error", L"couldn't create Mac Camera: " + hex(hr));
      return 1;
    }
    say("info", L"Mac Camera created, with " + dll.filename().wstring());
    return 0;
  }

  int do_unregister() {
    auto create = create_virtual_camera();
    int code = 0;
    if (create && SUCCEEDED(MFStartup(MF_VERSION))) {
      // The same parameters find the camera made with them, which then goes (as the sample removes
      // one); once even if it isn't present now
      for (int attempt = 0; attempt < 4 && (attempt == 0 || count_cameras() > 0); attempt++) {
        ComPtr<IMFVirtualCamera> camera;
        HRESULT hr = make_camera(create, &camera);
        if (SUCCEEDED(hr)) {
          hr = camera->Remove();
        }
        if (FAILED(hr) && count_cameras() > 0) {
          say("warning", L"couldn't remove Mac Camera: " + hex(hr));
          code = 1;
          break;
        }
      }
      MFShutdown();
      if (code == 0) {
        say("info", L"Mac Camera removed");
      }
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, k_clsid_key.c_str());
    return code;
  }

  bool expect(bool ok, const wchar_t *what) {
    say(ok ? "info" : "error", std::wstring(ok ? L"ok: " : L"FAILED: ") + what);
    return ok;
  }

  /// The next event of an event generator within timeout_ms
  HRESULT next_event(IMFMediaEventGenerator *generator, IMFMediaEvent **event, DWORD timeout_ms) {
    for (DWORD waited = 0;; waited += 10) {
      HRESULT hr = generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, event);
      if (hr != MF_E_NO_EVENTS_AVAILABLE || waited >= timeout_ms) {
        return hr;
      }
      Sleep(10);
    }
  }

  /// One size through the source: start it, three samples of the camera's own icon, stop
  bool test_size(IMFMediaSource *source, DWORD type_index, camera_policy::frame_size_t size) {
    ComPtr<IMFPresentationDescriptor> presentation;
    ComPtr<IMFStreamDescriptor> stream_descriptor;
    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> type;
    BOOL selected = FALSE;
    bool ok = SUCCEEDED(source->CreatePresentationDescriptor(&presentation)) &&
              SUCCEEDED(presentation->GetStreamDescriptorByIndex(0, &selected, &stream_descriptor)) &&
              SUCCEEDED(stream_descriptor->GetMediaTypeHandler(&handler)) &&
              SUCCEEDED(handler->GetMediaTypeByIndex(type_index, &type)) &&
              SUCCEEDED(handler->SetCurrentMediaType(type.Get())) &&
              SUCCEEDED(presentation->SelectStream(0));
    if (!expect(ok, L"a presentation with the stream selected")) {
      return false;
    }
    PROPVARIANT start;
    PropVariantInit(&start);
    if (!expect(SUCCEEDED(source->Start(presentation.Get(), nullptr, &start)), L"Start")) {
      return false;
    }

    // MENewStream (or MEUpdatedStream) with the stream, then MESourceStarted
    ComPtr<IMFMediaStream> stream;
    bool started = false;
    for (int i = 0; i < 4 && !(stream && started); i++) {
      ComPtr<IMFMediaEvent> event;
      if (FAILED(next_event(source, &event, 2000))) {
        break;
      }
      MediaEventType met = MEUnknown;
      event->GetType(&met);
      if (met == MENewStream || met == MEUpdatedStream) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal) {
          value.punkVal->QueryInterface(IID_PPV_ARGS(stream.GetAddressOf()));
        }
        PropVariantClear(&value);
      } else if (met == MESourceStarted) {
        started = true;
      }
    }
    if (!expect(stream && started, L"MENewStream and MESourceStarted")) {
      return false;
    }

    int samples = 0;
    for (int i = 0; i < 3; i++) {
      if (FAILED(stream->RequestSample(nullptr))) {
        break;
      }
    }
    for (int i = 0; i < 8 && samples < 3; i++) {
      ComPtr<IMFMediaEvent> event;
      if (FAILED(next_event(stream.Get(), &event, 2000))) {
        break;
      }
      MediaEventType met = MEUnknown;
      event->GetType(&met);
      if (met != MEMediaSample) {
        continue;
      }
      PROPVARIANT value;
      PropVariantInit(&value);
      ComPtr<IMFSample> sample;
      if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal) {
        value.punkVal->QueryInterface(IID_PPV_ARGS(sample.GetAddressOf()));
      }
      PropVariantClear(&value);
      ComPtr<IMFMediaBuffer> buffer;
      BYTE *data = nullptr;
      DWORD length = 0;
      if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
        // The icon's lens, in the middle a little up, on the dark background
        std::size_t center = (std::size_t) (size.height * 2 / 5) * size.width + size.width / 2;
        bool icon = length >= camera_policy::nv12_bytes(size.width, size.height) && data[center] == camera_policy::nv12::k_icon.y && data[0] == camera_policy::nv12::k_background.y;
        buffer->Unlock();
        if (icon) {
          samples++;
        }
      }
    }
    expect(samples == 3, L"three samples with the camera's icon");
    bool stopped = SUCCEEDED(source->Stop());
    expect(stopped, L"Stop");
    return samples == 3 && stopped;
  }

  int do_selftest() {
    if (FAILED(MFStartup(MF_VERSION))) {
      say("warning", L"skipped: Media Foundation isn't on this Windows");
      return 77;
    }
    bool ok = true;
    HMODULE module = LoadLibraryW(source_dll().c_str());
    ok = expect(module != nullptr, L"the source DLL loads") && ok;
    auto get_class_object = module ? reinterpret_cast<HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *)>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    ComPtr<IMFActivate> activate;
    ComPtr<IMFMediaSource> source;
    if (get_class_object && SUCCEEDED(get_class_object(k_clsid, IID_PPV_ARGS(factory.GetAddressOf())))) {
      factory->CreateInstance(nullptr, IID_PPV_ARGS(activate.GetAddressOf()));
    }
    ok = expect(activate != nullptr, L"its class factory makes the activate object") && ok;
    if (activate) {
      activate->ActivateObject(IID_PPV_ARGS(source.GetAddressOf()));
    }
    ok = expect(source != nullptr, L"the activate object makes the media source") && ok;
    if (source) {
      DWORD characteristics = 0;
      ok = expect(SUCCEEDED(source->GetCharacteristics(&characteristics)) && (characteristics & MFMEDIASOURCE_IS_LIVE), L"a live source") && ok;
      ComPtr<IMFMediaSourceEx> source_ex;
      ComPtr<IKsControl> ks;
      ComPtr<IMFSampleAllocatorControl> allocator_control;
      ComPtr<IMFAttributes> stream_attributes;
      ComPtr<IMFAttributes> source_attributes;
      bool interfaces = SUCCEEDED(source.As(&source_ex)) && SUCCEEDED(source.As(&ks)) && SUCCEEDED(source.As(&allocator_control)) &&
                        SUCCEEDED(source_ex->GetStreamAttributes(0, &stream_attributes)) && SUCCEEDED(source_ex->GetSourceAttributes(&source_attributes));
      ok = expect(interfaces, L"IMFMediaSourceEx, IKsControl and IMFSampleAllocatorControl, and the attributes") && ok;
      if (interfaces) {
        GUID category = GUID_NULL;
        stream_attributes->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category);
        ok = expect(category == PINNAME_VIDEO_CAPTURE && MFGetAttributeUINT32(stream_attributes.Get(), MF_DEVICESTREAM_FRAMESERVER_SHARED, 0) == 1, L"a shared video capture stream") && ok;
        ComPtr<IUnknown> profiles;
        bool has_profiles = SUCCEEDED(source_attributes->GetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, IID_PPV_ARGS(profiles.GetAddressOf())));
        say(has_profiles ? "info" : "warning", has_profiles ? L"ok: the legacy sensor profile" : L"no sensor profiles (no mfsensorgroup.dll here)");
      }

      // The media types: NV12 at 1280x720 and 1920x1080, 30 fps
      ComPtr<IMFPresentationDescriptor> presentation;
      ComPtr<IMFStreamDescriptor> stream_descriptor;
      ComPtr<IMFMediaTypeHandler> handler;
      DWORD types = 0;
      BOOL selected = FALSE;
      bool listed = SUCCEEDED(source->CreatePresentationDescriptor(&presentation)) && SUCCEEDED(presentation->GetStreamDescriptorByIndex(0, &selected, &stream_descriptor)) &&
                    SUCCEEDED(stream_descriptor->GetMediaTypeHandler(&handler)) && SUCCEEDED(handler->GetMediaTypeCount(&types)) && types == std::size(camera_policy::k_sizes);
      for (DWORD i = 0; listed && i < types; i++) {
        ComPtr<IMFMediaType> type;
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 numerator = 0;
        UINT32 denominator = 0;
        GUID subtype = GUID_NULL;
        listed = SUCCEEDED(handler->GetMediaTypeByIndex(i, &type)) && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12 &&
                 SUCCEEDED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) && width == camera_policy::k_sizes[i].width && height == camera_policy::k_sizes[i].height &&
                 SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) && numerator == camera_policy::k_fps && denominator == 1;
      }
      ok = expect(listed, L"NV12 at 1280x720 and 1920x1080, 30 fps") && ok;
      if (listed) {
        for (DWORD i = 0; i < types; i++) {
          ok = test_size(source.Get(), i, camera_policy::k_sizes[i]) && ok;
        }
      }
      ok = expect(SUCCEEDED(source->Shutdown()), L"Shutdown") && ok;
    }
    source.Reset();
    if (activate) {
      activate->ShutdownObject();
      activate->DetachObject();
    }
    activate.Reset();
    factory.Reset();
    MFShutdown();
    say(ok ? "info" : "error", ok ? L"the camera's source works" : L"the camera's source failed its test");
    return ok ? 0 : 1;
  }
}  // namespace

int wmain(int argc, wchar_t *argv[]) {
  std::wstring command = argc > 1 ? argv[1] : L"";
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (command == L"register") {
    return do_register();
  }
  if (command == L"unregister") {
    return do_unregister();
  }
  if (command == L"selftest") {
    return do_selftest();
  }
  say("error", L"usage: apollo-camera.exe register | unregister | selftest");
  return 1;
}
