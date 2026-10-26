/**
 * @file src/fork/clipboard_win.cpp
 * @brief Fork-only: reads and writes the Windows clipboard in clipbundle's formats.
 */
#ifdef _WIN32

// standard includes
  #include <algorithm>
  #include <cstring>
  #include <thread>
  #include <utility>

// platform includes
  #include <windows.h>
  #include <objbase.h>
  #include <ole2.h>
  // Defines the WIC GUIDs here, so windowscodecs doesn't need linking
  #include <initguid.h>
  #include <wincodec.h>

// local includes
  #include "clipboard_win.h"

namespace fork_tweaks::clipboard_win {
  namespace {
    constexpr int k_open_attempts = 10;
    constexpr std::uint64_t k_max_pixels = 40'000'000;

    // Releases a COM pointer when it goes out of scope
    template<class T>
    class com_ptr {
    public:
      com_ptr() = default;
      com_ptr(const com_ptr &) = delete;
      com_ptr &operator=(const com_ptr &) = delete;

      ~com_ptr() {
        if (m_ptr != nullptr) {
          m_ptr->Release();
        }
      }

      T **put() {
        return &m_ptr;
      }

      T *get() const {
        return m_ptr;
      }

      T *operator->() const {
        return m_ptr;
      }

      explicit operator bool() const {
        return m_ptr != nullptr;
      }

    private:
      T *m_ptr = nullptr;
    };

    struct pixels_bgra {
      UINT width = 0;
      UINT height = 0;
      std::string data;  ///< Straight (not premultiplied) BGRA, top row first
    };

    /// CF_HDROP's header (DROPFILES in shlobj.h): the paths follow, at files_offset
    struct drop_files_t {
      DWORD files_offset;
      POINT point;
      BOOL non_client;
      BOOL wide;
    };

    static_assert(sizeof(drop_files_t) == 20, "DROPFILES is 20 bytes");

    /// DROPEFFECT_COPY, for "Preferred DropEffect"
    constexpr DWORD k_drop_effect_copy = 1;

