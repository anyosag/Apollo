/**
 * @file clipbundle.cpp
 * @brief The clipboard bundle exchanged between LuzLunar and this Apollo build.
 *
 * Shared, byte for byte, by anyosag/Apollo (src/fork/clipbundle.*) and anyosag/LuzLunar
 * (app/fork/clipbundle.*). Keep both copies identical.
 */
#include "clipbundle.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef CLIPBUNDLE_WITH_ZLIB
  #include <zlib.h>
#endif

namespace clipbundle {

  namespace {
    constexpr char k_magic[4] = {'L', 'L', 'C', 'B'};
    constexpr std::uint8_t k_enc_raw = 0;
    constexpr std::uint8_t k_enc_zlib = 1;
    constexpr std::size_t k_min_compress = 1024;
    constexpr std::uint64_t k_max_zlib_ratio = 1032;  ///< Deflate can't expand data more than this

    const char *const k_start_fragment = "<!--StartFragment-->";
    const char *const k_end_fragment = "<!--EndFragment-->";

    void put_u8(std::string &out, std::uint8_t v) {
      out.push_back(static_cast<char>(v));
    }

    void put_u16(std::string &out, std::uint16_t v) {
      for (int i = 0; i < 2; i++) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
      }
    }

    void put_u32(std::string &out, std::uint32_t v) {
      for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
      }
    }

    void put_u64(std::string &out, std::uint64_t v) {
      for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
      }
    }

    std::uint32_t get_le(std::string_view data, std::size_t pos, int bytes) {
      std::uint32_t v = 0;
      for (int i = 0; i < bytes; i++) {
        v |= std::uint32_t(static_cast<unsigned char>(data[pos + i])) << (8 * i);
      }
      return v;
    }

    std::uint32_t get_be32(std::string_view data, std::size_t pos) {
      std::uint32_t v = 0;
      for (int i = 0; i < 4; i++) {
        v = (v << 8) | static_cast<unsigned char>(data[pos + i]);
      }
      return v;
    }

    // Reads the wire format, failing on anything past the end
    class reader {
    public:
      explicit reader(std::string_view data):
          m_data(data) {}

      bool u8(std::uint8_t &v) {
        if (!need(1)) {
          return false;
        }
        v = static_cast<std::uint8_t>(m_data[m_pos++]);
        return true;
      }

      bool u16(std::uint16_t &v) {
        if (!need(2)) {
          return false;
        }
        v = static_cast<std::uint16_t>(get_le(m_data, m_pos, 2));
        m_pos += 2;
        return true;
      }

      bool u32(std::uint32_t &v) {
        if (!need(4)) {
          return false;
        }
        v = get_le(m_data, m_pos, 4);
        m_pos += 4;
        return true;
      }

      bool u64(std::uint64_t &v) {
        std::uint32_t low, high;
        if (!u32(low) || !u32(high)) {
          return false;
        }
        v = (std::uint64_t(high) << 32) | low;
        return true;
      }

      bool bytes(std::size_t n, std::string_view &v) {
        if (!need(n)) {
          return false;
        }
        v = m_data.substr(m_pos, n);
        m_pos += n;
        return true;
      }

      bool done() const {
        return m_pos == m_data.size();
      }

    private:
      bool need(std::size_t n) const {
        return m_data.size() - m_pos >= n;
      }

      std::string_view m_data;
      std::size_t m_pos = 0;
    };

    bool fail(std::string *error, const char *reason) {
      if (error != nullptr) {
        *error = reason;
      }
      return false;
    }

    bool valid_token(std::string_view token) {
      return std::all_of(token.begin(), token.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
      });
    }

    bool valid_type(std::string_view type) {
      return !type.empty() && std::all_of(type.begin(), type.end(), [](char c) {
        return c > 0x20 && c < 0x7F;
      });
    }

    bool compressible(std::string_view type) {
      if (type.substr(0, 5) == "text/") {
        return true;
      }
      const type_info *info = find_type(type);
      return info != nullptr && info->compressible;
    }

    std::string lowercase(std::string_view text) {
      std::string out(text);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return out;
    }

    std::size_t find_ci(std::string_view haystack, std::string_view needle, std::size_t from = 0) {
      if (needle.size() > haystack.size()) {
        return std::string_view::npos;
      }
      for (std::size_t i = from; i + needle.size() <= haystack.size(); i++) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); j++) {
          if (std::tolower(static_cast<unsigned char>(haystack[i + j])) != std::tolower(static_cast<unsigned char>(needle[j]))) {
            match = false;
            break;
          }
        }
        if (match) {
          return i;
        }
      }
      return std::string_view::npos;
    }

    std::size_t rfind_ci(std::string_view haystack, std::string_view needle) {
      if (needle.size() > haystack.size()) {
        return std::string_view::npos;
      }
      for (std::size_t i = haystack.size() - needle.size() + 1; i-- > 0;) {
        if (find_ci(haystack.substr(i, needle.size()), needle) == 0) {
          return i;
        }
      }
      return std::string_view::npos;
    }

    bool is_space(char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    }

    // True if lower (lowercased HTML) has the tag <name at pos, like "<head>" but not "<header>"
    bool starts_tag(std::string_view lower, std::size_t pos, std::string_view name) {
      if (pos + 1 + name.size() > lower.size() || lower[pos] != '<' || lower.substr(pos + 1, name.size()) != name) {
        return false;
      }
      std::size_t after = pos + 1 + name.size();
      return after == lower.size() || lower[after] == '>' || lower[after] == '/' || is_space(lower[after]);
    }

    int group_of(std::string_view type, bool image_first) {
      if (is_office_type(type)) {
        return 0;
      }
      if (is_image_type(type)) {
        return image_first ? 1 : 3;
      }
      if (type == "text/plain") {
        return 4;
      }
      return 2;  // RTF and HTML
    }
  }  // namespace

  std::string encode(const bundle &b, bool compress) {
    std::string out(k_magic, sizeof(k_magic));
    put_u16(out, k_version);
    put_u16(out, b.flags);
    put_u64(out, b.etag);

    std::string token = b.token.substr(0, k_max_token);
    put_u8(out, static_cast<std::uint8_t>(token.size()));
    out += token;

    std::size_t count = std::min(b.items.size(), k_max_items);
    put_u8(out, static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; i++) {
      const item &it = b.items[i];
      std::string type = it.type.substr(0, k_max_type);
      put_u8(out, static_cast<std::uint8_t>(type.size()));
      out += type;

      std::uint8_t encoding = k_enc_raw;
      std::string stored;
#ifdef CLIPBUNDLE_WITH_ZLIB
      if (compress && it.data.size() >= k_min_compress && compressible(it.type)) {
        uLongf size = compressBound(static_cast<uLong>(it.data.size()));
        stored.resize(size);
        if (compress2(reinterpret_cast<Bytef *>(&stored[0]), &size, reinterpret_cast<const Bytef *>(it.data.data()), static_cast<uLong>(it.data.size()), 6) == Z_OK &&
            size <= it.data.size() / 10 * 9) {
          stored.resize(size);
          encoding = k_enc_zlib;
        }
      }
#else
      (void) compress;
#endif

      put_u8(out, encoding);
      put_u32(out, static_cast<std::uint32_t>(it.data.size()));
      if (encoding == k_enc_zlib) {
        put_u32(out, static_cast<std::uint32_t>(stored.size()));
        out += stored;
      } else {
        put_u32(out, static_cast<std::uint32_t>(it.data.size()));
        out += it.data;
      }
    }
    return out;
  }

  bool decode(std::string_view bytes, bundle &out, std::string *error) {
    out = bundle {};
    reader r(bytes);

    std::string_view magic;
    if (!r.bytes(4, magic) || magic != std::string_view(k_magic, 4)) {
      return fail(error, "not a clipboard bundle");
    }

    std::uint16_t version;
    if (!r.u16(version) || version != k_version) {
      return fail(error, "unsupported bundle version");
    }

    std::uint8_t token_size;
    std::string_view token;
    if (!r.u16(out.flags) || !r.u64(out.etag) || !r.u8(token_size) || token_size > k_max_token || !r.bytes(token_size, token) || !valid_token(token)) {
      return fail(error, "bad bundle header");
    }
    out.token = std::string(token);

    std::uint8_t count;
    if (!r.u8(count) || count > k_max_items) {
      return fail(error, "bad item count");
    }

    std::size_t total = 0;
    for (std::uint8_t i = 0; i < count; i++) {
      std::uint8_t type_size, encoding;
      std::string_view type, stored;
      std::uint32_t raw_size, stored_size;
      if (!r.u8(type_size) || type_size > k_max_type || !r.bytes(type_size, type) || !valid_type(type) ||
          !r.u8(encoding) || !r.u32(raw_size) || !r.u32(stored_size) || !r.bytes(stored_size, stored)) {
        return fail(error, "truncated or bad item");
      }

      total += raw_size;
      if (total > k_max_raw_bytes) {
        return fail(error, "bundle too large");
      }

      item it;
      it.type = std::string(type);
      if (encoding == k_enc_raw) {
        if (stored_size != raw_size) {
          return fail(error, "bad item size");
        }
        it.data = std::string(stored);
      } else if (encoding == k_enc_zlib) {
#ifdef CLIPBUNDLE_WITH_ZLIB
        // A raw size the stored bytes can't inflate to would allocate memory for nothing
        if (raw_size > stored_size * k_max_zlib_ratio) {
          return fail(error, "bad compressed item");
        }
        it.data.resize(raw_size);
        uLongf size = raw_size;
        int status = uncompress(reinterpret_cast<Bytef *>(raw_size > 0 ? &it.data[0] : nullptr), &size, reinterpret_cast<const Bytef *>(stored.data()), static_cast<uLong>(stored.size()));
        if (status != Z_OK || size != raw_size) {
          return fail(error, "bad compressed item");
        }
#else
        return fail(error, "compressed items aren't supported");
#endif
      } else {
        return fail(error, "unknown item encoding");
      }
      out.items.push_back(std::move(it));
    }

    if (!r.done()) {
      return fail(error, "extra bytes after the last item");
    }
    return true;
  }

  std::uint64_t etag(const std::vector<item> &items) {
    if (items.empty()) {
      return 0;
    }

    std::vector<const item *> sorted;
    for (const item &it : items) {
      sorted.push_back(&it);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const item *a, const item *b) {
      return a->type < b->type;
    });

    std::uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](const char *data, std::size_t size) {
      for (std::size_t i = 0; i < size; i++) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ull;
      }
    };
    for (const item *it : sorted) {
      std::string prefix = it->type;
      prefix.push_back('\0');
      put_u32(prefix, static_cast<std::uint32_t>(it->data.size()));
      mix(prefix.data(), prefix.size());
      mix(it->data.data(), it->data.size());
    }
    return hash != 0 ? hash : 1;
  }

  std::string etag_hex(std::uint64_t etag) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(etag));
    return buffer;
  }

  bool parse_etag_hex(std::string_view hex, std::uint64_t &etag) {
    if (hex.empty() || hex.size() > 16) {
      return false;
    }
    std::uint64_t v = 0;
    for (char c : hex) {
      int digit;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return false;
      }
      v = (v << 4) | std::uint64_t(digit);
    }
    etag = v;
    return true;
  }

  const std::vector<type_info> &types() {
    // Office formats are added only once they're confirmed to exist, with the same contents, on
    // both platforms. Art::GVML is Office's drawing format (shapes, charts, SmartArt): an OOXML zip.
    static const std::vector<type_info> table = {
      {"text/plain", nullptr, "public.utf8-plain-text", true},
      {"text/rtf", "Rich Text Format", "public.rtf", true},
      {"text/html", "HTML Format", "public.html", true},
      {"image/png", "PNG", "public.png", false},
      {"image/jpeg", "JFIF", "public.jpeg", false},
      {"ms/gvml", "Art::GVML ClipFormat", "com.microsoft.Art--GVML-ClipFormat", false},
    };
    return table;
  }

  const type_info *find_type(std::string_view type) {
    for (const type_info &info : types()) {
      if (type == info.type) {
        return &info;
      }
    }
    return nullptr;
  }

  const type_info *find_windows(std::string_view windows_name) {
    for (const type_info &info : types()) {
      if (info.windows_name != nullptr && windows_name == info.windows_name) {
        return &info;
      }
    }
    return nullptr;
  }

  const type_info *find_mac(std::string_view mac_type) {
    for (const type_info &info : types()) {
      if (mac_type == info.mac_type) {
        return &info;
      }
    }
    return nullptr;
  }

  bool is_text_type(std::string_view type) {
    return type == "text/plain" || type == "text/rtf" || type == "text/html";
  }

  bool is_image_type(std::string_view type) {
    return type.substr(0, 6) == "image/";
  }

  bool is_office_type(std::string_view type) {
    return type.substr(0, 3) == "ms/";
  }

  bool data_matches_type(std::string_view type, std::string_view data) {
    if (type == "image/png") {
      return data.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8);
    }
    if (type == "image/jpeg") {
      return data.substr(0, 3) == std::string_view("\xFF\xD8\xFF", 3);
    }
    return !is_image_type(type);
  }

  std::string to_lf(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); i++) {
      if (text[i] == '\r') {
        out.push_back('\n');
        if (i + 1 < text.size() && text[i + 1] == '\n') {
          i++;
        }
      } else {
        out.push_back(text[i]);
      }
    }
    return out;
  }

  std::string to_crlf(std::string_view text) {
    std::string lf = to_lf(text);
    std::string out;
    out.reserve(lf.size() + lf.size() / 16);
    for (char c : lf) {
      if (c == '\n') {
        out.push_back('\r');
      }
      out.push_back(c);
    }
    return out;
  }

  std::string strip_trailing_nuls(std::string_view data) {
    std::size_t end = data.size();
    while (end > 0 && data[end - 1] == '\0') {
      end--;
    }
    return std::string(data.substr(0, end));
  }

  std::string cf_html_build(std::string_view html_in) {
    std::string html(html_in);
    const std::size_t start_size = std::strlen(k_start_fragment);

    std::size_t start = html.find(k_start_fragment);
    std::size_t end = start == std::string::npos ? std::string::npos : html.find(k_end_fragment, start);
    if (end == std::string::npos) {
      std::size_t body = find_ci(html, "<body");
      std::size_t body_open_end = body == std::string::npos ? std::string::npos : html.find('>', body);
      std::size_t body_close = rfind_ci(html, "</body");
      if (body_open_end != std::string::npos && body_close != std::string::npos && body_close > body_open_end) {
        html.insert(body_close, k_end_fragment);
        html.insert(body_open_end + 1, k_start_fragment);
      } else {
        html = std::string("<html><body>") + k_start_fragment + html + k_end_fragment + "</body></html>";
      }
      start = html.find(k_start_fragment);
      end = html.find(k_end_fragment, start);
    }

    // Same 105-byte header as Chromium writes; the offsets are UTF-8 byte offsets
    constexpr std::size_t k_header_size = 105;
    char header[k_header_size + 1];
    std::snprintf(header, sizeof(header), "Version:0.9\r\nStartHTML:%010llu\r\nEndHTML:%010llu\r\nStartFragment:%010llu\r\nEndFragment:%010llu\r\n",
                  static_cast<unsigned long long>(k_header_size), static_cast<unsigned long long>(k_header_size + html.size()),
                  static_cast<unsigned long long>(k_header_size + start + start_size), static_cast<unsigned long long>(k_header_size + end));
    return std::string(header, k_header_size) + html;
  }

  bool cf_html_parse(std::string_view cf_html_in, std::string &html) {
    std::string cf = strip_trailing_nuls(cf_html_in);
    long start_html = -1, end_html = -1;

    // Header lines run until the first line that starts with markup
    std::size_t pos = 0;
    while (pos < cf.size() && cf[pos] != '<') {
      std::size_t line_end = cf.find('\n', pos);
      if (line_end == std::string::npos) {
        line_end = cf.size();
      }
      std::string line = cf.substr(pos, line_end - pos);
      std::size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string key = lowercase(line.substr(0, colon));
        long value = std::strtol(line.c_str() + colon + 1, nullptr, 10);
        if (key == "starthtml") {
          start_html = value;
        } else if (key == "endhtml") {
          end_html = value;
        }
      }
      pos = line_end + 1;
    }
    std::size_t markup = std::min(pos, cf.size());

    if (start_html >= long(markup) && end_html > start_html && std::size_t(end_html) <= cf.size()) {
      html = cf.substr(std::size_t(start_html), std::size_t(end_html - start_html));
    } else {
      html = cf.substr(markup);
    }
    return !html.empty();
  }

  bool html_has_local_file_images(std::string_view html) {
    std::string lower = lowercase(html);
    std::size_t pos = 0;
    while ((pos = lower.find("src", pos)) != std::string::npos) {
      std::size_t i = pos + 3;
      while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) {
        i++;
      }
      if (i < lower.size() && lower[i] == '=') {
        i++;
        while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) {
          i++;
        }
        if (i < lower.size() && (lower[i] == '"' || lower[i] == '\'')) {
          i++;
        }
        if (lower.compare(i, 5, "file:") == 0) {
          return true;
        }
      }
      pos += 3;
    }
    return false;
  }

  bool html_visible_text_empty(std::string_view html) {
    std::string lower = lowercase(html);
    std::string_view text(lower);
    std::size_t i = 0;
    while (i < text.size()) {
      if (text.compare(i, 4, "<!--") == 0) {
        std::size_t end = text.find("-->", i + 4);
        i = end == std::string_view::npos ? text.size() : end + 3;
        continue;
      }

      // Elements whose content isn't shown
      bool skipped = false;
      for (const char *element : {"head", "title", "style", "script"}) {
        if (starts_tag(text, i, element)) {
          std::size_t end = text.find(std::string("</") + element, i);
          std::size_t close = end == std::string_view::npos ? end : text.find('>', end);
          i = close == std::string_view::npos ? text.size() : close + 1;
          skipped = true;
          break;
        }
      }
      if (skipped) {
        continue;
      }

      unsigned char c = static_cast<unsigned char>(text[i]);
      if (c == '<') {
        std::size_t end = text.find('>', i);
        i = end == std::string_view::npos ? text.size() : end + 1;
        continue;
      }
      if (c == '&') {
        bool space = false;
        for (const char *entity : {"&nbsp;", "&#160;", "&#xa0;", "&#32;", "&#x20;"}) {
          if (text.compare(i, std::strlen(entity), entity) == 0) {
            i += std::strlen(entity);
            space = true;
            break;
          }
        }
        if (space) {
          continue;
        }
        return false;
      }
      if (is_space(text[i])) {
        i++;
        continue;
      }
      if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0) {
        i += 2;  // UTF-8 no-break space
        continue;
      }
      if (c == 0xEF && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xBB && static_cast<unsigned char>(text[i + 2]) == 0xBF) {
        i += 3;  // byte order mark
        continue;
      }
      return false;
    }
    return true;
  }

  bool picture_only_text(std::string_view text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && is_space(text[begin])) {
      begin++;
    }
    while (end > begin && is_space(text[end - 1])) {
      end--;
    }
    std::string_view trimmed = text.substr(begin, end - begin);
    if (trimmed.empty()) {
      return true;
    }
    if (std::any_of(trimmed.begin(), trimmed.end(), is_space)) {
      return false;
    }
    // A browser's "Copy image" may offer the picture's address as text
    std::string scheme = lowercase(trimmed.substr(0, 8));
    for (const char *prefix : {"http://", "https://", "data:", "file:", "blob:"}) {
      if (scheme.compare(0, std::strlen(prefix), prefix) == 0) {
        return true;
      }
    }
    return false;
  }

  std::string ensure_utf8_meta(std::string_view html) {
    std::size_t head_limit = find_ci(html, "<body");
    std::string_view head = html.substr(0, head_limit == std::string_view::npos ? std::min<std::size_t>(html.size(), 4096) : head_limit);
    if (find_ci(head, "charset") != std::string_view::npos) {
      return std::string(html);
    }

    const char *meta = "<meta charset=\"utf-8\">";
    std::string out(html);
    std::size_t head_tag = find_ci(out, "<head");
    std::size_t head_tag_end = head_tag == std::string::npos ? std::string::npos : out.find('>', head_tag);
    if (head_tag_end != std::string::npos) {
      out.insert(head_tag_end + 1, meta);
    } else {
      out.insert(0, meta);
    }
    return out;
  }

  std::size_t png_length(std::string_view png) {
    static const char k_signature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    if (png.size() < 8 || png.substr(0, 8) != std::string_view(k_signature, 8)) {
      return png.size();
    }
    std::size_t pos = 8;
    while (pos + 12 <= png.size()) {
      std::uint32_t length = get_be32(png, pos);
      if (length > png.size() - pos - 12) {
        break;
      }
      std::string_view type = png.substr(pos + 4, 4);
      pos += 12 + length;
      if (type == "IEND") {
        return pos;
      }
    }
    return png.size();
  }

  std::size_t zip_length(std::string_view zip) {
    if (zip.size() < 22) {
      return zip.size();
    }
    std::size_t lowest = zip.size() > 22 + 0xFFFF ? zip.size() - 22 - 0xFFFF : 0;
    for (std::size_t pos = zip.size() - 22 + 1; pos-- > lowest;) {
      if (get_le(zip, pos, 4) == 0x06054b50) {
        std::size_t end = pos + 22 + get_le(zip, pos + 20, 2);
        return end <= zip.size() ? end : zip.size();
      }
    }
    return zip.size();
  }

  std::size_t dib_pixel_offset(std::string_view dib) {
    if (dib.size() < 12) {
      return 0;
    }
    std::uint32_t header_size = get_le(dib, 0, 4);
    std::size_t offset;
    if (header_size == 12) {
      // BITMAPCOREHEADER: 3-byte palette entries
      std::uint32_t bit_count = get_le(dib, 10, 2);
      offset = 12 + (bit_count <= 8 ? (std::size_t(1) << bit_count) * 3 : 0);
    } else if (header_size >= 40 && dib.size() >= 40) {
      std::uint32_t bit_count = get_le(dib, 14, 2);
      std::uint32_t compression = get_le(dib, 16, 4);
      std::uint32_t colors_used = get_le(dib, 32, 4);
      offset = header_size;
      if (header_size == 40 && compression == 3) {  // BI_BITFIELDS masks follow a 40-byte header
        offset += 12;
      } else if (header_size == 40 && compression == 6) {  // BI_ALPHABITFIELDS
        offset += 16;
      }
      if (colors_used > 0) {
        offset += std::size_t(colors_used) * 4;
      } else if (bit_count <= 8) {
        offset += (std::size_t(1) << bit_count) * 4;
      }
    } else {
      return 0;
    }
    return offset <= dib.size() ? offset : 0;
  }

  std::string dib_to_bmp(std::string_view dib) {
    std::size_t pixels = dib_pixel_offset(dib);
    if (pixels == 0) {
      return {};
    }
    std::string bmp = "BM";
    put_u32(bmp, static_cast<std::uint32_t>(14 + dib.size()));
    put_u32(bmp, 0);
    put_u32(bmp, static_cast<std::uint32_t>(14 + pixels));
    bmp.append(dib.data(), dib.size());
    return bmp;
  }

  bool include_image(const std::vector<kind> &source_order, std::string_view plain_text, std::string_view html) {
    for (kind k : source_order) {
      if (k == kind::image) {
        return true;
      }
      if (k == kind::text) {
        // Text first: still a picture copy if the text has nothing of its own, like a browser's
        // "Copy image", which offers <img src="..."> as HTML (and maybe the URL as text) before the picture
        return picture_only_text(plain_text) && (html.empty() || html_visible_text_empty(html));
      }
    }
    return true;  // no text at all
  }

  void prepare_for_send(std::vector<item> &items, bool image_first) {
    // R4: Word's HTML points at its local temp copies of pictures; RTF embeds them
    bool has_rtf = std::any_of(items.begin(), items.end(), [](const item &it) {
      return it.type == "text/rtf";
    });
    if (has_rtf) {
      items.erase(std::remove_if(items.begin(), items.end(), [](const item &it) {
                    return it.type == "text/html" && html_has_local_file_images(it.data);
                  }),
                  items.end());
    }

    // R6: a picture copy's HTML that shows no text only points at the picture (often a URL the other
    // machine can't load), so the picture itself is pasted instead
    bool has_image = std::any_of(items.begin(), items.end(), [](const item &it) {
      return is_image_type(it.type);
    });
    if (image_first && has_image) {
      items.erase(std::remove_if(items.begin(), items.end(), [](const item &it) {
                    return it.type == "text/html" && html_visible_text_empty(it.data);
                  }),
                  items.end());
    }

    std::stable_sort(items.begin(), items.end(), [image_first](const item &a, const item &b) {
      return group_of(a.type, image_first) < group_of(b.type, image_first);
    });
  }

  bool encode_within_limit(bundle &b, std::string &encoded, bool compress) {
    bool had_rich = std::any_of(b.items.begin(), b.items.end(), [](const item &it) {
      return it.type != "text/plain";
    });

    for (;;) {
      std::size_t raw = 0;
      for (const item &it : b.items) {
        raw += it.data.size();
      }
      if (raw <= k_max_raw_bytes) {
        encoded = encode(b, compress);
        if (encoded.size() <= k_max_bundle_bytes) {
          break;
        }
      }

      // Drop the last item that isn't plain text
      auto victim = std::find_if(b.items.rbegin(), b.items.rend(), [](const item &it) {
        return it.type != "text/plain";
      });
      if (victim == b.items.rend()) {
        encoded.clear();
        return false;
      }
      b.items.erase(std::next(victim).base());
    }

    bool has_rich = std::any_of(b.items.begin(), b.items.end(), [](const item &it) {
      return it.type != "text/plain";
    });
    if (had_rich && !has_rich) {
      encoded.clear();
      return false;
    }
    return true;
  }

}  // namespace clipbundle
