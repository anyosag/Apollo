// Checks the "US - Mac" keyboard layout through Windows itself, once install.ps1 has registered it.
// Apart from AltGr, it must match the PC's own US layout: scan codes, virtual keys, characters with
// Shift, Ctrl, Alt and Caps Lock, VkKeyScan and key names. With AltGr it must type the Mac's Option
// characters, and its dead keys must compose. A standalone program for the fork's CI step:
//   keyboard_layout_win_smoke.exe <layout id>   (the registry key install.ps1 made, e.g. a0000409)

#include "keyboard_layout_expected.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>

using namespace keyboard_layout_expected;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

// Mismatches print one line each, up to a limit, so one run shows what to fix. Comparisons with US
// print US's result first, checks of AltGr the expected one.
static void mismatch(const char *what, unsigned code, const std::wstring &first, const std::wstring &second) {
  if (++s_Failures <= 200) {
    std::printf("FAIL: %s 0x%04X: %ls | %ls\n", what, code, first.c_str(), second.c_str());
  }
}

static std::wstring hex(int value) {
  wchar_t text[16];
  std::swprintf(text, 16, L"0x%X", static_cast<unsigned>(value));
  return text;
}

constexpr int SHIFT = 1;
constexpr int CTRL = 2;
constexpr int ALT = 4;
constexpr int CAPS = 8;
constexpr int ALTGR = CTRL | ALT;

static void key_state(BYTE state[256], int mods) {
  std::fill(state, state + 256, BYTE {0});
  if (mods & SHIFT) {
    state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
  }
  if (mods & CTRL) {
    state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
  }
  if (mods & ALT) {
    state[VK_MENU] = 0x80;
    // AltGr is Left Ctrl with Right Alt
    state[(mods & CTRL) ? VK_RMENU : VK_LMENU] = 0x80;
  }
  if (mods & CAPS) {
    state[VK_CAPITAL] = 0x01;
  }
}

// What ToUnicodeEx() returns for a key, and the characters, as text to compare and print.
// flags 4 leaves the keyboard state (a pending dead key) alone.
static std::wstring to_unicode(HKL hkl, UINT vk, int mods, UINT flags = 4) {
  BYTE state[256];
  key_state(state, mods);
  wchar_t buffer[16] = {};
  UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl);
  int n = ToUnicodeEx(vk, scan, state, buffer, 16, flags, hkl);
  std::wstring text = std::to_wstring(n) + L":";
  for (int i = 0; i < (n < 0 ? 1 : n); i++) {
    text += hex(buffer[i]) + L" ";
  }
  return text;
}

// Clears a dead key left pending by a failed check
static void flush_dead_key(HKL hkl) {
  to_unicode(hkl, VK_SPACE, 0, 0);
  to_unicode(hkl, VK_SPACE, 0, 0);
}

static void compare_with_us(HKL us, HKL mac) {
  // Scan codes, alone and after E0 and E1, to virtual keys
  for (UINT prefix : {0x0000u, 0xE000u, 0xE100u}) {
    for (UINT sc = 0; sc <= 0xFF; sc++) {
      UINT a = MapVirtualKeyExW(prefix | sc, MAPVK_VSC_TO_VK_EX, us);
      UINT b = MapVirtualKeyExW(prefix | sc, MAPVK_VSC_TO_VK_EX, mac);
      if (a != b) {
        mismatch("scan code to virtual key (US | US - Mac)", prefix | sc, hex(a), hex(b));
      }
    }
  }

  for (UINT vk = 1; vk <= 0xFE; vk++) {
    UINT a = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, us);
    UINT b = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, mac);
    if (a != b) {
      mismatch("virtual key to scan code (US | US - Mac)", vk, hex(a), hex(b));
    }
    a = MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, us);
    b = MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, mac);
    if (a != b) {
      mismatch("virtual key to character (US | US - Mac)", vk, hex(a), hex(b));
    }

    for (int mods : {0, SHIFT, CTRL, SHIFT | CTRL, ALT, SHIFT | ALT, CAPS, CAPS | SHIFT, CAPS | CTRL}) {
      std::wstring us_text = to_unicode(us, vk, mods);
      std::wstring mac_text = to_unicode(mac, vk, mods);
      if (us_text != mac_text) {
        char what[64];
        std::snprintf(what, sizeof(what), "ToUnicodeEx (modifiers %d, US | US - Mac) of virtual key", mods);
        mismatch(what, vk, us_text, mac_text);
      }
    }
  }

  // The keys and Shift states VkKeyScan finds for ASCII
  for (wchar_t ch = 1; ch <= 0x7F; ch++) {
    SHORT a = VkKeyScanExW(ch, us);
    SHORT b = VkKeyScanExW(ch, mac);
    if (a != b) {
      mismatch("VkKeyScan (US | US - Mac) of character", ch, hex(a), hex(b));
    }
  }

  // Key names come from the thread's layout
  for (int extended = 0; extended <= 1; extended++) {
    for (LONG sc = 0; sc <= 0x7F; sc++) {
      LONG lparam = (sc << 16) | (extended << 24);
      wchar_t a[64] = {}, b[64] = {};
      ActivateKeyboardLayout(us, 0);
      GetKeyNameTextW(lparam, a, 64);
      ActivateKeyboardLayout(mac, 0);
      GetKeyNameTextW(lparam, b, 64);
      if (std::wcscmp(a, b) != 0) {
        mismatch(extended ? "key name (US | US - Mac) of E0" : "key name (US | US - Mac) of scan code", sc, a, b);
      }
    }
  }
}