    /// The paths in CF_HDROP's memory: NUL-terminated strings after the header, ending with an empty one
    std::vector<std::wstring> dropped_files(const std::string &raw) {
      std::vector<std::wstring> files;
      drop_files_t header {};
      if (raw.size() < sizeof(header)) {
        return files;
      }
      std::memcpy(&header, raw.data(), sizeof(header));
      if (header.files_offset < sizeof(header) || header.files_offset >= raw.size()) {
        return files;
      }

      if (header.wide) {
        std::size_t count = (raw.size() - header.files_offset) / sizeof(wchar_t);
        std::wstring all(count, L'\0');
        std::memcpy(&all[0], raw.data() + header.files_offset, count * sizeof(wchar_t));
        std::size_t start = 0;
        while (start < all.size()) {
          std::size_t end = all.find(L'\0', start);
          if (end == std::wstring::npos || end == start) {
            break;
          }
          files.push_back(all.substr(start, end - start));
          start = end + 1;
        }
        return files;
      }

      // An old app's ANSI list
      std::string all = raw.substr(header.files_offset);
      std::size_t start = 0;
      while (start < all.size()) {
        std::size_t end = all.find('\0', start);
        if (end == std::string::npos || end == start) {
          break;
        }
        std::string path = all.substr(start, end - start);
        int size = MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
        std::wstring wide(size, L'\0');
        MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), &wide[0], size);
        files.push_back(std::move(wide));
        start = end + 1;
      }
      return files;
    }

    /**
     * CF_HDROP asked of the OLE data object on the clipboard. Windows 11's File Explorer copies files
     * as that object alone: the clipboard lists only "DataObject", and the file list comes only from
     * the object itself (OleGetClipboard()), as Explorer's own paste gets it. OLE needs a
     * single-threaded apartment and the clipboard worker's is multithreaded, so it's asked from a
     * thread of its own. Call it with the clipboard closed: OleGetClipboard() opens it.
     */
    std::vector<std::wstring> ole_dropped_files() {
      std::vector<std::wstring> files;
      std::thread([&files]() {
        if (FAILED(OleInitialize(nullptr))) {
          return;
        }
        IDataObject *data = nullptr;
        if (SUCCEEDED(OleGetClipboard(&data)) && data != nullptr) {
          FORMATETC format {static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
          STGMEDIUM medium {};
          if (SUCCEEDED(data->GetData(&format, &medium))) {
            if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal != nullptr) {
              SIZE_T size = GlobalSize(medium.hGlobal);
              const void *memory = GlobalLock(medium.hGlobal);
              if (memory != nullptr) {
                files = dropped_files(std::string(static_cast<const char *>(memory), size));
                GlobalUnlock(medium.hGlobal);
              }
            }
            ReleaseStgMedium(&medium);
          }
          data->Release();
        }
        OleUninitialize();
      }).join();
      return files;
    }

    /// CF_HDROP's memory for the paths, as Explorer writes it
    std::string drop_files(const std::vector<std::wstring> &files) {
      drop_files_t header {};
      header.files_offset = sizeof(header);
      header.wide = TRUE;
      std::string out(reinterpret_cast<const char *>(&header), sizeof(header));
      for (const std::wstring &file : files) {
        out.append(reinterpret_cast<const char *>(file.c_str()), (file.size() + 1) * sizeof(wchar_t));
      }
      out.append(sizeof(wchar_t), '\0');
      return out;
    }

    bool open_clipboard() {
      for (int attempt = 0; attempt < k_open_attempts; attempt++) {
        if (OpenClipboard(nullptr)) {
          return true;
        }
        Sleep(10 * (attempt + 1));
      }
      return false;
    }

    UINT registered(const char *name) {
      return RegisterClipboardFormatA(name);
    }

    std::string format_name(UINT format) {
      wchar_t buffer[256];
      int length = GetClipboardFormatNameW(format, buffer, 256);
      if (length > 0) {
        int size = WideCharToMultiByte(CP_UTF8, 0, buffer, length, nullptr, 0, nullptr, nullptr);
        std::string name(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer, length, &name[0], size, nullptr, nullptr);
        return name;
      }
      switch (format) {
        case CF_TEXT:
          return "CF_TEXT";
        case CF_BITMAP:
          return "CF_BITMAP";
        case CF_METAFILEPICT:
          return "CF_METAFILEPICT";
        case CF_OEMTEXT:
          return "CF_OEMTEXT";
        case CF_DIB:
          return "CF_DIB";
        case CF_UNICODETEXT:
          return "CF_UNICODETEXT";
        case CF_ENHMETAFILE:
          return "CF_ENHMETAFILE";
        case CF_HDROP:
          return "CF_HDROP";
        case CF_LOCALE:
          return "CF_LOCALE";
        case CF_DIBV5:
          return "CF_DIBV5";
        default:
          return "#" + std::to_string(format);
      }
    }

    // Copies a clipboard format's memory. The clipboard must be open.
    bool read_global(UINT format, std::string &out) {
      HANDLE handle = GetClipboardData(format);
      if (handle == nullptr) {
        return false;
      }
      SIZE_T size = GlobalSize(handle);
      const void *data = GlobalLock(handle);
      if (data == nullptr) {
        return false;
      }
      out.assign(static_cast<const char *>(data), size);
      GlobalUnlock(handle);
      return true;
    }

    std::string utf16_to_utf8(const wchar_t *text, std::size_t length) {
      if (length == 0) {
        return {};
      }
      int size = WideCharToMultiByte(CP_UTF8, 0, text, int(length), nullptr, 0, nullptr, nullptr);
      std::string out(size, '\0');
      WideCharToMultiByte(CP_UTF8, 0, text, int(length), &out[0], size, nullptr, nullptr);
      return out;
    }

    std::wstring utf8_to_utf16(const std::string &text) {
      if (text.empty()) {
        return {};
      }
      int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
      std::wstring out(size, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), &out[0], size);
      return out;
    }

    clipbundle::kind kind_of(UINT format, const std::string &name) {
      switch (format) {
        case CF_UNICODETEXT:
        case CF_TEXT:
        case CF_OEMTEXT:
          return clipbundle::kind::text;
        case CF_DIB:
        case CF_DIBV5:
        case CF_BITMAP:
        case CF_ENHMETAFILE:
        case CF_METAFILEPICT:
          return clipbundle::kind::image;
        default:
          break;
      }
      if (name == "Rich Text Format" || name == "HTML Format") {
        return clipbundle::kind::text;
      }
      if (name == "PNG" || name == "JFIF" || name == "GIF") {
        return clipbundle::kind::image;
      }
      return clipbundle::kind::other;
    }

    IWICImagingFactory *wic() {
      // One factory per thread that asks; the clipboard worker is the only one
      thread_local com_ptr<IWICImagingFactory> factory;
      if (!factory) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void **>(factory.put()));
      }
      return factory.get();
    }

    // The container an image's first bytes announce, if it's one decode_image accepts
    const GUID *container_of(const std::string &encoded) {
      static constexpr unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
      static constexpr unsigned char jpeg[] = {0xFF, 0xD8, 0xFF};
      auto starts_with = [&encoded](const unsigned char *magic, std::size_t size) {
        return encoded.size() >= size && std::memcmp(encoded.data(), magic, size) == 0;
      };
      if (starts_with(png, sizeof(png))) {
        return &GUID_ContainerFormatPng;
      }
      if (starts_with(jpeg, sizeof(jpeg))) {
        return &GUID_ContainerFormatJpeg;
      }
      if (encoded.size() >= 2 && encoded[0] == 'B' && encoded[1] == 'M') {
        return &GUID_ContainerFormatBmp;
      }
      return nullptr;
    }

    // Decodes a BMP, PNG or JPEG to straight BGRA. The bytes must be the format the caller expects, and
    // only Windows' own decoder for it is used: images come from the client, and WIC would otherwise
    // try every installed codec.
    bool decode_image(const std::string &encoded, const GUID &container, pixels_bgra &out) {
      IWICImagingFactory *factory = wic();
      const GUID *announced = container_of(encoded);
      if (factory == nullptr || announced == nullptr || !IsEqualGUID(*announced, container)) {
        return false;
      }

      com_ptr<IWICStream> stream;
      com_ptr<IWICBitmapDecoder> decoder;
      com_ptr<IWICBitmapFrameDecode> frame;
      com_ptr<IWICFormatConverter> converter;
      if (FAILED(factory->CreateStream(stream.put())) ||
          FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(const_cast<char *>(encoded.data())), DWORD(encoded.size()))) ||
          FAILED(factory->CreateDecoder(container, &GUID_VendorMicrosoft, decoder.put())) ||
          FAILED(decoder->Initialize(stream.get(), WICDecodeMetadataCacheOnDemand)) ||
          FAILED(decoder->GetFrame(0, frame.put())) ||
          FAILED(factory->CreateFormatConverter(converter.put())) ||
          FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
          FAILED(converter->GetSize(&out.width, &out.height))) {
        return false;
      }
      if (out.width == 0 || out.height == 0 || std::uint64_t(out.width) * out.height > k_max_pixels) {
        return false;
      }

      UINT stride = out.width * 4;
      out.data.resize(std::size_t(stride) * out.height);
      return SUCCEEDED(converter->CopyPixels(nullptr, stride, UINT(out.data.size()), reinterpret_cast<BYTE *>(&out.data[0])));
    }

    std::string encode_png(const pixels_bgra &image) {
      IWICImagingFactory *factory = wic();
      if (factory == nullptr) {
        return {};
      }

      com_ptr<IWICBitmap> bitmap;
      com_ptr<IStream> stream;
      com_ptr<IWICBitmapEncoder> encoder;
      com_ptr<IWICBitmapFrameEncode> frame;
      WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
      UINT stride = image.width * 4;
      if (FAILED(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA, stride, UINT(image.data.size()), reinterpret_cast<BYTE *>(const_cast<char *>(image.data.data())), bitmap.put())) ||
          FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) ||
          FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
          FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
          FAILED(encoder->CreateNewFrame(frame.put(), nullptr)) ||
          FAILED(frame->Initialize(nullptr)) ||
          FAILED(frame->SetSize(image.width, image.height)) ||
          FAILED(frame->SetPixelFormat(&format)) ||
          FAILED(frame->WriteSource(bitmap.get(), nullptr)) ||
          FAILED(frame->Commit()) ||
          FAILED(encoder->Commit())) {
        return {};
      }

      STATSTG stat = {};
      HGLOBAL memory = nullptr;
      if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.get(), &memory))) {
        return {};
      }
      const void *data = GlobalLock(memory);
      if (data == nullptr) {
        return {};
      }
      std::string png(static_cast<const char *>(data), std::size_t(stat.cbSize.QuadPart));
      GlobalUnlock(memory);
      return png;
    }

    // A bitmap from the clipboard (CF_DIB / CF_DIBV5) as PNG
    std::string dib_to_png(const std::string &dib) {
      std::string bmp = clipbundle::dib_to_bmp(dib);
      pixels_bgra image;
      if (bmp.empty() || !decode_image(bmp, GUID_ContainerFormatBmp, image)) {
        return {};
      }

      // 32-bit bitmaps often leave alpha at 0 without meaning transparency
      bool any_alpha = false;
      for (std::size_t i = 3; i < image.data.size(); i += 4) {
        if (image.data[i] != 0) {
          any_alpha = true;
          break;
        }
      }
      if (!any_alpha) {
        for (std::size_t i = 3; i < image.data.size(); i += 4) {
          image.data[i] = char(0xFF);
        }
      }
      return encode_png(image);
    }

    // A PNG or JPEG as CF_DIBV5 (32-bit with alpha, sRGB), for apps that only paste bitmaps
    std::string image_to_dibv5(const std::string &encoded, const GUID &container) {
      pixels_bgra image;
      if (!decode_image(encoded, container, image)) {
        return {};
      }

      BITMAPV5HEADER header = {};
      header.bV5Size = sizeof(BITMAPV5HEADER);
      header.bV5Width = LONG(image.width);
      header.bV5Height = LONG(image.height);  // bottom-up rows
      header.bV5Planes = 1;
      header.bV5BitCount = 32;
      header.bV5Compression = BI_BITFIELDS;
      header.bV5SizeImage = DWORD(image.data.size());
      header.bV5RedMask = 0x00FF0000;
      header.bV5GreenMask = 0x0000FF00;
      header.bV5BlueMask = 0x000000FF;
      header.bV5AlphaMask = 0xFF000000;
      header.bV5CSType = LCS_sRGB;
      header.bV5Intent = LCS_GM_IMAGES;

      std::string dib(reinterpret_cast<const char *>(&header), sizeof(header));
      std::size_t stride = std::size_t(image.width) * 4;
      dib.reserve(dib.size() + image.data.size());
      for (UINT row = image.height; row-- > 0;) {
        dib.append(image.data, row * stride, stride);
      }
      return dib;
    }

    HGLOBAL make_global(const void *data, std::size_t size) {
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size > 0 ? size : 1);
      if (memory == nullptr) {
        return nullptr;
      }
      void *target = GlobalLock(memory);
      if (target == nullptr) {
        GlobalFree(memory);
        return nullptr;
      }
      if (size > 0) {
        std::memcpy(target, data, size);
      } else {
        *static_cast<char *>(target) = 0;
      }
      GlobalUnlock(memory);
      return memory;
    }
  }  // namespace

  std::uint32_t sequence() {
    return GetClipboardSequenceNumber();
  }

  bool read(snapshot &out) {
    out = snapshot {};
    out.sequence = GetClipboardSequenceNumber();
    if (!open_clipboard()) {
      return false;
    }

    struct offered {
      UINT format;
      std::string name;
      clipbundle::kind kind;
    };

    std::vector<offered> formats;
    bool ole_object = false;  // an OLE data object is on the clipboard
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
      std::string name = format_name(format);
      formats.push_back({format, name, kind_of(format, name)});
      out.formats.push_back(name);
      if (name == "ExcludeClipboardContentFromMonitorProcessing" || name == "Clipboard Viewer Ignore") {
        out.concealed = true;
      }
      if (format == CF_HDROP) {
        out.file_copy = true;
      }
      if (name == "DataObject") {
        ole_object = true;
      }
    }

    if (out.file_copy) {
      std::string drop;
      if (read_global(CF_HDROP, drop)) {
        out.files = dropped_files(drop);
      }
    }

    std::vector<clipbundle::kind> kinds;
    bool image_offered = false;
    for (const offered &f : formats) {
      kinds.push_back(f.kind);
      image_offered = image_offered || f.kind == clipbundle::kind::image;
    }

    std::vector<std::pair<std::size_t, clipbundle::item>> items;
    std::size_t budget = clipbundle::k_max_raw_bytes;
    auto add = [&items, &budget](std::size_t position, const char *type, std::string data) {
      if (data.size() > budget) {
        return;
      }
      budget -= data.size();
      items.push_back({position, clipbundle::item {type, std::move(data)}});
    };

    // Plain text first: it's always sent, even for file copies
    std::string raw;
    std::string plain_text;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) && read_global(CF_UNICODETEXT, raw)) {
      const wchar_t *text = reinterpret_cast<const wchar_t *>(raw.data());
      std::size_t length = 0;
      while (length < raw.size() / 2 && text[length] != L'\0') {
        length++;
      }
      plain_text = clipbundle::to_lf(utf16_to_utf8(text, length));
      if (!plain_text.empty()) {
        add(formats.size(), "text/plain", plain_text);
      }
    }

    // The HTML too, which tells a picture copy (a browser's <img> tag) from a text copy
    std::string html;
    bool have_html = false;
    for (const offered &f : formats) {
      if (f.name == "HTML Format") {
        have_html = read_global(f.format, raw) && clipbundle::cf_html_parse(raw, html);
        break;
      }
    }

    // Decided before reading any bitmap, so an app isn't asked to render one that won't be sent
    bool want_image = !out.file_copy && clipbundle::include_image(kinds, plain_text, html);
    bool image_first = want_image;

    std::string dib;
    std::size_t dib_position = 0;
    bool have_encoded_image = false;
    if (!out.file_copy) {
      for (std::size_t i = 0; i < formats.size(); i++) {
        const offered &f = formats[i];
        if (f.format == CF_DIBV5 || f.format == CF_DIB) {
          if (!want_image || have_encoded_image || !dib.empty()) {
            continue;
          }
          // The first bitmap offered is the app's own; ones Windows synthesizes come later
          if (read_global(f.format, dib)) {
            dib_position = i;
          }
          continue;
        }

        const clipbundle::type_info *info = clipbundle::find_windows(f.name);
        if (info == nullptr || std::strcmp(info->type, "text/plain") == 0) {
          continue;
        }
        bool image = clipbundle::is_image_type(info->type);
        if (image && (!want_image || have_encoded_image)) {
          continue;
        }

        std::string type = info->type;
        if (type == "text/html") {
          // Already read for the image rule
          if (have_html) {
            add(i, info->type, std::move(html));
            have_html = false;
          }
          continue;
        }
        if (!read_global(f.format, raw)) {
          continue;
        }

        if (type == "text/rtf") {
          add(i, info->type, clipbundle::strip_trailing_nuls(raw));
        } else if (type == "image/png") {
          raw.resize(clipbundle::png_length(raw));
          add(i, info->type, std::move(raw));
          have_encoded_image = true;
        } else if (type == "image/jpeg") {
          add(i, info->type, std::move(raw));
          have_encoded_image = true;
        } else if (type == "ms/gvml") {
          raw.resize(clipbundle::zip_length(raw));
          add(i, info->type, std::move(raw));
        } else {
          add(i, info->type, std::move(raw));
        }
      }
    }
    CloseClipboard();

    // Files that only the OLE data object lists (Windows 11's File Explorer), or a CF_HDROP that
    // couldn't be read: asked of the object. Not for copies that offered anything else, like Office's.
    if (out.files.empty() && items.empty() && dib.empty() && (ole_object || out.file_copy)) {
      out.files = ole_dropped_files();
      out.files_through_ole = !out.files.empty();
      out.file_copy = out.file_copy || out.files_through_ole;
    }

    // Converting happens after closing, so the clipboard isn't held while encoding
    if (!dib.empty() && !have_encoded_image) {
      std::string png = dib_to_png(dib);
      if (!png.empty()) {
        add(dib_position, "image/png", std::move(png));
      }
    }

    std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
      return a.first < b.first;
    });
    for (auto &entry : items) {
      out.items.push_back(std::move(entry.second));
    }
    clipbundle::prepare_for_send(out.items, image_first);

    bool image_sent = std::any_of(out.items.begin(), out.items.end(), [](const clipbundle::item &it) {
      return clipbundle::is_image_type(it.type);
    });
    out.image_not_sent = image_offered && !out.file_copy && !image_sent;
    return true;
  }

  std::uint32_t write(const std::vector<clipbundle::item> &items, bool concealed, const std::vector<std::wstring> &files) {
    struct entry {
      UINT format;
      HGLOBAL memory;
    };

    std::vector<entry> entries;
    auto add = [&entries](UINT format, const void *data, std::size_t size) {
      if (format == 0) {
        return;
      }
      HGLOBAL memory = make_global(data, size);
      if (memory != nullptr) {
        entries.push_back({format, memory});
      }
    };
    auto add_string = [&add](UINT format, const std::string &data) {
      // Text formats end with a NUL
      add(format, data.c_str(), data.size() + 1);
    };

    // Everything is prepared before opening the clipboard, so it's held as briefly as possible
    std::vector<std::string> seen;
    bool have_dib = false;
    for (const clipbundle::item &it : items) {
      // One item per type, since a repeated type would only replace the first, and images must be
      // the format they claim to be
      if (std::find(seen.begin(), seen.end(), it.type) != seen.end() || !clipbundle::data_matches_type(it.type, it.data)) {
        continue;
      }
      seen.push_back(it.type);

      if (it.type == "text/plain") {
        std::wstring text = utf8_to_utf16(clipbundle::to_crlf(it.data));
        add(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
      } else if (it.type == "text/rtf") {
        add_string(registered("Rich Text Format"), it.data);
      } else if (it.type == "text/html") {
        add_string(registered("HTML Format"), clipbundle::cf_html_build(it.data));
      } else if (it.type == "image/png" || it.type == "image/jpeg") {
        add(registered(it.type == "image/png" ? "PNG" : "JFIF"), it.data.data(), it.data.size());
        // The clipboard holds one bitmap, so only the first image is decoded for it
        if (!have_dib) {
          std::string dib = image_to_dibv5(it.data, it.type == "image/png" ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg);
          if (!dib.empty()) {
            add(CF_DIBV5, dib.data(), dib.size());
            have_dib = true;
          }
        }
      } else if (const clipbundle::type_info *info = clipbundle::find_type(it.type); info != nullptr && info->windows_name != nullptr) {
        add(registered(info->windows_name), it.data.data(), it.data.size());
      }
    }

    if (!files.empty()) {
      std::string drop = drop_files(files);
      add(CF_HDROP, drop.data(), drop.size());
      add(registered("Preferred DropEffect"), &k_drop_effect_copy, sizeof(k_drop_effect_copy));
    }

    if (concealed) {
      // Kept out of clipboard history, cloud sync and clipboard managers
      DWORD no = 0;
      add(registered("ExcludeClipboardContentFromMonitorProcessing"), &no, sizeof(no));
      add(registered("CanIncludeInClipboardHistory"), &no, sizeof(no));
      add(registered("CanUploadToCloudClipboard"), &no, sizeof(no));
    }

    auto free_all = [&entries]() {
      for (const entry &e : entries) {
        GlobalFree(e.memory);
      }
    };
    if (entries.empty()) {
      return 0;
    }
    if (!open_clipboard()) {
      free_all();
      return 0;
    }
    if (!EmptyClipboard()) {
      CloseClipboard();
      free_all();
      return 0;
    }

    bool any = false;
    for (const entry &e : entries) {
      if (SetClipboardData(e.format, e.memory) != nullptr) {
        any = true;  // the clipboard owns the memory now
      } else {
        GlobalFree(e.memory);
      }
    }
    CloseClipboard();
    return any ? GetClipboardSequenceNumber() : 0;
  }
}  // namespace fork_tweaks::clipboard_win

#endif
