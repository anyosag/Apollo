// Smoke test for src/fork/clipboard_win.cpp against the real Windows clipboard. A standalone program
// for the fork's CI step; prints "skipped" and succeeds if the runner has no usable clipboard:
//   g++ -std=c++17 -DCLIPBUNDLE_WITH_ZLIB -Isrc/fork tests/fork/clipboard_win_smoke.cpp
//       src/fork/clipboard_win.cpp src/fork/clipbundle.cpp -lz -lole32 -luser32 -o clipboard_win_smoke

#include "clipboard_win.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <ole2.h>
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

// CF_HDROP's memory for the paths, as Explorer writes it (DROPFILES, then the paths)
static std::string drop_list(const std::vector<std::wstring> &files) {
  struct {
    DWORD offset;
    POINT point;
    BOOL non_client;
    BOOL wide;
  } header {20, {0, 0}, FALSE, TRUE};
  static_assert(sizeof(header) == 20, "DROPFILES is 20 bytes");
  std::string out(reinterpret_cast<const char *>(&header), sizeof(header));
  for (const std::wstring &file : files) {
    out.append(reinterpret_cast<const char *>(file.c_str()), (file.size() + 1) * sizeof(wchar_t));
  }
  out.append(sizeof(wchar_t), '\0');
  return out;
}

// The COM interfaces used below, so nothing more needs linking
static const GUID k_iid_unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID k_iid_data_object = {0x0000010e, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID k_iid_enum_formats = {0x00000103, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

// An enumerator of no formats
class no_formats: public IEnumFORMATETC {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (IsEqualGUID(riid, k_iid_unknown) || IsEqualGUID(riid, k_iid_enum_formats)) {
      *out = static_cast<IEnumFORMATETC *>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG refs = InterlockedDecrement(&m_refs);
    if (refs == 0) {
      delete this;
    }
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG, FORMATETC *, ULONG *fetched) override {
    if (fetched != nullptr) {
      *fetched = 0;
    }
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC **out) override {
    *out = new no_formats();
    return S_OK;
  }

private:
  LONG m_refs = 1;
};

// What Windows 11's File Explorer puts on the clipboard: an OLE data object that lists no formats,
// so the clipboard shows only OLE's own ("DataObject"), and gives CF_HDROP when asked for it
class explorer_copy: public IDataObject {
public:
  explicit explorer_copy(std::string drop): m_drop(std::move(drop)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (IsEqualGUID(riid, k_iid_unknown) || IsEqualGUID(riid, k_iid_data_object)) {
      *out = static_cast<IDataObject *>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG refs = InterlockedDecrement(&m_refs);
    if (refs == 0) {
      delete this;
    }
    return refs;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
    if (format->cfFormat != CF_HDROP || (format->tymed & TYMED_HGLOBAL) == 0) {
      return DV_E_FORMATETC;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, m_drop.size());
    if (memory == nullptr) {
      return E_OUTOFMEMORY;
    }
    std::memcpy(GlobalLock(memory), m_drop.data(), m_drop.size());
    GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override { return format->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
    out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **out) override {
    *out = new no_formats();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **out) override {
    *out = nullptr;
    return OLE_E_ADVISENOTSUPPORTED;
  }

private:
  LONG m_refs = 1;
  std::string m_drop;
};

// Puts an explorer_copy on the clipboard from an OLE thread of its own, as Explorer does, and answers
// OLE's calls for it until `finished`
static void explorer_thread(std::string drop, HANDLE ready, HANDLE finished, bool &set) {
  if (FAILED(OleInitialize(nullptr))) {
    SetEvent(ready);
    return;
  }
  explorer_copy *copy = new explorer_copy(std::move(drop));
  set = SUCCEEDED(OleSetClipboard(copy));
  copy->Release();  // the clipboard holds its own reference
  SetEvent(ready);
  while (MsgWaitForMultipleObjects(1, &finished, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (set) {
    OleSetClipboard(nullptr);
  }
  OleUninitialize();
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

  // Bytes that aren't the declared format aren't written, or decoded into a bitmap
  std::string bmp = "BM" + std::string(60, '\0');
  CHECK(write({{"image/png", bmp}, {"text/plain", "caption"}}, false) != 0);
  snapshot mislabeled;
  CHECK(read(mislabeled));
  CHECK(!has_format(mislabeled, "PNG"));
  CHECK(!has_format(mislabeled, "CF_DIBV5"));
  CHECK(find(mislabeled, "text/plain") != nullptr);

  // A repeated type keeps the first item
  CHECK(write({{"text/plain", "first"}, {"text/plain", "second"}}, false) != 0);
  snapshot repeated;
  CHECK(read(repeated));
  const clipbundle::item *first = find(repeated, "text/plain");
  CHECK(first != nullptr && first->data == "first");

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

  // Files, as Explorer lists them, with a copy (not a move) asked for
  std::vector<std::wstring> files = {L"C:\\Users\\Public\\caf\u00e9.txt", L"C:\\Users\\Public\\A folder"};
  CHECK(write({{"text/plain", "caf\xC3\xA9.txt\nA folder"}}, false, files) != 0);
  snapshot copied;
  CHECK(read(copied));
  CHECK(copied.file_copy);
  CHECK(copied.files == files);
  CHECK(has_format(copied, "Preferred DropEffect"));
  CHECK(find(copied, "text/plain") != nullptr);
  CHECK(find(copied, "image/png") == nullptr);

  // Windows 11's File Explorer: the files come only from the clipboard's OLE data object, and a read
  // from a multithreaded apartment (like Apollo's clipboard worker) still finds them
  {
    std::vector<std::wstring> explorer_files = {L"C:\\Users\\Public\\From Explorer", L"C:\\Users\\Public\\b.txt"};
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool set = false;
    std::thread explorer(explorer_thread, drop_list(explorer_files), ready, finished, std::ref(set));
    WaitForSingleObject(ready, 10000);
    if (!set) {
      std::printf("OLE-only file copy skipped: OleSetClipboard isn't available here\n");
    } else {
      // A read that hangs fails the step instead of stalling CI
      snapshot from_explorer;
      bool read_ok = false;
      HANDLE read_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      std::thread reader([&]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        read_ok = read(from_explorer);
        CoUninitialize();
        SetEvent(read_done);
      });
      if (WaitForSingleObject(read_done, 15000) != WAIT_OBJECT_0) {
        std::printf("FAIL %s:%d: reading an OLE-only file copy didn't finish in 15 s\n", __FILE__, __LINE__);
        std::fflush(stdout);
        std::_Exit(1);
      }
      reader.join();
      CloseHandle(read_done);
      CHECK(read_ok);
      CHECK(has_format(from_explorer, "DataObject"));
      CHECK(from_explorer.files == explorer_files);
      CHECK(from_explorer.file_copy && from_explorer.files_through_ole);
      CHECK(from_explorer.items.empty());
    }
    SetEvent(finished);
    explorer.join();
    CloseHandle(ready);
    CloseHandle(finished);
  }

  // A copy that publishes its formats (text) is read as before, without asking OLE
  CHECK(write({{"text/plain", "after"}}, false) != 0);
  snapshot after;
  CHECK(read(after) && !after.files_through_ole && find(after, "text/plain") != nullptr);

  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All clipboard_win checks passed\n");
  return 0;
}
