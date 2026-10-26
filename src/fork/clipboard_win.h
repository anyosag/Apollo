/**
 * @file src/fork/clipboard_win.h
 * @brief Fork-only: reads and writes the Windows clipboard in clipbundle's formats.
 *
 * No Apollo dependencies, so tests/fork/clipboard_win_smoke.cpp can build it on its own. Call from a
 * thread that has initialized COM (images go through WIC). Windows only.
 */
#pragma once

#ifdef _WIN32

// standard includes
  #include <cstdint>
  #include <string>
  #include <vector>

// local includes
  #include "clipbundle.h"

namespace fork_tweaks::clipboard_win {
  struct snapshot {
    std::uint32_t sequence = 0;  ///< GetClipboardSequenceNumber() before reading, so a change while reading isn't missed
    bool concealed = false;  ///< A password manager marked the content as private
    bool file_copy = false;  ///< Files copied in Explorer (CF_HDROP); no image is read
    std::vector<std::wstring> files;  ///< Their full paths, in the order copied
    bool files_through_ole = false;  ///< The clipboard listed only an OLE data object, which gave the files (Windows 11's File Explorer)
    bool image_not_sent = false;  ///< An image was offered but isn't in items (a text copy, or it couldn't be converted)
    std::vector<clipbundle::item> items;  ///< In sending order
    std::vector<std::string> formats;  ///< Every format offered, for debug logs
  };

  /** The clipboard's sequence number; 0 if it isn't available */
  std::uint32_t sequence();

  /** Reads the clipboard. Returns false if it couldn't be opened (another app held it). */
  bool read(snapshot &out);

  /**
   * Replaces the clipboard with the items, in their order, plus a bitmap copy of images for apps
   * that only paste bitmaps. With files (full paths), it also lists them as CF_HDROP with
   * "Preferred DropEffect" set to copy, so Explorer pastes copies. Returns the new sequence number,
   * or 0 if the clipboard couldn't be written.
   */
  std::uint32_t write(const std::vector<clipbundle::item> &items, bool concealed, const std::vector<std::wstring> &files = {});
}  // namespace fork_tweaks::clipboard_win

#endif
