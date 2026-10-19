// Reads the tables of a built "US - Mac" layout DLL (src/fork/keyboard_layout/) the way Windows does
// and checks the Mac's Option characters and dead keys in them. A standalone program for the fork's
// CI step, run on both DLLs:
//   keyboard_layout_tables_test.exe src_assets/windows/keyboard/kbdluzus.dll        (64-bit build)
//   keyboard_layout_tables_test_wow64.exe src_assets/windows/keyboard/wow64/kbdluzus.dll
// The second is a 32-bit build with BUILD_WOW6432, which reads the 64-bit pointers of the WOW64 copy.
// keyboard_layout_win_smoke.cpp checks the installed layout through Windows itself.

#include "../../src/fork/keyboard_layout/kbd_tables.h"
#include "keyboard_layout_expected.h"

#include <cstdio>
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

typedef KBDTABLES *KBD_PTR (*descriptor_t)(void);

// The tables' pointers are __ptr64 in the WOW64 build, so each one turns into a native pointer
// through a C cast
static const KBDTABLES *s_Tables;

// What a key types with these modifier bits: a character, WCH_NONE, or WCH_DEAD with the dead key's
// character in *dead
static WCHAR translate(BYTE vk, BYTE mod_bits, WCHAR *dead) {
  const MODIFIERS *modifiers = (const MODIFIERS *) s_Tables->pCharModifiers;
  if (mod_bits > modifiers->wMaxModBits) {
    return WCH_NONE;
  }
  BYTE column = modifiers->ModNumber[mod_bits];
  if (column == SHFT_INVALID) {
    return WCH_NONE;
  }

  for (const VK_TO_WCHAR_TABLE *table = (const VK_TO_WCHAR_TABLE *) s_Tables->pVkToWcharTable; table->pVkToWchars; table++) {
    const BYTE *entry = (const BYTE *) table->pVkToWchars;
    for (;; entry += table->cbSize) {
      const VK_TO_WCHARS1 *keys = reinterpret_cast<const VK_TO_WCHARS1 *>(entry);
      if (keys->VirtualKey == 0) {
        break;
      }
      if (keys->VirtualKey != vk) {
        continue;
      }
      if (column >= table->nModifications) {
        return WCH_NONE;
      }
      WCHAR ch = keys->wch[column];
      if (ch == WCH_DEAD) {
        const VK_TO_WCHARS1 *next = reinterpret_cast<const VK_TO_WCHARS1 *>(entry + table->cbSize);
        CHECK(next->VirtualKey == 0xFF);
        *dead = next->wch[column];
      }
      return ch;
    }
  }
  return WCH_NONE;
}

static WCHAR compose(WCHAR accent, WCHAR letter) {
  for (const DEADKEY *dk = (const DEADKEY *) s_Tables->pDeadKey; dk->dwBoth; dk++) {
    if (dk->dwBoth == static_cast<DWORD>(MAKELONG(letter, accent))) {
      return dk->wchComposed;
    }
  }
  return WCH_NONE;
}

static std::wstring key_name(const VSC_LPWSTR *names, BYTE vsc) {
  for (; names->vsc; names++) {
    if (names->vsc == vsc) {
      return (const WCHAR *) names->pwsz;
    }
  }
  return L"";
}

static void check_option_characters() {
  for (const row_t &row : rows) {
    CHECK(row.keys.size() == row.option.size());
    CHECK(row.keys.size() == row.shift_option.size());

    for (size_t i = 0; i < row.keys.size(); i++) {
      BYTE vk = static_cast<BYTE>(row.keys[i]);
      WCHAR dead = 0;

      WCHAR option = translate(vk, KBDCTRL | KBDALT, &dead);
      if (is_dead(vk)) {
        if (option != WCH_DEAD || dead != row.option[i]) {
          std::printf("FAIL: AltGr+VK 0x%02X: 0x%04X (dead 0x%04X), expected the dead key 0x%04X\n", vk, option, dead, row.option[i]);
          s_Failures++;
        }
      } else if (option != row.option[i]) {
        std::printf("FAIL: AltGr+VK 0x%02X: 0x%04X, expected 0x%04X\n", vk, option, row.option[i]);
        s_Failures++;
      }

      WCHAR expected = row.shift_option[i] ? row.shift_option[i] : WCH_NONE;
      WCHAR shift_option = translate(vk, KBDSHIFT | KBDCTRL | KBDALT, &dead);
      if (shift_option != expected) {
        std::printf("FAIL: Shift+AltGr+VK 0x%02X: 0x%04X, expected 0x%04X\n", vk, shift_option, expected);
        s_Failures++;
      }
    }
  }
}

