// "US - Mac", a Windows keyboard layout for typing from a Mac through LuzLunar: Windows' US layout,
// plus the Mac U.S. layout's Option characters on AltGr (Right Alt). LuzLunar sends the Mac's right
// Option key as Right Alt, so Right Option+M types µ on the PC as on the Mac, and Right Option+E then E
// types é. Left Option stays Alt for shortcuts.
//
// Without AltGr, and in every scan code, key name and control character, it is Microsoft's US layout
// (the WDK's kbdus sample, KBD_TYPE 4); the CI test compares it with the PC's own US layout.

#include "kbd_tables.h"

// Scan code to virtual key, indexed by scan code
static USHORT ausVK[] = {
  VK__none_, VK_ESCAPE, '1', '2', '3', '4', '5', '6',  // 0x00
  '7', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',  // 0x10
  'O', 'P', VK_OEM_4, VK_OEM_6, VK_RETURN, VK_LCONTROL, 'A', 'S',
  'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1,  // 0x20
  VK_OEM_7, VK_OEM_3, VK_LSHIFT, VK_OEM_5, 'Z', 'X', 'C', 'V',
  'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2,  // 0x30
  VK_RSHIFT | KBDEXT,  // The right Shift is an extended key
  VK_MULTIPLY | KBDMULTIVK,  // With Shift or Alt, Print Screen
  VK_LMENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5,
  VK_F6, VK_F7, VK_F8, VK_F9, VK_F10,  // 0x40
  VK_NUMLOCK | KBDEXT | KBDMULTIVK,  // With Ctrl, Pause
  VK_SCROLL | KBDMULTIVK,
  // The keypad's digits and point, with their navigation keys when Num Lock is off
  VK_HOME | KBDNUMPAD | KBDSPECIAL,  // 7
  VK_UP | KBDNUMPAD | KBDSPECIAL,  // 8
  VK_PRIOR | KBDNUMPAD | KBDSPECIAL,  // 9
  VK_SUBTRACT,
  VK_LEFT | KBDNUMPAD | KBDSPECIAL,  // 4
  VK_CLEAR | KBDNUMPAD | KBDSPECIAL,  // 5
  VK_RIGHT | KBDNUMPAD | KBDSPECIAL,  // 6
  VK_ADD,
  VK_END | KBDNUMPAD | KBDSPECIAL,  // 1
  VK_DOWN | KBDNUMPAD | KBDSPECIAL,  // 2
  VK_NEXT | KBDNUMPAD | KBDSPECIAL,  // 3
  VK_INSERT | KBDNUMPAD | KBDSPECIAL,  // 0
  VK_DELETE | KBDNUMPAD | KBDSPECIAL,  // .
  VK_SNAPSHOT, VK__none_, VK_OEM_102, VK_F11, VK_F12, VK_CLEAR, VK_OEM_WSCTRL, VK_OEM_FINISH,  // 0x54
  VK_OEM_JUMP, VK_EREOF, VK_OEM_BACKTAB, VK_OEM_AUTO, VK__none_, VK__none_, VK_ZOOM, VK_HELP,  // 0x5C
  VK_F13, VK_F14, VK_F15, VK_F16, VK_F17, VK_F18, VK_F19, VK_F20,  // 0x64
  VK_F21, VK_F22, VK_F23, VK_OEM_PA3, VK__none_, VK_OEM_RESET, VK__none_, VK_ABNT_C1,  // 0x6C
  VK__none_, VK__none_, VK_F24, VK__none_, VK__none_, VK__none_, VK__none_, VK_OEM_PA1,  // 0x74
  VK_TAB, VK__none_, VK_ABNT_C2,  // 0x7C
};

