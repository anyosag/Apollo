/**
 * @file third-party/luzvda/luzvda.h
 * @brief Apollo's calls into the LuzVDA virtual display driver (anyosag/LuzVDA), protocol 1.x.
 *
 * luzvda-ioctl.h next to this file is the driver's own protocol header, copied byte for byte from
 * the driver release that Apollo packages; CI checks that they match.
 *
 * Each call returns 0 or the Win32 error DeviceIoControl gave.
 */
#pragma once

// standard includes
#include <cstring>
#include <memory>
#include <new>

// platform includes
#include <windows.h>
#include <setupapi.h>

// local includes
#include "luzvda-ioctl.h"

namespace LUZVDA {
  /// The driver's device interface
  inline const GUID INTERFACE_GUID = LUZVDA_INTERFACE_GUID_INIT;

  inline LUZVDA_GUID toProtocol(const GUID &guid) {
    LUZVDA_GUID out;
    static_assert(sizeof(out.bytes) == sizeof(GUID));
    std::memcpy(out.bytes, &guid, sizeof(out.bytes));
    return out;
  }

  inline LUZVDA_LUID toProtocol(const LUID &luid) {
    return {luid.LowPart, luid.HighPart};
  }

  inline LUID fromProtocol(const LUZVDA_LUID &luid) {
    LUID out;
    out.LowPart = luid.low;
    out.HighPart = luid.high;
    return out;
  }

  /**
   * @brief Opens the driver for reading and writing. Only SYSTEM and Administrators may.
   * @param error Set to why it failed, or 0.
   * @return INVALID_HANDLE_VALUE when the driver isn't running or the device can't be opened.
   */
  inline HANDLE openDevice(DWORD &error) {
    error = ERROR_FILE_NOT_FOUND;

    HDEVINFO devices = SetupDiGetClassDevsW(&INTERFACE_GUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
      error = GetLastError();
      return INVALID_HANDLE_VALUE;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA iface {};
    iface.cbSize = sizeof(iface);

    for (DWORD i = 0; handle == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(devices, nullptr, &INTERFACE_GUID, i, &iface); ++i) {
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
        continue;
      }

      // Allocated as DWORDs for the structure's alignment
      std::unique_ptr<DWORD[]> buffer(new (std::nothrow) DWORD[(size + sizeof(DWORD) - 1) / sizeof(DWORD)]);
      if (!buffer) {
        continue;
      }
      auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.get());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, size, nullptr, nullptr)) {
        continue;
      }

      // Synchronous: every request completes before DeviceIoControl returns
      handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      error = handle == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    }

    SetupDiDestroyDeviceInfoList(devices);
    return handle;
  }

  inline DWORD control(HANDLE device, DWORD code, const void *in, DWORD inSize, void *out, DWORD outSize) {
    DWORD returned = 0;
    if (!DeviceIoControl(device, code, const_cast<void *>(in), inSize, out, outSize, &returned, nullptr)) {
      return GetLastError();
    }
    // A reply shorter than its structure would leave part of it unset
    if (out && returned < outSize) {
      return ERROR_INVALID_DATA;
    }
    return 0;
  }

  inline DWORD getVersion(HANDLE device, LUZVDA_VERSION &out) {
    return control(device, IOCTL_LUZVDA_GET_VERSION, nullptr, 0, &out, sizeof(out));
  }

  /// The driver speaks this protocol: same major version, and at least the minor one Apollo needs
  inline bool isCompatible(const LUZVDA_VERSION &version) {
    // Through a variable, as comparing with minor version 0 directly is always true and warns
    constexpr uint32_t neededMinor = LUZVDA_PROTOCOL_MINOR;
    return version.major == LUZVDA_PROTOCOL_MAJOR && version.minor >= neededMinor;
  }

  inline DWORD ping(HANDLE device) {
    return control(device, IOCTL_LUZVDA_PING, nullptr, 0, nullptr, 0);
  }

  inline DWORD getWatchdog(HANDLE device, LUZVDA_WATCHDOG &out) {
    return control(device, IOCTL_LUZVDA_GET_WATCHDOG, nullptr, 0, &out, sizeof(out));
  }

  /// Returns the display already connected with this GUID if its description is the same, and fails
  /// if it differs (the driver's STATUS_OBJECT_NAME_COLLISION)
  inline DWORD addDisplay(HANDLE device, const LUZVDA_DISPLAY_DESC &desc, LUZVDA_ADD_OUT &out) {
    return control(device, IOCTL_LUZVDA_ADD_DISPLAY, &desc, sizeof(desc), &out, sizeof(out));
  }

  inline DWORD removeDisplay(HANDLE device, const LUZVDA_GUID &guid) {
    return control(device, IOCTL_LUZVDA_REMOVE_DISPLAY, &guid, sizeof(guid), nullptr, 0);
  }

  inline DWORD updateModes(HANDLE device, const LUZVDA_UPDATE_MODES &update) {
    return control(device, IOCTL_LUZVDA_UPDATE_MODES, &update, sizeof(update), nullptr, 0);
  }

  inline DWORD setRenderAdapter(HANDLE device, const LUID &adapter) {
    auto luid = toProtocol(adapter);
    return control(device, IOCTL_LUZVDA_SET_RENDER_ADAPTER, &luid, sizeof(luid), nullptr, 0);
  }

  inline DWORD listDisplays(HANDLE device, LUZVDA_LIST &out) {
    out = {};
    DWORD error = control(device, IOCTL_LUZVDA_LIST_DISPLAYS, nullptr, 0, &out, sizeof(out));
    if (!error && out.count > LUZVDA_MAX_DISPLAYS) {
      return ERROR_INVALID_DATA;
    }
    return error;
  }
}  // namespace LUZVDA
