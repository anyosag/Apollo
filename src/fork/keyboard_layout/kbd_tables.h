// The tables a Windows keyboard layout DLL hands to Windows from KbdLayerDescriptor(), as the WDK's
// kbd.h declares them, with only what kbdluzus.c uses.
//
// 64-bit Windows also gives 32-bit programs a WOW64 copy of each layout, a 32-bit DLL whose tables keep
// 64-bit pointers (MSVC's __ptr64). Build that copy with BUILD_WOW6432; the sizes checked at the end
// are the same in both builds.
#pragma once

#include <windows.h>

#if defined(BUILD_WOW6432)
  #define KBD_PTR __ptr64
#else
  #define KBD_PTR
#endif

// Modifier bits, from aVkToBits
#define KBDBASE 0
#define KBDSHIFT 1
#define KBDCTRL 2
#define KBDALT 4

// A combination of modifier bits that types nothing
#define SHFT_INVALID 0x0F

// Special characters in VK_TO_WCHARS tables
#define WCH_NONE 0xF000
#define WCH_DEAD 0xF001

// VK_TO_WCHARS attributes: Caps Lock works like Shift for this key
#define CAPLOK 0x01

// KBDTABLES.fLocaleFlags: Right Alt is AltGr, and Windows presses Left Ctrl with it
#define KLLF_ALTGR 0x0001
#define KBD_VERSION 1

// Flags on the virtual keys of scan codes
#define KBDEXT 0x0100
#define KBDMULTIVK 0x0200
#define KBDSPECIAL 0x0400
#define KBDNUMPAD 0x0800

// No virtual key, and the Brazilian keys only kbd.h names
#define VK__none_ 0xFF
#define VK_ABNT_C1 0xC1
#define VK_ABNT_C2 0xC2

typedef struct {
  BYTE Vk;
  BYTE ModBits;
} VK_TO_BIT;

typedef struct {
  VK_TO_BIT *KBD_PTR pVkToBit;
  WORD wMaxModBits;
  BYTE ModNumber[8];  // wMaxModBits + 1 entries; kbd.h leaves the size open
} MODIFIERS;

typedef struct {
  BYTE VirtualKey;
  BYTE Attributes;
  WCHAR wch[1];
} VK_TO_WCHARS1;

typedef struct {
  BYTE VirtualKey;
  BYTE Attributes;
  WCHAR wch[6];
} VK_TO_WCHARS6;

typedef struct {
  VK_TO_WCHARS1 *KBD_PTR pVkToWchars;
  BYTE nModifications;
  BYTE cbSize;
} VK_TO_WCHAR_TABLE;

typedef struct {
  DWORD dwBoth;  // MAKELONG(character, dead key's character)
  WCHAR wchComposed;
  USHORT uFlags;
} DEADKEY;

#define DEADTRANS(ch, accent, comp, flags) {MAKELONG(ch, accent), comp, flags}

typedef struct {
  BYTE vsc;
  WCHAR *KBD_PTR pwsz;
} VSC_LPWSTR;

typedef struct {
  BYTE Vsc;
  USHORT Vk;
} VSC_VK;

typedef struct {
  BYTE VirtualKey;
  WORD ModificationNumber;
  WCHAR wch[1];
} LIGATURE1;

typedef struct {
  MODIFIERS *KBD_PTR pCharModifiers;
  VK_TO_WCHAR_TABLE *KBD_PTR pVkToWcharTable;
  DEADKEY *KBD_PTR pDeadKey;
  VSC_LPWSTR *KBD_PTR pKeyNames;
  VSC_LPWSTR *KBD_PTR pKeyNamesExt;
  WCHAR *KBD_PTR *KBD_PTR pKeyNamesDead;
  USHORT *KBD_PTR pusVSCtoVK;
  BYTE bMaxVSCtoVK;
  VSC_VK *KBD_PTR pVSCtoVK_E0;
  VSC_VK *KBD_PTR pVSCtoVK_E1;
  DWORD fLocaleFlags;
  BYTE nLgMax;
  BYTE cbLgEntry;
  LIGATURE1 *KBD_PTR pLigature;
  DWORD dwType;
  DWORD dwSubType;
} KBDTABLES;

#if defined(_WIN64) || defined(BUILD_WOW6432)
// The 64-bit layout Windows reads, in either build
C_ASSERT(sizeof(MODIFIERS) == 24);
C_ASSERT(sizeof(VK_TO_WCHAR_TABLE) == 16);
C_ASSERT(sizeof(DEADKEY) == 8);
C_ASSERT(sizeof(VSC_LPWSTR) == 16);
C_ASSERT(sizeof(VSC_VK) == 4);
C_ASSERT(sizeof(VK_TO_WCHARS6) == 14);
C_ASSERT(sizeof(KBDTABLES) == 104);
#endif