static std::wstring expect_char(wchar_t ch) {
  return L"1:" + hex(ch) + L" ";
}

static void check_option_characters(HKL mac) {
  for (const row_t &row : rows) {
    CHECK(row.keys.size() == row.option.size());
    CHECK(row.keys.size() == row.shift_option.size());

    for (size_t i = 0; i < row.keys.size(); i++) {
      UINT vk = row.keys[i];
      std::wstring option = to_unicode(mac, vk, ALTGR);
      std::wstring expected = is_dead(static_cast<BYTE>(vk)) ? L"-1:" + hex(row.option[i]) + L" " : expect_char(row.option[i]);
      if (option != expected) {
        mismatch("AltGr (expected | got) of virtual key", vk, expected, option);
      }

      std::wstring shift_option = to_unicode(mac, vk, SHIFT | ALTGR);
      expected = row.shift_option[i] ? expect_char(row.shift_option[i]) : L"0:";
      if (shift_option != expected) {
        mismatch("Shift+AltGr (expected | got) of virtual key", vk, expected, shift_option);
      }
    }
  }
}

static void check_dead_keys(HKL mac) {
  for (const accent_t &accent : accents) {
    // The dead key that types this accent
    UINT dead_vk = 0;
    for (const row_t &row : rows) {
      for (size_t i = 0; i < row.keys.size(); i++) {
        if (is_dead(static_cast<BYTE>(row.keys[i])) && row.option[i] == accent.accent) {
          dead_vk = row.keys[i];
        }
      }
    }
    CHECK(dead_vk != 0);

    auto type = [&](UINT vk, int mods, const std::wstring &expected) {
      flush_dead_key(mac);
      std::wstring dead = to_unicode(mac, dead_vk, ALTGR, 0);
      std::wstring after = to_unicode(mac, vk, mods, 0);
      if (dead != L"-1:" + hex(accent.accent) + L" " || after != expected) {
        mismatch("dead key (expected | got) then virtual key", vk, expected, dead + L" then " + after);
      }
    };

    for (size_t i = 0; i < accent.letters.size(); i++) {
      bool shift;
      BYTE vk = letter_key(accent.letters[i], &shift);
      type(vk, shift ? SHIFT : 0, expect_char(accent.composed[i]));
    }
    // Space types the accent; a key it doesn't combine with types both
    type(VK_SPACE, 0, expect_char(accent.accent));
    type('Q', 0, L"2:" + hex(accent.accent) + L" " + hex(L'q') + L" ");
  }
  flush_dead_key(mac);
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 2) {
    std::printf("usage: keyboard_layout_win_smoke <layout id>\n");
    return 2;
  }

  HKL us = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
  HKL mac = LoadKeyboardLayoutW(argv[1], KLF_NOTELLSHELL);
  if (!us || !mac) {
    std::printf("FAIL: can't load the layouts (error %lu)\n", GetLastError());
    return 1;
  }
  std::printf("US is %p, US - Mac (%ls) is %p\n", static_cast<void *>(us), argv[1], static_cast<void *>(mac));
  CHECK(us != mac);

  // Windows loaded this layout and didn't fall back to another
  wchar_t name[KL_NAMELENGTH] = {};
  ActivateKeyboardLayout(mac, 0);
  GetKeyboardLayoutNameW(name);
  if (_wcsicmp(name, argv[1]) != 0) {
    std::printf("FAIL: the active layout is %ls, not %ls\n", name, argv[1]);
    s_Failures++;
  }

  // Right Alt+M (Left Ctrl and Right Alt down) types µ: what the layout is for
  CHECK(to_unicode(mac, 'M', ALTGR) == expect_char(0x00B5));

  compare_with_us(us, mac);
  check_option_characters(mac);
  check_dead_keys(mac);

  ActivateKeyboardLayout(us, 0);
  UnloadKeyboardLayout(mac);

  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("keyboard layout through Windows: all checks passed\n");
  return 0;
}