static void check_accents() {
  for (const accent_t &accent : accents) {
    CHECK(accent.letters.size() == accent.composed.size());
    for (size_t i = 0; i < accent.letters.size(); i++) {
      WCHAR composed = compose(accent.accent, accent.letters[i]);
      if (composed != accent.composed[i]) {
        std::printf("FAIL: 0x%04X then '%c': 0x%04X, expected 0x%04X\n", accent.accent, static_cast<char>(accent.letters[i]), composed, accent.composed[i]);
        s_Failures++;
      }
    }
    // Space types the accent alone; other keys don't combine
    CHECK(compose(accent.accent, L' ') == accent.accent);
    CHECK(compose(accent.accent, L'q') == WCH_NONE);
  }

  // Each dead key's accent has compositions and a name
  int dead_keys_seen = 0;
  for (const row_t &row : rows) {
    for (size_t i = 0; i < row.keys.size(); i++) {
      BYTE vk = static_cast<BYTE>(row.keys[i]);
      WCHAR dead = 0;
      if (translate(vk, KBDCTRL | KBDALT, &dead) != WCH_DEAD) {
        continue;
      }
      dead_keys_seen++;
      CHECK(compose(dead, L' ') == dead);

      bool named = false;
      for (WCHAR *KBD_PTR const *name = (WCHAR *KBD_PTR const *) s_Tables->pKeyNamesDead; *name; name++) {
        named = named || ((const WCHAR *) *name)[0] == dead;
      }
      CHECK(named);
    }
  }
  CHECK(dead_keys_seen == static_cast<int>(dead_keys.size()));
}

static void check_us_layout() {
  WCHAR dead = 0;
  // Without AltGr, US
  CHECK(translate('M', KBDBASE, &dead) == L'm');
  CHECK(translate('M', KBDSHIFT, &dead) == L'M');
  CHECK(translate('E', KBDBASE, &dead) == L'e');
  CHECK(translate(VK_OEM_3, KBDBASE, &dead) == L'`');
  CHECK(translate(VK_OEM_3, KBDSHIFT, &dead) == L'~');
  CHECK(translate('2', KBDSHIFT | KBDCTRL, &dead) == 0x0000);
  CHECK(translate(VK_OEM_4, KBDCTRL, &dead) == 0x001B);
  CHECK(translate(VK_NUMPAD5, KBDBASE, &dead) == L'5');
  // Alt alone types nothing, so Alt+M stays a menu shortcut
  CHECK(translate('M', KBDALT, &dead) == WCH_NONE);
  CHECK(translate('M', KBDSHIFT | KBDALT, &dead) == WCH_NONE);

  // Right Alt is AltGr
  CHECK((s_Tables->fLocaleFlags & KLLF_ALTGR) != 0);
  CHECK(HIWORD(s_Tables->fLocaleFlags) == KBD_VERSION);

  // Scan codes: M, Right Alt (E0 38), Pause (E1 1D)
  CHECK(s_Tables->bMaxVSCtoVK == 0x7F);
  const USHORT *vsc_to_vk = (const USHORT *) s_Tables->pusVSCtoVK;
  CHECK(vsc_to_vk[0x32] == 'M');
  CHECK(vsc_to_vk[0x38] == VK_LMENU);
  bool right_alt = false;
  for (const VSC_VK *e0 = (const VSC_VK *) s_Tables->pVSCtoVK_E0; e0->Vsc; e0++) {
    right_alt = right_alt || (e0->Vsc == 0x38 && e0->Vk == (VK_RMENU | KBDEXT));
  }
  CHECK(right_alt);
  const VSC_VK *e1 = (const VSC_VK *) s_Tables->pVSCtoVK_E1;
  CHECK(e1[0].Vsc == 0x1D && e1[0].Vk == VK_PAUSE);

  CHECK(key_name((const VSC_LPWSTR *) s_Tables->pKeyNames, 0x01) == L"Esc");
  CHECK(key_name((const VSC_LPWSTR *) s_Tables->pKeyNamesExt, 0x38) == L"Right Alt");
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s <kbdluzus.dll>\n", argv[0]);
    return 2;
  }

  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll) {
    std::printf("FAIL: can't load %s (error %lu)\n", argv[1], GetLastError());
    return 1;
  }
  auto descriptor = reinterpret_cast<descriptor_t>(reinterpret_cast<void *>(GetProcAddress(dll, "KbdLayerDescriptor")));
  if (!descriptor) {
    std::printf("FAIL: %s exports no KbdLayerDescriptor\n", argv[1]);
    return 1;
  }
  s_Tables = (const KBDTABLES *) descriptor();
  CHECK(s_Tables != nullptr);
  CHECK(descriptor == reinterpret_cast<descriptor_t>(reinterpret_cast<void *>(GetProcAddress(dll, MAKEINTRESOURCEA(1)))));

  if (s_Tables) {
    check_option_characters();
    check_accents();
    check_us_layout();
  }

  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("keyboard layout tables: all checks passed (%d-bit build)\n", static_cast<int>(sizeof(void *) * 8));
  return 0;
}
