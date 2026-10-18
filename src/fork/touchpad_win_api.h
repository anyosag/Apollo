/**
 * @file src/fork/touchpad_win_api.h
 * @brief Windows 11's synthetic touchpad API, which neither MinGW-w64's headers nor the Windows
 * SDK declare yet.
 *
 * `CreateSyntheticPointerDevice2` takes the device's physical size, which touchpads need (the
 * first version can't make them). Microsoft documents it as pre-release:
 * https://learn.microsoft.com/windows/win32/input-precisiontouchpad/createsyntheticpointerdevice2
 * The layout below follows that page. The functions are looked up by name in user32.dll, never
 * by ordinal: an ordinal can name another function in another Windows build.
 */
#pragma once

#ifdef _WIN32

// standard includes
  #include <cstddef>

// platform includes
  #ifndef WINVER
    // The synthetic pointer API is Windows 10 and later
    #define WINVER 0x0A00
  #endif
  #include <windows.h>

namespace fork_touchpad::win {
  /// `PT_TOUCHPAD`
  constexpr POINTER_INPUT_TYPE pt_touchpad = 5;

  /// `SYNTHETIC_DEVICE_CREATION_OPTIONS`
  enum creation_option_e : ULONG {
    sdco_none = 0x0,
    sdco_physical_size = 0x1,  ///< Positions are on the device (ptHimetricLocation), not the screen
    sdco_touchpad_gesture_only = 0x2,  ///< Never move the pointer or click; not used, so input acts like a real touchpad
  };

  /// `SYNTHETIC_DEVICE_CREATION_PARAMS`
  struct creation_params_t {
    POINTER_INPUT_TYPE pointerType;
    ULONG maxCount;
    POINTER_FEEDBACK_MODE feedbackMode;
    HMONITOR hMonitor;
    ULONG deviceWidth;  ///< Himetric (1/100 mm)
    ULONG deviceHeight;
    ULONG options;  ///< creation_option_e flags
  };

  #ifdef _WIN64
  static_assert(sizeof(creation_params_t) == 40, "SYNTHETIC_DEVICE_CREATION_PARAMS is 40 bytes on x64");
  static_assert(offsetof(creation_params_t, hMonitor) == 16, "hMonitor follows three 32-bit fields and padding");
  static_assert(offsetof(creation_params_t, options) == 32, "options follows the two sizes");
  #endif

  using create_fn = HSYNTHETICPOINTERDEVICE(WINAPI *)(creation_params_t *params);
  using inject_fn = BOOL(WINAPI *)(HSYNTHETICPOINTERDEVICE device, const POINTER_TYPE_INFO *pointerInfo, UINT32 count);
  using destroy_fn = VOID(WINAPI *)(HSYNTHETICPOINTERDEVICE device);

  struct api_t {
    create_fn create = nullptr;
    inject_fn inject = nullptr;
    destroy_fn destroy = nullptr;

    explicit operator bool() const {
      return create != nullptr && inject != nullptr && destroy != nullptr;
    }
  };

  /// The functions from user32.dll, looked up once; empty on Windows without them
  inline const api_t &api() {
    static const api_t loaded = [] {
      api_t a;
      HMODULE user32 = GetModuleHandleW(L"user32.dll");
      if (user32 != nullptr) {
        a.create = reinterpret_cast<create_fn>(reinterpret_cast<void *>(GetProcAddress(user32, "CreateSyntheticPointerDevice2")));
        a.inject = reinterpret_cast<inject_fn>(reinterpret_cast<void *>(GetProcAddress(user32, "InjectSyntheticPointerInput")));
        a.destroy = reinterpret_cast<destroy_fn>(reinterpret_cast<void *>(GetProcAddress(user32, "DestroySyntheticPointerDevice")));
      }
      return a;
    }();
    return loaded;
  }

  /**
   * One touchpad contact for InjectSyntheticPointerInput. Microsoft's touchpad sample sets dwTime
   * and leaves PerformanceCount at zero; @p performance_count is there for the smoke test's
   * timing variants.
   */
  inline POINTER_TYPE_INFO touchpad_contact(UINT32 pointer_id, POINTER_FLAGS flags, int button_change, LONG x, LONG y, DWORD time, UINT64 performance_count = 0) {
    POINTER_TYPE_INFO p {};
    p.type = pt_touchpad;
    auto &pointer = p.touchInfo.pointerInfo;
    pointer.pointerType = pt_touchpad;
    pointer.pointerId = pointer_id;
    pointer.pointerFlags = flags;
    pointer.ptHimetricLocation.x = x;
    pointer.ptHimetricLocation.y = y;
    pointer.ButtonChangeType = (POINTER_BUTTON_CHANGE_TYPE) button_change;
    pointer.dwTime = time;
    pointer.PerformanceCount = performance_count;
    p.touchInfo.touchMask = TOUCH_MASK_NONE;
    return p;
  }

  /// The parameters for a touchpad of 5 contacts, @p width by @p height himetric
  inline creation_params_t touchpad_params(ULONG width, ULONG height) {
    creation_params_t p {};
    p.pointerType = pt_touchpad;
    p.maxCount = 5;
    p.feedbackMode = POINTER_FEEDBACK_NONE;
    p.hMonitor = nullptr;  // A touchpad follows the mouse pointer, on whichever display it is
    p.deviceWidth = width;
    p.deviceHeight = height;
    p.options = sdco_physical_size;
    return p;
  }
}  // namespace fork_touchpad::win

#endif