// Scan codes after an E0 prefix
static VSC_VK aE0VscToVk[] = {
  {0x10, VK_MEDIA_PREV_TRACK | KBDEXT},
  {0x19, VK_MEDIA_NEXT_TRACK | KBDEXT},
  {0x1D, VK_RCONTROL | KBDEXT},
  {0x20, VK_VOLUME_MUTE | KBDEXT},
  {0x21, VK_LAUNCH_APP2 | KBDEXT},
  {0x22, VK_MEDIA_PLAY_PAUSE | KBDEXT},
  {0x24, VK_MEDIA_STOP | KBDEXT},
  {0x2E, VK_VOLUME_DOWN | KBDEXT},
  {0x30, VK_VOLUME_UP | KBDEXT},
  {0x32, VK_BROWSER_HOME | KBDEXT},
  {0x35, VK_DIVIDE | KBDEXT},
  {0x37, VK_SNAPSHOT | KBDEXT},
  {0x38, VK_RMENU | KBDEXT},
  {0x47, VK_HOME | KBDEXT},
  {0x48, VK_UP | KBDEXT},
  {0x49, VK_PRIOR | KBDEXT},
  {0x4B, VK_LEFT | KBDEXT},
  {0x4D, VK_RIGHT | KBDEXT},
  {0x4F, VK_END | KBDEXT},
  {0x50, VK_DOWN | KBDEXT},
  {0x51, VK_NEXT | KBDEXT},
  {0x52, VK_INSERT | KBDEXT},
  {0x53, VK_DELETE | KBDEXT},
  {0x5B, VK_LWIN | KBDEXT},
  {0x5C, VK_RWIN | KBDEXT},
  {0x5D, VK_APPS | KBDEXT},
  {0x5F, VK_SLEEP | KBDEXT},
  {0x65, VK_BROWSER_SEARCH | KBDEXT},
  {0x66, VK_BROWSER_FAVORITES | KBDEXT},
  {0x67, VK_BROWSER_REFRESH | KBDEXT},
  {0x68, VK_BROWSER_STOP | KBDEXT},
  {0x69, VK_BROWSER_FORWARD | KBDEXT},
  {0x6A, VK_BROWSER_BACK | KBDEXT},
  {0x6B, VK_LAUNCH_APP1 | KBDEXT},
  {0x6C, VK_LAUNCH_MAIL | KBDEXT},
  {0x6D, VK_LAUNCH_MEDIA_SELECT | KBDEXT},
  {0x1C, VK_RETURN | KBDEXT},  // The keypad's Enter
  {0x46, VK_CANCEL | KBDEXT},  // Break (Ctrl+Pause)
  {0, 0},
};

// Scan codes after an E1 prefix
static VSC_VK aE1VscToVk[] = {
  {0x1D, VK_PAUSE},
  {0, 0},
};

static VK_TO_BIT aVkToBits[] = {
  {VK_SHIFT, KBDSHIFT},
  {VK_CONTROL, KBDCTRL},
  {VK_MENU, KBDALT},
  {0, 0},
};

// Modifier bits to the column of aVkToWch6. AltGr is Ctrl+Alt; Alt alone types nothing, so Alt+key
// reaches apps as a menu shortcut.
static MODIFIERS CharModifiers = {
  aVkToBits,
  7,
  {
    0,  // none
    1,  // Shift
    2,  // Ctrl
    3,  // Shift+Ctrl
    SHFT_INVALID,  // Alt
    SHFT_INVALID,  // Shift+Alt
    4,  // AltGr
    5,  // Shift+AltGr
  },
};

// The dead keys' characters: alone, after a key that doesn't combine with them, and with Space
#define DEAD_GRAVE 0x0060  // `
#define DEAD_ACUTE 0x00B4  // ´
#define DEAD_CIRCUMFLEX 0x02C6  // ˆ
#define DEAD_TILDE 0x02DC  // ˜
#define DEAD_DIAERESIS 0x00A8  // ¨

