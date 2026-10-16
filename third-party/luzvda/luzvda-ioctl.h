/*
 * LuzVDA control interface, protocol 1.0.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 LuzVDA contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 * associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Written for LuzVDA, not derived from Microsoft's sample, so that programs under other licenses can
 * include it. It needs nothing but <stdint.h>, and builds as C or C++ with MSVC, MinGW and GCC.
 *
 * Every request is METHOD_BUFFERED. The device opens only for SYSTEM and Administrators (the INF's
 * Security setting), and requests that change something also need the handle opened for reading and
 * writing. Each structure's size is fixed by the static asserts at the end; a request whose buffer is
 * smaller fails with STATUS_BUFFER_TOO_SMALL, and one whose struct_size is wrong with
 * STATUS_REVISION_MISMATCH.
 */
#ifndef LUZVDA_IOCTL_H
#define LUZVDA_IOCTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CTL_CODE(FILE_DEVICE_UNKNOWN, function, METHOD_BUFFERED, access), spelled out so this header needs
 * no Windows headers */
#define LUZVDA_ACCESS_READ 1u
#define LUZVDA_ACCESS_READ_WRITE 3u
#define LUZVDA_CTL_CODE(function, access) ((0x22u << 16) | ((access) << 14) | ((function) << 2))

/* Out: LUZVDA_VERSION */
#define IOCTL_LUZVDA_GET_VERSION LUZVDA_CTL_CODE(0x8FF, LUZVDA_ACCESS_READ)
/* Resets the watchdog. No buffers */
#define IOCTL_LUZVDA_PING LUZVDA_CTL_CODE(0x888, LUZVDA_ACCESS_READ_WRITE)
/* Out: LUZVDA_WATCHDOG */
#define IOCTL_LUZVDA_GET_WATCHDOG LUZVDA_CTL_CODE(0x803, LUZVDA_ACCESS_READ)
/* In: LUZVDA_DISPLAY_DESC. Out: LUZVDA_ADD_OUT. Adding a GUID that is already connected returns
 * that display if the description is identical, and STATUS_OBJECT_NAME_COLLISION if it isn't */
#define IOCTL_LUZVDA_ADD_DISPLAY LUZVDA_CTL_CODE(0x810, LUZVDA_ACCESS_READ_WRITE)
/* In: LUZVDA_GUID. STATUS_NOT_FOUND if no display has it */
#define IOCTL_LUZVDA_REMOVE_DISPLAY LUZVDA_CTL_CODE(0x811, LUZVDA_ACCESS_READ_WRITE)
/* In: LUZVDA_UPDATE_MODES. Replaces a display's target modes (IddCxMonitorUpdateModes2). Windows
 * then takes a mode from this list even if the display didn't have it when it was added (seen on
 * Windows 11 build 26200) */
#define IOCTL_LUZVDA_UPDATE_MODES LUZVDA_CTL_CODE(0x812, LUZVDA_ACCESS_READ_WRITE)
/* In: LUZVDA_LUID. The GPU that renders every display. STATUS_NOT_FOUND if no GPU has that LUID */
#define IOCTL_LUZVDA_SET_RENDER_ADAPTER LUZVDA_CTL_CODE(0x813, LUZVDA_ACCESS_READ_WRITE)
/* Out: LUZVDA_LIST */
#define IOCTL_LUZVDA_LIST_DISPLAYS LUZVDA_CTL_CODE(0x814, LUZVDA_ACCESS_READ)

#define LUZVDA_PROTOCOL_MAJOR 1u
#define LUZVDA_PROTOCOL_MINOR 0u

/* The device interface: {15d7851f-629d-4e2a-bffd-9c5f43ddd57a} */
#define LUZVDA_INTERFACE_GUID_INIT {0x15d7851f, 0x629d, 0x4e2a, {0xbf, 0xfd, 0x9c, 0x5f, 0x43, 0xdd, 0xd5, 0x7a}}
#define LUZVDA_HARDWARE_ID "Root\\LuzVDA"

#define LUZVDA_MAX_MODES 32u
#define LUZVDA_MAX_DISPLAYS 16u
#define LUZVDA_NAME_BYTES 64u

/* The display can show HDR (PQ, BT.2020); the luminance fields must be set */
#define LUZVDA_DISPLAY_HDR 0x1u

/* A Windows GUID's bytes as they lie in memory */
typedef struct LUZVDA_GUID {
  uint8_t bytes[16];
} LUZVDA_GUID;

/* A Windows LUID */
typedef struct LUZVDA_LUID {
  uint32_t low;
  int32_t high;
} LUZVDA_LUID;

typedef struct LUZVDA_VERSION {
  uint16_t major;
  uint16_t minor;
  /* The driver's own version, as in its INF's DriverVer: a.b.c.d */
  uint16_t driver[4];
} LUZVDA_VERSION;

