// What the "US - Mac" keyboard layout (src/fork/keyboard_layout/kbdluzus.c) must type with AltGr: the
// Mac U.S. layout's Option characters, written out again here row by row as the Mac's Keyboard Viewer
// shows them, so a slip in either place fails the tests.
#pragma once

#include <string_view>

#include <windows.h>

namespace keyboard_layout_expected {
  using namespace std::string_view_literals;

  struct row_t {
    std::u16string_view keys;  // virtual keys, one per character
    std::u16string_view option;  // Option, i.e. AltGr; a dead key's accent
    std::u16string_view shift_option;  // Shift+Option; \0 for nothing
  };

  inline constexpr row_t rows[] = {
    {u"\xC0" u"1234567890\xBD\xBB"sv, u"`¡™£¢∞§¶•ªº–≠"sv, u"`⁄€‹›ﬁﬂ‡°·‚—±"sv},
    {u"QWERTYUIOP\xDB\xDD\xDC"sv, u"œ∑´®†¥¨ˆøπ“‘«"sv, u"Œ„´‰ˇÁ¨ˆØ∏”’»"sv},
    // Shift+Option+K is the Apple logo, a private character Windows fonts don't draw
    {u"ASDFGHJKL\xBA\xDE"sv, u"åß∂ƒ©˙∆˚¬…æ"sv, u"ÅÍÎÏ˝ÓÔ\0ÒÚÆ"sv},
    {u"ZXCVBNM\xBC\xBE\xBF"sv, u"Ω≈ç√∫˜µ≤≥÷"sv, u"¸˛Ç◊ı˜Â¯˘¿"sv},
    {u" "sv, u" "sv, u" "sv},  // no-break space
  };

  // Option+these is a dead key, whose accent is its character in rows
  inline constexpr std::u16string_view dead_keys = u"\xC0" u"EUIN"sv;

  struct accent_t {
    char16_t accent;
    std::u16string_view letters;
    std::u16string_view composed;
  };

  inline constexpr accent_t accents[] = {
    {u'`', u"aeiouAEIOU"sv, u"àèìòùÀÈÌÒÙ"sv},
    {u'´', u"aeiouyAEIOUY"sv, u"áéíóúýÁÉÍÓÚÝ"sv},
    {u'ˆ', u"aeiouAEIOU"sv, u"âêîôûÂÊÎÔÛ"sv},
    {u'˜', u"anoANO"sv, u"ãñõÃÑÕ"sv},
    {u'¨', u"aeiouyAEIOUY"sv, u"äëïöüÿÄËÏÖÜŸ"sv},
  };

  // The virtual key that types a letter of accents, and whether it takes Shift
  inline BYTE letter_key(char16_t letter, bool *shift) {
    *shift = letter >= u'A' && letter <= u'Z';
    return static_cast<BYTE>(*shift ? letter : letter - u'a' + u'A');
  }

  inline bool is_dead(BYTE vk) {
    return dead_keys.find(static_cast<char16_t>(vk)) != std::u16string_view::npos;
  }
}  // namespace keyboard_layout_expected