// Each key's characters. A row with 0xFF follows a dead key and holds its character in the column
// where the key has WCH_DEAD. The keys are in the US layout's order, which decides the key
// VkKeyScan() finds for a character two keys type.
static VK_TO_WCHARS6 aVkToWch6[] = {
  //                              Shift     Ctrl      Shift+Ctrl  AltGr     Shift+AltGr
  {VK_OEM_4, 0, '[', '{', 0x001B, WCH_NONE, 0x201C, 0x201D},  // “ ”
  {VK_OEM_6, 0, ']', '}', 0x001D, WCH_NONE, 0x2018, 0x2019},  // ‘ ’
  {VK_OEM_5, 0, '\\', '|', 0x001C, WCH_NONE, 0x00AB, 0x00BB},  // « »
  {VK_OEM_102, 0, '\\', '|', 0x001C, WCH_NONE, 0x00AB, 0x00BB},  // « »
  {VK_BACK, 0, '\b', '\b', 0x007F, WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_ESCAPE, 0, 0x001B, 0x001B, 0x001B, WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_RETURN, 0, '\r', '\r', '\n', WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_SPACE, 0, ' ', ' ', ' ', WCH_NONE, 0x00A0, 0x00A0},  // no-break space
  {VK_CANCEL, 0, 0x0003, 0x0003, 0x0003, WCH_NONE, WCH_NONE, WCH_NONE},
  {'2', 0, '2', '@', WCH_NONE, 0x0000, 0x2122, 0x20AC},  // ™ €
  {'6', 0, '6', '^', WCH_NONE, 0x001E, 0x00A7, 0xFB02},  // § ﬂ
  {VK_OEM_MINUS, 0, '-', '_', WCH_NONE, 0x001F, 0x2013, 0x2014},  // – —
  {VK_OEM_3, 0, '`', '~', WCH_NONE, WCH_NONE, WCH_DEAD, '`'},
  {0xFF, 0, WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE, DEAD_GRAVE, WCH_NONE},
  {'1', 0, '1', '!', WCH_NONE, WCH_NONE, 0x00A1, 0x2044},  // ¡ ⁄
  {'3', 0, '3', '#', WCH_NONE, WCH_NONE, 0x00A3, 0x2039},  // £ ‹
  {'4', 0, '4', '$', WCH_NONE, WCH_NONE, 0x00A2, 0x203A},  // ¢ ›
  {'5', 0, '5', '%', WCH_NONE, WCH_NONE, 0x221E, 0xFB01},  // ∞ ﬁ
  {'7', 0, '7', '&', WCH_NONE, WCH_NONE, 0x00B6, 0x2021},  // ¶ ‡
  {'8', 0, '8', '*', WCH_NONE, WCH_NONE, 0x2022, 0x00B0},  // • °
  {'9', 0, '9', '(', WCH_NONE, WCH_NONE, 0x00AA, 0x00B7},  // ª ·
  {'0', 0, '0', ')', WCH_NONE, WCH_NONE, 0x00BA, 0x201A},  // º ‚
  {VK_OEM_PLUS, 0, '=', '+', WCH_NONE, WCH_NONE, 0x2260, 0x00B1},  // ≠ ±
  {'Q', CAPLOK, 'q', 'Q', WCH_NONE, WCH_NONE, 0x0153, 0x0152},  // œ Œ
  {'W', CAPLOK, 'w', 'W', WCH_NONE, WCH_NONE, 0x2211, 0x201E},  // ∑ „
  {'E', CAPLOK, 'e', 'E', WCH_NONE, WCH_NONE, WCH_DEAD, DEAD_ACUTE},
  {0xFF, 0, WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE, DEAD_ACUTE, WCH_NONE},
  {'R', CAPLOK, 'r', 'R', WCH_NONE, WCH_NONE, 0x00AE, 0x2030},  // ® ‰
  {'T', CAPLOK, 't', 'T', WCH_NONE, WCH_NONE, 0x2020, 0x02C7},  // † ˇ
  {'Y', CAPLOK, 'y', 'Y', WCH_NONE, WCH_NONE, 0x00A5, 0x00C1},  // ¥ Á
  {'U', CAPLOK, 'u', 'U', WCH_NONE, WCH_NONE, WCH_DEAD, DEAD_DIAERESIS},
  {0xFF, 0, WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE, DEAD_DIAERESIS, WCH_NONE},
  {'I', CAPLOK, 'i', 'I', WCH_NONE, WCH_NONE, WCH_DEAD, DEAD_CIRCUMFLEX},
  {0xFF, 0, WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE, DEAD_CIRCUMFLEX, WCH_NONE},
  {'O', CAPLOK, 'o', 'O', WCH_NONE, WCH_NONE, 0x00F8, 0x00D8},  // ø Ø
  {'P', CAPLOK, 'p', 'P', WCH_NONE, WCH_NONE, 0x03C0, 0x220F},  // π ∏
  {'A', CAPLOK, 'a', 'A', WCH_NONE, WCH_NONE, 0x00E5, 0x00C5},  // å Å
  {'S', CAPLOK, 's', 'S', WCH_NONE, WCH_NONE, 0x00DF, 0x00CD},  // ß Í
  {'D', CAPLOK, 'd', 'D', WCH_NONE, WCH_NONE, 0x2202, 0x00CE},  // ∂ Î
  {'F', CAPLOK, 'f', 'F', WCH_NONE, WCH_NONE, 0x0192, 0x00CF},  // ƒ Ï
  {'G', CAPLOK, 'g', 'G', WCH_NONE, WCH_NONE, 0x00A9, 0x02DD},  // © ˝
  {'H', CAPLOK, 'h', 'H', WCH_NONE, WCH_NONE, 0x02D9, 0x00D3},  // ˙ Ó
  {'J', CAPLOK, 'j', 'J', WCH_NONE, WCH_NONE, 0x2206, 0x00D4},  // ∆ Ô
  // Shift+Option+K is the Apple logo, a private character Windows fonts don't draw
  {'K', CAPLOK, 'k', 'K', WCH_NONE, WCH_NONE, 0x02DA, WCH_NONE},  // ˚
  {'L', CAPLOK, 'l', 'L', WCH_NONE, WCH_NONE, 0x00AC, 0x00D2},  // ¬ Ò
  {VK_OEM_1, 0, ';', ':', WCH_NONE, WCH_NONE, 0x2026, 0x00DA},  // … Ú
  {VK_OEM_7, 0, '\'', '"', WCH_NONE, WCH_NONE, 0x00E6, 0x00C6},  // æ Æ
  {'Z', CAPLOK, 'z', 'Z', WCH_NONE, WCH_NONE, 0x03A9, 0x00B8},  // Ω ¸
  {'X', CAPLOK, 'x', 'X', WCH_NONE, WCH_NONE, 0x2248, 0x02DB},  // ≈ ˛
  {'C', CAPLOK, 'c', 'C', WCH_NONE, WCH_NONE, 0x00E7, 0x00C7},  // ç Ç
  {'V', CAPLOK, 'v', 'V', WCH_NONE, WCH_NONE, 0x221A, 0x25CA},  // √ ◊
  {'B', CAPLOK, 'b', 'B', WCH_NONE, WCH_NONE, 0x222B, 0x0131},  // ∫ ı
  {'N', CAPLOK, 'n', 'N', WCH_NONE, WCH_NONE, WCH_DEAD, DEAD_TILDE},
  {0xFF, 0, WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE, DEAD_TILDE, WCH_NONE},
  {'M', CAPLOK, 'm', 'M', WCH_NONE, WCH_NONE, 0x00B5, 0x00C2},  // µ Â
  {VK_OEM_COMMA, 0, ',', '<', WCH_NONE, WCH_NONE, 0x2264, 0x00AF},  // ≤ ¯
  {VK_OEM_PERIOD, 0, '.', '>', WCH_NONE, WCH_NONE, 0x2265, 0x02D8},  // ≥ ˘
  {VK_OEM_2, 0, '/', '?', WCH_NONE, WCH_NONE, 0x00F7, 0x00BF},  // ÷ ¿
  {VK_DECIMAL, 0, '.', '.', WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_TAB, 0, '\t', '\t', WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_ADD, 0, '+', '+', WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_DIVIDE, 0, '/', '/', WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_MULTIPLY, 0, '*', '*', WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE},
  {VK_SUBTRACT, 0, '-', '-', WCH_NONE, WCH_NONE, WCH_NONE, WCH_NONE},
  {0, 0, 0, 0, 0, 0, 0, 0},
};