typedef struct LUZVDA_WATCHDOG {
  /* Seconds without a request before every display is removed; 0 when the watchdog is off */
  uint32_t timeout_s;
  uint32_t countdown_s;
} LUZVDA_WATCHDOG;

typedef struct LUZVDA_MODE {
  uint32_t width;
  uint32_t height;
  /* Refresh rate in millihertz: 59940 for 59.94 Hz */
  uint32_t refresh_mhz;
} LUZVDA_MODE;

/* What the virtual display tells Windows about itself */
typedef struct LUZVDA_DISPLAY_DESC {
  /* sizeof(LUZVDA_DISPLAY_DESC) */
  uint32_t struct_size;
  /* LUZVDA_DISPLAY_* */
  uint32_t flags;
  /* Identifies the display; Windows keeps its settings per display */
  LUZVDA_GUID monitor_guid;
  /* UTF-8, ending in NUL within the array. The EDID keeps the first 13 printable ASCII characters */
  char name[LUZVDA_NAME_BYTES];
  /* The physical size, or 0 when unknown */
  uint16_t width_mm;
  uint16_t height_mm;
  /* Red, green, blue and white x,y chromaticity, in 1/1024 (EDID's encoding). All 0 means sRGB */
  uint16_t primaries[8];
  /* Peak and full-screen luminance in nits; HDR displays only */
  uint32_t max_luminance;
  uint32_t max_full_frame_luminance;
  /* Black level in 1/10000 nits; HDR displays only */
  uint32_t min_luminance;
  /* 1 to LUZVDA_MAX_MODES, and which one Windows should start in */
  uint32_t mode_count;
  uint32_t preferred_mode;
  LUZVDA_MODE modes[LUZVDA_MAX_MODES];
} LUZVDA_DISPLAY_DESC;

typedef struct LUZVDA_ADD_OUT {
  LUZVDA_LUID adapter;
  uint32_t target_id;
  uint32_t reserved;
} LUZVDA_ADD_OUT;

typedef struct LUZVDA_UPDATE_MODES {
  /* sizeof(LUZVDA_UPDATE_MODES) */
  uint32_t struct_size;
  LUZVDA_GUID monitor_guid;
  uint32_t mode_count;
  uint32_t preferred_mode;
  LUZVDA_MODE modes[LUZVDA_MAX_MODES];
} LUZVDA_UPDATE_MODES;

typedef struct LUZVDA_LIST_ENTRY {
  LUZVDA_GUID monitor_guid;
  LUZVDA_LUID adapter;
  uint32_t target_id;
  uint32_t connector;
  LUZVDA_MODE preferred_mode;
} LUZVDA_LIST_ENTRY;

typedef struct LUZVDA_LIST {
  uint32_t count;
  uint32_t reserved;
  LUZVDA_LIST_ENTRY displays[LUZVDA_MAX_DISPLAYS];
} LUZVDA_LIST;

#ifdef __cplusplus
}
  #define LUZVDA_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
  #define LUZVDA_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#include <stddef.h>

/* The layout is the protocol: the driver (MSVC) and Apollo (MinGW) must agree on every offset */
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_GUID) == 16, "LUZVDA_GUID");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_LUID) == 8, "LUZVDA_LUID");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_VERSION) == 12, "LUZVDA_VERSION");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_WATCHDOG) == 8, "LUZVDA_WATCHDOG");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_MODE) == 12, "LUZVDA_MODE");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_DISPLAY_DESC, monitor_guid) == 8, "LUZVDA_DISPLAY_DESC.monitor_guid");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_DISPLAY_DESC, name) == 24, "LUZVDA_DISPLAY_DESC.name");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_DISPLAY_DESC, width_mm) == 88, "LUZVDA_DISPLAY_DESC.width_mm");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_DISPLAY_DESC, primaries) == 92, "LUZVDA_DISPLAY_DESC.primaries");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_DISPLAY_DESC, max_luminance) == 108, "LUZVDA_DISPLAY_DESC.max_luminance");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_DISPLAY_DESC, mode_count) == 120, "LUZVDA_DISPLAY_DESC.mode_count");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_DISPLAY_DESC, modes) == 128, "LUZVDA_DISPLAY_DESC.modes");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_DISPLAY_DESC) == 512, "LUZVDA_DISPLAY_DESC");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_ADD_OUT) == 16, "LUZVDA_ADD_OUT");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_UPDATE_MODES, mode_count) == 20, "LUZVDA_UPDATE_MODES.mode_count");
LUZVDA_STATIC_ASSERT(offsetof(LUZVDA_UPDATE_MODES, modes) == 28, "LUZVDA_UPDATE_MODES.modes");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_UPDATE_MODES) == 412, "LUZVDA_UPDATE_MODES");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_LIST_ENTRY) == 44, "LUZVDA_LIST_ENTRY");
LUZVDA_STATIC_ASSERT(sizeof(LUZVDA_LIST) == 712, "LUZVDA_LIST");

#endif /* LUZVDA_IOCTL_H */
