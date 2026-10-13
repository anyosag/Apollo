// Smoke test for src/fork/clipboard_win.cpp against the real Windows clipboard. A standalone program
// for the fork's CI step; prints "skipped" and succeeds if the runner has no usable clipboard:
//   g++ -std=c++17 -DCLIPBUNDLE_WITH_ZLIB -Isrc/fork tests/fork/clipboard_win_smoke.cpp
//       src/fork/clipboard_win.cpp src/fork/clipbundle.cpp -lz -lole32 -luser32 -o clipboard_win_smoke

#include "clipboard_win.h"

#include <cstdio>
#include <string>

#include <windows.h>
#include <zlib.h>

using namespace fork_tweaks::clipboard_win;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void put_be32(std::string &out, std::uint32_t v) {
  for (int i = 3; i >= 0; i--) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

static void chunk(std::string &png, const char *type, const std::string &data) {
  put_be32(png, static_cast<std::uint32_t>(data.size()));
  std::string body = std::string(type, 4) + data;
  png += body;
  put_be32(png, crc32(0, reinterpret_cast<const Bytef *>(body.data()), static_cast<uInt>(body.size())));
}

// A 2x1 RGBA PNG: one half-transparent red pixel, one opaque blue pixel
static std::string make_png() {
  std::string png("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  put_be32(ihdr, 2);
  put_be32(ihdr, 1);
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);  // 8-bit RGBA
  chunk(png, "IHDR", ihdr);

  std::string row("\x00\xff\x00\x00\x80\x00\x00\xff\xff", 9);  // filter 0, then RGBA RGBA
  uLongf size = compressBound(static_cast<uLong>(row.size()));
  std::string idat(size, '\0');
  compress(reinterpret_cast<Bytef *>(&idat[0]), &size, reinterpret_cast<const Bytef *>(row.data()), static_cast<uLong>(row.size()));
  idat.resize(size);
  chunk(png, "IDAT", idat);
  chunk(png, "IEND", "");
  return png;
}

static const clipbundle::item *find(const snapshot &s, const char *type) {
  for (const clipbundle::item &it : s.items) {
    if (it.type == type) {
      return &it;
    }
  }
  return nullptr;
}

static bool has_format(const snapshot &s, const char *name) {
  for (const std::string &format : s.formats) {
    if (format == name) {
      return true;
    }
  }
  return false;
}

int main() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  // Formatted text
  std::uint32_t sequence = write({{"text/rtf", "{\\rtf1 \\b caf\\'e9\\b0}"}, {"text/html", "<b>caf\xC3\xA9</b>"}, {"text/plain", "caf\xC3\xA9\nline 2"}}, false);
  if (sequence == 0) {
    std::printf("skipped: the clipboard isn't available here\n");
    return 0;
  }

  snapshot text;
  CHECK(read(text));
  CHECK(text.sequence == sequence);
  CHECK(sequence == fork_tweaks::clipboard_win::sequence());  // unchanged without writes
  const clipbundle::item *plain = find(text, "text/plain");
  const clipbundle::item *html = find(text, "text/html");
  const clipbundle::item *rtf = find(text, "text/rtf");
  CHECK(plain != nullptr && plain->data == "caf\xC3\xA9\nline 2");
  CHECK(html != nullptr && html->data.find("<b>caf\xC3\xA9</b>") != std::string::npos);
  CHECK(rtf != nullptr && rtf->data == "{\\rtf1 \\b caf\\'e9\\b0}");
  CHECK(!text.items.empty() && text.items.back().type == "text/plain");
  CHECK(find(text, "image/png") == nullptr);

  // An image copy: PNG plus a bitmap for apps that only paste bitmaps
  std::string png = make_png();
  CHECK(write({{"image/png", png}}, false) != 0);
  snapshot image;
  CHECK(read(image));
  const clipbundle::item *read_png = find(image, "image/png");
  CHECK(read_png != nullptr && read_png->data == png);
  CHECK(has_format(image, "PNG"));
  CHECK(has_format(image, "CF_DIBV5"));

  // A bitmap-only copy (like Print Screen) arrives as PNG, opaque even though alpha is 0
  std::string dib(40 + 8, '\0');
  dib[0] = 40;
  dib[4] = 2;  // width
  dib[8] = 1;  // height
  dib[12] = 1;  // planes
  dib[14] = 32;  // bits per pixel, BI_RGB
  dib[40] = '\xff';  // blue pixel, alpha 0
  dib[46] = '\xff';  // red pixel, alpha 0
  if (OpenClipboard(nullptr)) {
    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, dib.size());
    memcpy(GlobalLock(memory), dib.data(), dib.size());
    GlobalUnlock(memory);
    SetClipboardData(CF_DIB, memory);
    CloseClipboard();

    snapshot bitmap;
    CHECK(read(bitmap));
    const clipbundle::item *converted = find(bitmap, "image/png");
    CHECK(converted != nullptr && converted->data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0);
    // IHDR: width 2, height 1, 8-bit RGBA
    CHECK(converted != nullptr && converted->data.size() > 26 && converted->data[19] == 2 && converted->data[23] == 1 && converted->data[25] == 6);
  }

  // Private content is marked and detected
  CHECK(write({{"text/plain", "secret"}}, true) != 0);
  snapshot secret;
  CHECK(read(secret) && secret.concealed);

  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All clipboard_win checks passed\n");
  return 0;
}