// The keypad's digits, last so VkKeyScan() finds digits on the main keys first
static VK_TO_WCHARS1 aVkToWch1[] = {
  {VK_NUMPAD0, 0, '0'},
  {VK_NUMPAD1, 0, '1'},
  {VK_NUMPAD2, 0, '2'},
  {VK_NUMPAD3, 0, '3'},
  {VK_NUMPAD4, 0, '4'},
  {VK_NUMPAD5, 0, '5'},
  {VK_NUMPAD6, 0, '6'},
  {VK_NUMPAD7, 0, '7'},
  {VK_NUMPAD8, 0, '8'},
  {VK_NUMPAD9, 0, '9'},
  {0, 0, 0},
};

static VK_TO_WCHAR_TABLE aVkToWcharTable[] = {
  {(VK_TO_WCHARS1 *) aVkToWch6, 6, sizeof(aVkToWch6[0])},
  {aVkToWch1, 1, sizeof(aVkToWch1[0])},
  {NULL, 0, 0},
};

// What a dead key and the next character type together. As on the Mac, any other character types
// the dead key's character and then itself.
static DEADKEY aDeadKey[] = {
  DEADTRANS('a', DEAD_GRAVE, 0x00E0, 0),  // à
  DEADTRANS('e', DEAD_GRAVE, 0x00E8, 0),  // è
  DEADTRANS('i', DEAD_GRAVE, 0x00EC, 0),  // ì
  DEADTRANS('o', DEAD_GRAVE, 0x00F2, 0),  // ò
  DEADTRANS('u', DEAD_GRAVE, 0x00F9, 0),  // ù
  DEADTRANS('A', DEAD_GRAVE, 0x00C0, 0),  // À
  DEADTRANS('E', DEAD_GRAVE, 0x00C8, 0),  // È
  DEADTRANS('I', DEAD_GRAVE, 0x00CC, 0),  // Ì
  DEADTRANS('O', DEAD_GRAVE, 0x00D2, 0),  // Ò
  DEADTRANS('U', DEAD_GRAVE, 0x00D9, 0),  // Ù
  DEADTRANS(' ', DEAD_GRAVE, DEAD_GRAVE, 0),

  DEADTRANS('a', DEAD_ACUTE, 0x00E1, 0),  // á
  DEADTRANS('e', DEAD_ACUTE, 0x00E9, 0),  // é
  DEADTRANS('i', DEAD_ACUTE, 0x00ED, 0),  // í
  DEADTRANS('o', DEAD_ACUTE, 0x00F3, 0),  // ó
  DEADTRANS('u', DEAD_ACUTE, 0x00FA, 0),  // ú
  DEADTRANS('y', DEAD_ACUTE, 0x00FD, 0),  // ý
  DEADTRANS('A', DEAD_ACUTE, 0x00C1, 0),  // Á
  DEADTRANS('E', DEAD_ACUTE, 0x00C9, 0),  // É
  DEADTRANS('I', DEAD_ACUTE, 0x00CD, 0),  // Í
  DEADTRANS('O', DEAD_ACUTE, 0x00D3, 0),  // Ó
  DEADTRANS('U', DEAD_ACUTE, 0x00DA, 0),  // Ú
  DEADTRANS('Y', DEAD_ACUTE, 0x00DD, 0),  // Ý
  DEADTRANS(' ', DEAD_ACUTE, DEAD_ACUTE, 0),

  DEADTRANS('a', DEAD_CIRCUMFLEX, 0x00E2, 0),  // â
  DEADTRANS('e', DEAD_CIRCUMFLEX, 0x00EA, 0),  // ê
  DEADTRANS('i', DEAD_CIRCUMFLEX, 0x00EE, 0),  // î
  DEADTRANS('o', DEAD_CIRCUMFLEX, 0x00F4, 0),  // ô
  DEADTRANS('u', DEAD_CIRCUMFLEX, 0x00FB, 0),  // û
  DEADTRANS('A', DEAD_CIRCUMFLEX, 0x00C2, 0),  // Â
  DEADTRANS('E', DEAD_CIRCUMFLEX, 0x00CA, 0),  // Ê
  DEADTRANS('I', DEAD_CIRCUMFLEX, 0x00CE, 0),  // Î
  DEADTRANS('O', DEAD_CIRCUMFLEX, 0x00D4, 0),  // Ô
  DEADTRANS('U', DEAD_CIRCUMFLEX, 0x00DB, 0),  // Û
  DEADTRANS(' ', DEAD_CIRCUMFLEX, DEAD_CIRCUMFLEX, 0),

  DEADTRANS('a', DEAD_TILDE, 0x00E3, 0),  // ã
  DEADTRANS('n', DEAD_TILDE, 0x00F1, 0),  // ñ
  DEADTRANS('o', DEAD_TILDE, 0x00F5, 0),  // õ
  DEADTRANS('A', DEAD_TILDE, 0x00C3, 0),  // Ã
  DEADTRANS('N', DEAD_TILDE, 0x00D1, 0),  // Ñ
  DEADTRANS('O', DEAD_TILDE, 0x00D5, 0),  // Õ
  DEADTRANS(' ', DEAD_TILDE, DEAD_TILDE, 0),

  DEADTRANS('a', DEAD_DIAERESIS, 0x00E4, 0),  // ä
  DEADTRANS('e', DEAD_DIAERESIS, 0x00EB, 0),  // ë
  DEADTRANS('i', DEAD_DIAERESIS, 0x00EF, 0),  // ï
  DEADTRANS('o', DEAD_DIAERESIS, 0x00F6, 0),  // ö
  DEADTRANS('u', DEAD_DIAERESIS, 0x00FC, 0),  // ü
  DEADTRANS('y', DEAD_DIAERESIS, 0x00FF, 0),  // ÿ
  DEADTRANS('A', DEAD_DIAERESIS, 0x00C4, 0),  // Ä
  DEADTRANS('E', DEAD_DIAERESIS, 0x00CB, 0),  // Ë
  DEADTRANS('I', DEAD_DIAERESIS, 0x00CF, 0),  // Ï
  DEADTRANS('O', DEAD_DIAERESIS, 0x00D6, 0),  // Ö
  DEADTRANS('U', DEAD_DIAERESIS, 0x00DC, 0),  // Ü
  DEADTRANS('Y', DEAD_DIAERESIS, 0x0178, 0),  // Ÿ
  DEADTRANS(' ', DEAD_DIAERESIS, DEAD_DIAERESIS, 0),

  {0, 0, 0},
};

// Key names for GetKeyNameText(), by scan code. Keys that type a character are named by it.
static VSC_LPWSTR aKeyNames[] = {
  {0x01, L"Esc"},
  {0x0E, L"Backspace"},
  {0x0F, L"Tab"},
  {0x1C, L"Enter"},
  {0x1D, L"Ctrl"},
  {0x2A, L"Shift"},
  {0x36, L"Right Shift"},
  {0x37, L"Num *"},
  {0x38, L"Alt"},
  {0x39, L"Space"},
  {0x3A, L"Caps Lock"},
  {0x3B, L"F1"},
  {0x3C, L"F2"},
  {0x3D, L"F3"},
  {0x3E, L"F4"},
  {0x3F, L"F5"},
  {0x40, L"F6"},
  {0x41, L"F7"},
  {0x42, L"F8"},
  {0x43, L"F9"},
  {0x44, L"F10"},
  {0x45, L"Pause"},
  {0x46, L"Scroll Lock"},
  {0x47, L"Num 7"},
  {0x48, L"Num 8"},
  {0x49, L"Num 9"},
  {0x4A, L"Num -"},
  {0x4B, L"Num 4"},
  {0x4C, L"Num 5"},
  {0x4D, L"Num 6"},
  {0x4E, L"Num +"},
  {0x4F, L"Num 1"},
  {0x50, L"Num 2"},
  {0x51, L"Num 3"},
  {0x52, L"Num 0"},
  {0x53, L"Num Del"},
  {0x54, L"Sys Req"},
  {0x57, L"F11"},
  {0x58, L"F12"},
  {0x7C, L"F13"},
  {0x7D, L"F14"},
  {0x7E, L"F15"},
  {0x7F, L"F16"},
  {0x80, L"F17"},
  {0x81, L"F18"},
  {0x82, L"F19"},
  {0x83, L"F20"},
  {0x84, L"F21"},
  {0x85, L"F22"},
  {0x86, L"F23"},
  {0x87, L"F24"},
  {0, NULL},
};

// Names of the keys with an E0 prefix
static VSC_LPWSTR aKeyNamesExt[] = {
  {0x1C, L"Num Enter"},
  {0x1D, L"Right Ctrl"},
  {0x35, L"Num /"},
  {0x37, L"Prnt Scrn"},
  {0x38, L"Right Alt"},
  {0x45, L"Num Lock"},
  {0x46, L"Break"},
  {0x47, L"Home"},
  {0x48, L"Up"},
  {0x49, L"Page Up"},
  {0x4B, L"Left"},
  {0x4D, L"Right"},
  {0x4F, L"End"},
  {0x50, L"Down"},
  {0x51, L"Page Down"},
  {0x52, L"Insert"},
  {0x53, L"Delete"},
  {0x54, L"<00>"},
  {0x56, L"Help"},
  {0x5B, L"Left Windows"},
  {0x5C, L"Right Windows"},
  {0x5D, L"Application"},
  {0, NULL},
};

// Dead keys' names: the character, then the name
static WCHAR *KBD_PTR aKeyNamesDead[] = {
  L"\x0060" L"GRAVE",
  L"\x00B4" L"ACUTE",
  L"\x02C6" L"CIRCUMFLEX",
  L"\x02DC" L"TILDE",
  L"\x00A8" L"DIAERESIS",
  NULL,
};

static KBDTABLES KbdTables = {
  &CharModifiers,
  aVkToWcharTable,
  aDeadKey,
  aKeyNames,
  aKeyNamesExt,
  aKeyNamesDead,
  ausVK,
  sizeof(ausVK) / sizeof(ausVK[0]),
  aE0VscToVk,
  aE1VscToVk,
  MAKELONG(KLLF_ALTGR, KBD_VERSION),
  0,  // no ligatures
  0,
  NULL,
  0,  // dwType and dwSubType: unset, as in the US layout
  0,
};

// The DLL's one export (kbdluzus.def). Windows reads the tables this returns.
KBDTABLES *KBD_PTR KbdLayerDescriptor(void) {
  return &KbdTables;
}
