// Tests for clipbundle (the clipboard bundle shared by anyosag/Apollo and anyosag/LuzLunar).
// Identical in both repos. A standalone program, so CI builds it without either app:
//   g++ -std=c++17 -DCLIPBUNDLE_WITH_ZLIB -I<dir with clipbundle.h> clipbundle_test.cpp clipbundle.cpp -lz

#include "clipbundle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

using namespace clipbundle;

static int s_Failures = 0;
static std::size_t s_LargestAllocation = 0;

// Records the largest allocation, so tests can check a bad bundle is refused before allocating for it
void *operator new(std::size_t size) {
  if (size > s_LargestAllocation) {
    s_LargestAllocation = size;
  }
  if (void *p = std::malloc(size > 0 ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *p) noexcept {
  std::free(p);
}

void operator delete(void *p, std::size_t) noexcept {
  std::free(p);
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
  std::free(p);
}

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static std::string random_bytes(std::size_t size, unsigned seed) {
  std::mt19937 rng(seed);
  std::string out(size, '\0');
  for (char &c : out) {
    c = static_cast<char>(rng() & 0xFF);
  }
  return out;
}

static std::string repeat(const std::string &s, int times) {
  std::string out;
  for (int i = 0; i < times; i++) {
    out += s;
  }
  return out;
}

static bool same_items(const std::vector<item> &a, const std::vector<item> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); i++) {
    if (a[i].type != b[i].type || a[i].data != b[i].data) {
      return false;
    }
  }
  return true;
}

static void test_round_trip() {
  bundle b;
  b.flags = flag_concealed;
  b.etag = 0x0123456789abcdefull;
  b.token = "a1b2c3d4-42";
  b.items = {
    {"text/plain", "Hello \xF0\x9F\x8C\x99 \xE6\x9C\x88\n"},  // emoji and CJK
    {"text/rtf", repeat("{\\rtf1 \\b bold\\b0 }", 400)},  // compressible
    {"image/png", random_bytes(5000, 1)},  // never compressed
    {"ms/gvml", std::string("PK\0\0with\0nuls", 13)},
    {"text/html", ""},  // empty item
  };

  for (bool compress : {false, true}) {
    std::string wire = encode(b, compress);
    bundle out;
    std::string error;
    CHECK(decode(wire, out, &error));
    CHECK(error.empty());
    CHECK(out.flags == b.flags);
    CHECK(out.etag == b.etag);
    CHECK(out.token == b.token);
    CHECK(same_items(out.items, b.items));
  }

#ifdef CLIPBUNDLE_WITH_ZLIB
  // Compression shrinks text, and leaves random bytes alone
  CHECK(encode(b, true).size() < encode(b, false).size() - 4000);
  bundle random_text;
  random_text.items = {{"text/plain", random_bytes(4096, 2)}};
  CHECK(encode(random_text, true).size() == encode(random_text, false).size());
#endif

  // An empty bundle (an "unchanged" reply)
  bundle empty;
  empty.flags = flag_unchanged;
  empty.token = "ff-1";
  bundle out;
  CHECK(decode(encode(empty), out));
  CHECK(out.flags == flag_unchanged && out.items.empty() && out.token == "ff-1");
}

static void test_corrupt_input() {
  bundle b;
  b.token = "abc-1";
  b.items = {{"text/plain", "hello"}, {"text/rtf", repeat("{\\rtf1 x}", 300)}};
  std::string wire = encode(b);
  bundle out;

  // Every truncation fails
  for (std::size_t i = 0; i < wire.size(); i++) {
    CHECK(!decode(std::string_view(wire).substr(0, i), out));
  }
  // Trailing bytes fail
  CHECK(!decode(wire + "x", out));

  std::string bad = wire;
  bad[0] = 'X';
  CHECK(!decode(bad, out));  // magic

  bad = wire;
  bad[4] = 2;
  CHECK(!decode(bad, out));  // version

  // Too many items
  std::string many(wire.data(), 4 + 2 + 2 + 8);
  many.push_back(0);  // no token
  many.push_back(static_cast<char>(k_max_items + 1));
  CHECK(!decode(many, out));

  // A raw item whose stored size doesn't match its raw size
  bundle one;
  one.items = {{"text/plain", "abc"}};
  std::string mismatch = encode(one, false);
  mismatch[mismatch.size() - 3 - 4 - 4] = 4;  // raw size 3 -> 4
  CHECK(!decode(mismatch, out));

  // A bad token or type
  bundle bad_token;
  bad_token.token = "UPPER";
  CHECK(!decode(encode(bad_token), out));
  bundle bad_type;
  bad_type.items = {{"has space", "x"}};
  CHECK(!decode(encode(bad_type), out));

  // Unknown encoding
  std::string unknown = encode(one, false);
  unknown[unknown.size() - 3 - 4 - 4 - 1] = 7;
  CHECK(!decode(unknown, out));

#ifdef CLIPBUNDLE_WITH_ZLIB
  // A compressed item claiming more than its bytes can inflate to fails before allocating it
  bundle zeros;
  zeros.items = {{"text/plain", std::string(std::size_t(8) << 20, '\0')}};
  std::string wire_zeros = encode(zeros);
  CHECK(decode(wire_zeros, out) && same_items(out.items, zeros.items));  // the best ratio deflate gets
  const std::size_t raw_at = 4 + 2 + 2 + 8 + 1 + 1 + 1 + 10 + 1;  // header, no token, one "text/plain" item
  const std::size_t stored_size = wire_zeros.size() - raw_at - 8;
  CHECK(wire_zeros[raw_at - 1] == 1);  // zlib
  for (std::uint32_t claimed : {static_cast<std::uint32_t>(stored_size * 1032 + 1), static_cast<std::uint32_t>(k_max_raw_bytes)}) {
    for (int i = 0; i < 4; i++) {
      wire_zeros[raw_at + i] = static_cast<char>((claimed >> (8 * i)) & 0xFF);
    }
    s_LargestAllocation = 0;
    CHECK(!decode(wire_zeros, out));
    CHECK(s_LargestAllocation < claimed);
  }
#endif
}

static void test_etag() {
  std::vector<item> a = {{"text/plain", "x"}, {"text/html", "<b>x</b>"}};
  std::vector<item> b = {{"text/html", "<b>x</b>"}, {"text/plain", "x"}};
  std::vector<item> c = {{"text/plain", "y"}, {"text/html", "<b>x</b>"}};
  CHECK(etag(a) == etag(b));  // order doesn't matter
  CHECK(etag(a) != etag(c));
  CHECK(etag({}) == 0);
  CHECK(etag(a) != 0);
  // The type is part of the hash
  CHECK(etag({{"text/plain", "x"}}) != etag({{"text/rtf", "x"}}));

  std::uint64_t parsed = 0;
  CHECK(parse_etag_hex(etag_hex(etag(a)), parsed) && parsed == etag(a));
  CHECK(etag_hex(0x1f).size() == 16);
  CHECK(!parse_etag_hex("xyz", parsed));
  CHECK(!parse_etag_hex("", parsed));
}

static void test_tables() {
  CHECK(find_type("text/plain") != nullptr);
  CHECK(find_windows("Rich Text Format") != nullptr && std::strcmp(find_windows("Rich Text Format")->type, "text/rtf") == 0);
  CHECK(find_windows("HTML Format") != nullptr);
  CHECK(find_mac("com.microsoft.Art--GVML-ClipFormat") != nullptr && std::strcmp(find_mac("com.microsoft.Art--GVML-ClipFormat")->windows_name, "Art::GVML ClipFormat") == 0);
  CHECK(find_mac("public.png") != nullptr);
  CHECK(find_windows("Embed Source") == nullptr);
  CHECK(is_image_type("image/png") && !is_image_type("text/plain"));
  CHECK(is_office_type("ms/gvml") && !is_office_type("text/rtf"));
  CHECK(is_text_type("text/html") && !is_text_type("image/png"));
}

static void test_signatures() {
  CHECK(data_matches_type("image/png", std::string("\x89PNG\r\n\x1a\n...", 11)));
  CHECK(data_matches_type("image/jpeg", "\xFF\xD8\xFF\xE0..."));
  CHECK(!data_matches_type("image/png", "\xFF\xD8\xFF\xE0..."));  // a JPEG labeled PNG
  CHECK(!data_matches_type("image/jpeg", "BM...."));
  CHECK(!data_matches_type("image/png", ""));
  CHECK(!data_matches_type("image/gif", "GIF89a"));  // not a type the bundle carries
  CHECK(data_matches_type("text/plain", "anything"));
  CHECK(data_matches_type("ms/gvml", ""));
}

static void test_line_endings() {
  CHECK(to_lf("a\r\nb\rc\nd") == "a\nb\nc\nd");
  CHECK(to_crlf("a\nb\r\nc\rd") == "a\r\nb\r\nc\r\nd");
  CHECK(strip_trailing_nuls(std::string("abc\0\0", 5)) == "abc");
  CHECK(strip_trailing_nuls(std::string("\0", 1)).empty());
}

static void test_cf_html() {
  // A bare fragment is wrapped
  std::string built = cf_html_build("<b>caf\xC3\xA9</b>");
  CHECK(built.compare(0, 12, "Version:0.9\r") == 0);
  std::string html;
  CHECK(cf_html_parse(built, html));
  CHECK(html == "<html><body><!--StartFragment--><b>caf\xC3\xA9</b><!--EndFragment--></body></html>");

  // Offsets are byte offsets that point at the fragment
  auto field = [&built](const char *name) {
    std::size_t pos = built.find(name);
    return std::stol(built.substr(pos + std::strlen(name), 10));
  };
  long start_fragment = field("StartFragment:");
  long end_fragment = field("EndFragment:");
  CHECK(built.substr(start_fragment, end_fragment - start_fragment) == "<b>caf\xC3\xA9</b>");
  CHECK(field("EndHTML:") == long(built.size()));
  CHECK(field("StartHTML:") == 105);

  // A full document gets markers inside <body>, keeping <head> (Word's styles)
  std::string doc = "<html><head><style>p{color:red}</style></head><BODY class=x><p>Hi</p></BODY></html>";
  built = cf_html_build(doc);
  CHECK(cf_html_parse(built, html));
  CHECK(html == "<html><head><style>p{color:red}</style></head><BODY class=x><!--StartFragment--><p>Hi</p><!--EndFragment--></BODY></html>");

  // Existing markers are kept as they are
  std::string marked = "<html><body><!--StartFragment-->x<!--EndFragment--></body></html>";
  CHECK(cf_html_parse(cf_html_build(marked), html) && html == marked);

  // Word style: Version 1.0, SourceURL, 8-digit offsets, trailing NUL
  std::string body = "<html><body><!--StartFragment-->w<!--EndFragment--></body></html>";
  std::string header = "Version:1.0\r\nStartHTML:00000000\r\nEndHTML:00000000\r\nStartFragment:00000000\r\nEndFragment:00000000\r\nSourceURL:file:///C:/doc.docx\r\n";
  char numbers[64];
  std::snprintf(numbers, sizeof(numbers), "%08llu", static_cast<unsigned long long>(header.size()));
  header.replace(header.find("StartHTML:") + 10, 8, numbers);
  std::snprintf(numbers, sizeof(numbers), "%08llu", static_cast<unsigned long long>(header.size() + body.size()));
  header.replace(header.find("EndHTML:") + 8, 8, numbers);
  CHECK(cf_html_parse(header + body + std::string("\0", 1), html) && html == body);

  // StartHTML:-1 (fragment only) falls back to everything after the header
  std::string no_html = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:0000000100\r\nEndFragment:0000000110\r\n<p>frag</p>";
  CHECK(cf_html_parse(no_html, html) && html == "<p>frag</p>");

  // Out-of-range offsets fall back too
  std::string broken = "Version:0.9\r\nStartHTML:0000099999\r\nEndHTML:0000999999\r\n<p>x</p>";
  CHECK(cf_html_parse(broken, html) && html == "<p>x</p>");
}

static void test_html_helpers() {
  CHECK(html_has_local_file_images("<img width=10 src=\"file:///C:/Users/x/AppData/clip_image001.png\">"));
  CHECK(html_has_local_file_images("<v:imagedata src='FILE:///tmp/a.png'/>"));
  CHECK(html_has_local_file_images("<IMG SRC = file:///x.png>"));
  CHECK(!html_has_local_file_images("<img src=\"https://example.com/a.png\">"));
  CHECK(!html_has_local_file_images("<img src=\"data:image/png;base64,AAA\"> source file: none"));

  CHECK(ensure_utf8_meta("<html><head><meta charset=\"windows-1252\"></head><body>x</body></html>").find("utf-8") == std::string::npos);
  CHECK(ensure_utf8_meta("<html><head></head><body>x</body></html>") == "<html><head><meta charset=\"utf-8\"></head><body>x</body></html>");
  CHECK(ensure_utf8_meta("<b>x</b>") == "<meta charset=\"utf-8\"><b>x</b>");
}

static void test_trimming() {
  // PNG: signature, IHDR (13 bytes), IEND, then junk
  std::string png("\x89PNG\r\n\x1a\n", 8);
  png += std::string("\0\0\0\x0dIHDR", 8) + std::string(13, 'h') + "crc!";
  png += std::string("\0\0\0\0IEND", 8) + "crc!";
  std::size_t real = png.size();
  CHECK(png_length(png + std::string(100, '\0')) == real);
  CHECK(png_length("not a png") == 9);

  // Zip: data, end of central directory with a 3-byte comment, then padding
  std::string zip = "PK\x03\x04 some data";
  zip += std::string("PK\x05\x06", 4) + std::string(16, '\0') + std::string("\x03\0", 2) + "abc";
  std::size_t zip_real = zip.size();
  CHECK(zip_length(zip + std::string(50, '\0')) == zip_real);
  CHECK(zip_length("short") == 5);
}

static void test_dib() {
  // 40-byte BITMAPINFOHEADER, 32 bpp BI_BITFIELDS: header + 12 bytes of masks
  std::string dib(40 + 12 + 16, '\0');
  dib[0] = 40;
  dib[14] = 32;
  dib[16] = 3;
  CHECK(dib_pixel_offset(dib) == 52);

  // 8 bpp with no colors-used count: a 256-entry palette
  std::string dib8(40 + 1024 + 4, '\0');
  dib8[0] = 40;
  dib8[14] = 8;
  CHECK(dib_pixel_offset(dib8) == 40 + 1024);

  // BITMAPV5HEADER (124 bytes), 32 bpp: masks are inside the header
  std::string dib5(124 + 16, '\0');
  dib5[0] = 124;
  dib5[14] = 32;
  dib5[16] = 3;
  CHECK(dib_pixel_offset(dib5) == 124);

  std::string bmp = dib_to_bmp(dib);
  CHECK(bmp.size() == dib.size() + 14);
  CHECK(bmp.compare(0, 2, "BM") == 0);
  CHECK(static_cast<unsigned char>(bmp[10]) == 14 + 52);

  CHECK(dib_pixel_offset("tiny") == 0);
  CHECK(dib_to_bmp("tiny").empty());
}

static void test_picture_text() {
  // HTML that shows no text
  CHECK(html_visible_text_empty(""));
  CHECK(html_visible_text_empty("<html><body><!--StartFragment--><img src=\"https://example.com/cat.png\" alt=\"A cat\"/><!--EndFragment--></body></html>"));
  CHECK(html_visible_text_empty("<html><head><title>Cat</title><style>p{color:red}</style><meta charset=\"utf-8\"></head><body>\r\n &nbsp;<img src=x>&#160;</body></html>"));
  CHECK(html_visible_text_empty("<script>var a = 'text';</script><IMG SRC=x>\xC2\xA0"));
  CHECK(html_visible_text_empty("<img src=x"));  // cut off inside a tag
  // HTML with text
  CHECK(!html_visible_text_empty("<html><body><!--StartFragment--><table><tr><td>1</td></tr></table><!--EndFragment--></body></html>"));
  CHECK(!html_visible_text_empty("<img src=x> caption"));
  CHECK(!html_visible_text_empty("<header>Shown</header>"));  // not <head>
  CHECK(!html_visible_text_empty("&amp;"));
  CHECK(!html_visible_text_empty("<p>caf\xC3\xA9</p>"));

  // Plain text that's only a picture's address, or nothing
  CHECK(picture_only_text(""));
  CHECK(picture_only_text(" \n\t"));
  CHECK(picture_only_text("https://cdn.example.com/a.png?size=large"));
  CHECK(picture_only_text("  HTTP://EXAMPLE.COM/A.PNG\n"));
  CHECK(picture_only_text("data:image/png;base64,iVBORw0KGgo="));
  CHECK(!picture_only_text("hello"));
  CHECK(!picture_only_text("see https://example.com/a.png"));
  CHECK(!picture_only_text("1\t2\n3\t4\n"));
}

static void test_rules() {
  const std::string img_html = "<html><body><!--StartFragment--><img src=\"https://example.com/a.png\"/><!--EndFragment--></body></html>";
  const std::string table_html = "<html><body><!--StartFragment--><table><tr><td>1</td><td>2</td></tr></table><!--EndFragment--></body></html>";

  // R3 with the format orders apps really use
  CHECK(!include_image({kind::other, kind::text, kind::text, kind::image}, "1\t2\n", table_html));  // Excel range: text before its bitmap
  CHECK(!include_image({kind::text, kind::text, kind::image}, "Some words", "<p>Some words</p>"));  // Word text with a rendered bitmap
  CHECK(!include_image({kind::text, kind::image}, "Some words", ""));  // text and a bitmap, no HTML
  CHECK(include_image({kind::other, kind::image, kind::text}, "caption", "<p>caption</p>"));  // Word picture: the picture comes first
  CHECK(include_image({kind::image}, "", ""));  // Snipping Tool / screenshot
  CHECK(include_image({kind::other, kind::text, kind::image}, "", img_html));  // Chromium "Copy image": HTML, then the bitmap
  CHECK(include_image({kind::text, kind::text, kind::image}, "https://example.com/a.png", img_html));  // Firefox: HTML and the URL as text
  CHECK(include_image({kind::text, kind::image}, "", ""));  // an empty text format before the picture
  CHECK(include_image({}, "", ""));

  // Ordering: Office first, then RTF/HTML (source order), images, then plain text
  std::vector<item> items = {{"text/plain", "t"}, {"image/png", "p"}, {"text/html", "<b>h</b>"}, {"text/rtf", "{\\rtf1}"}, {"ms/gvml", "g"}};
  prepare_for_send(items, false);
  CHECK(items.size() == 5);
  CHECK(items[0].type == "ms/gvml");
  CHECK(items[1].type == "text/html" && items[2].type == "text/rtf");
  CHECK(items[3].type == "image/png");
  CHECK(items[4].type == "text/plain");

  // A picture copy puts the image first
  items = {{"text/plain", "t"}, {"text/html", "<b>h</b>"}, {"image/png", "p"}};
  prepare_for_send(items, true);
  CHECK(items.size() == 3 && items[0].type == "image/png" && items[2].type == "text/plain");

  // R6: ...and drops HTML that only points at the picture
  items = {{"text/plain", "https://example.com/a.png"}, {"text/html", img_html}, {"image/png", "p"}};
  prepare_for_send(items, true);
  CHECK(items.size() == 2 && items[0].type == "image/png" && items[1].type == "text/plain");
  // ...but keeps it when the image didn't make it
  items = {{"text/html", img_html}};
  prepare_for_send(items, true);
  CHECK(items.size() == 1);
  // ...and when it isn't a picture copy
  items = {{"text/html", img_html}, {"image/png", "p"}};
  prepare_for_send(items, false);
  CHECK(items.size() == 2);

  // R4: Word's HTML with local images is dropped when RTF is there...
  items = {{"text/html", "<img src=\"file:///C:/clip_image001.png\">"}, {"text/rtf", "{\\rtf1\\pict}"}, {"text/plain", "t"}};
  prepare_for_send(items, false);
  CHECK(items.size() == 2 && items[0].type == "text/rtf");
  // ...but kept without RTF
  items = {{"text/html", "<img src=\"file:///C:/a.png\">"}, {"text/plain", "t"}};
  prepare_for_send(items, false);
  CHECK(items.size() == 2);
}

static void test_limit() {
  bundle b;
  b.items = {{"text/plain", "t"}, {"text/rtf", "{\\rtf1}"}};
  std::string encoded;
  CHECK(encode_within_limit(b, encoded));
  CHECK(!encoded.empty() && b.items.size() == 2);

  // An oversized image is dropped, keeping RTF and text
  b.items = {{"text/rtf", "{\\rtf1}"}, {"image/png", random_bytes(k_max_bundle_bytes + 10, 3)}, {"text/plain", "t"}};
  CHECK(encode_within_limit(b, encoded));
  CHECK(b.items.size() == 2 && b.items[0].type == "text/rtf" && b.items[1].type == "text/plain");
  CHECK(encoded.size() <= k_max_bundle_bytes);

  // If only text would be left of a richer copy, it doesn't fit
  b.items = {{"image/png", random_bytes(k_max_bundle_bytes + 10, 4)}, {"text/plain", "t"}};
  CHECK(!encode_within_limit(b, encoded));
  CHECK(encoded.empty());
}

static file_entry entry(const std::string &path, std::uint64_t size, bool directory = false, std::int64_t mtime = 1700000000) {
  file_entry e;
  e.path = path;
  e.size = size;
  e.directory = directory;
  e.mtime = mtime;
  return e;
}

static void test_manifest_round_trip() {
  file_manifest m;
  m.entries = {entry("Photos", 0, true), entry("Photos/IMG 1.jpg", 4000000), entry("Photos/raw/IMG 1.dng", 25000000),
               entry("notes.txt", 12, false, 0), entry("Empty", 0, true), entry("Ünïcødé ✓.pdf", 1)};
  m.transfer = transfer_id(m.entries);
  CHECK(valid_transfer_id(m.transfer));

  file_manifest back;
  std::string error;
  CHECK(decode_manifest(encode_manifest(m), back, &error));
  CHECK(error.empty());
  CHECK(back.transfer == m.transfer);
  CHECK(back.entries.size() == m.entries.size());
  for (std::size_t i = 0; i < m.entries.size() && i < back.entries.size(); i++) {
    CHECK(back.entries[i].path == m.entries[i].path);
    CHECK(back.entries[i].size == m.entries[i].size);
    CHECK(back.entries[i].mtime == m.entries[i].mtime);
    CHECK(back.entries[i].directory == m.entries[i].directory);
  }
  CHECK(manifest_bytes(back) == 29000013);

  std::vector<std::size_t> top = top_level(back);
  CHECK(top.size() == 4);
  CHECK(top.size() == 4 && top[0] == 0 && top[1] == 3 && top[2] == 4 && top[3] == 5);

  // In a bundle next to the text, and part of its hash like any item
  bundle b;
  b.items = {{"text/plain", "Photos\nnotes.txt"}, {k_files_type, encode_manifest(m)}};
  bundle decoded;
  CHECK(decode(encode(b), decoded) && same_items(decoded.items, b.items));
  CHECK(etag(b.items) != etag({b.items[0]}));
}

static void test_transfer_id() {
  std::vector<file_entry> a = {entry("a.txt", 1), entry("b.txt", 2)};
  std::vector<file_entry> same = {entry("a.txt", 1), entry("b.txt", 2)};
  std::vector<file_entry> resized = {entry("a.txt", 1), entry("b.txt", 3)};
  std::vector<file_entry> touched = {entry("a.txt", 1), entry("b.txt", 2, false, 1700000001)};
  CHECK(transfer_id(a) == transfer_id(same));  // a copy read again matches
  CHECK(transfer_id(a) != transfer_id(resized));
  CHECK(transfer_id(a) != transfer_id(touched));
  CHECK(transfer_id(a).size() == 32);

  CHECK(!valid_transfer_id(""));
  CHECK(!valid_transfer_id(std::string(31, 'a')));
  CHECK(!valid_transfer_id(std::string(32, 'A')));
  CHECK(!valid_transfer_id(std::string(31, 'a') + "/"));
  CHECK(valid_transfer_id(std::string(32, '0')));
}

static void test_bad_manifests() {
  file_manifest m;
  m.entries = {entry("a.txt", 5)};
  m.transfer = transfer_id(m.entries);
  std::string good = encode_manifest(m);
  file_manifest out;
  CHECK(decode_manifest(good, out));

  // Truncated anywhere, or with a byte too many
  for (std::size_t n = 0; n < good.size(); n++) {
    CHECK(!decode_manifest(good.substr(0, n), out));
  }
  CHECK(!decode_manifest(good + "x", out));

  // Paths that could leave the folder they're written into
  for (const char *path : {"../evil", "a/../../evil", "/etc/passwd", "C:\\Windows\\win.ini", "C:evil", "a\\..\\b", "a//b", "./a", "a/.",
                           "", "a/b/", "tab\there"}) {
    file_manifest bad;
    bad.entries = {entry(path, 1)};
    bad.transfer = transfer_id(bad.entries);
    std::string error;
    CHECK(!decode_manifest(encode_manifest(bad), out, &error));
    CHECK(!error.empty());
  }

  // Too many files, too many bytes, a bad id
  file_manifest many;
  for (std::size_t i = 0; i <= k_max_file_entries; i++) {
    many.entries.push_back(entry("f" + std::to_string(i), 1));
  }
  many.transfer = transfer_id(many.entries);
  CHECK(!decode_manifest(encode_manifest(many), out));

  file_manifest huge;
  huge.entries = {entry("a", k_max_file_bytes), entry("b", 1)};
  huge.transfer = transfer_id(huge.entries);
  CHECK(!decode_manifest(encode_manifest(huge), out));
  huge.entries.pop_back();
  huge.transfer = transfer_id(huge.entries);
  CHECK(decode_manifest(encode_manifest(huge), out));

  file_manifest unnamed = m;
  unnamed.transfer = "not-an-id";
  CHECK(!decode_manifest(encode_manifest(unnamed), out));

  file_manifest empty;
  empty.transfer = transfer_id(empty.entries);
  CHECK(!decode_manifest(encode_manifest(empty), out));

  // A folder's size is ignored
  file_manifest folder;
  folder.entries = {entry("dir", 99, true)};
  folder.transfer = transfer_id(folder.entries);
  CHECK(decode_manifest(encode_manifest(folder), out) && out.entries[0].size == 0);
}

static void test_paths() {
  CHECK(well_formed_path("a"));
  CHECK(well_formed_path("a b/c.d/e"));
  CHECK(well_formed_path("..hidden"));  // a name, not a parent
  CHECK(well_formed_path("Ünïcødé/✓"));
  CHECK(!well_formed_path(".."));
  CHECK(!well_formed_path("a/../b"));
  CHECK(!well_formed_path("D:"));
  CHECK(!well_formed_path("a:b"));  // drive A, relative
  CHECK(!well_formed_path(std::string(k_max_path_bytes + 1, 'a')));

  // Names Windows can't have, and its devices
  file_manifest m;
  m.entries = {entry("What? <draft>: v2*.txt", 1), entry("CON", 1), entry("con.txt", 1), entry("COM1.log", 1), entry("COMPASS", 1),
               entry("trailing. . ", 1), entry("...", 1), entry("pipe|d", 1)};
  std::vector<std::string> local = local_paths(m);
  CHECK(local.size() == m.entries.size());
  if (local.size() == m.entries.size()) {
    CHECK(local[0] == "What_ _draft__ v2_.txt");
    CHECK(local[1] == "_CON");
    CHECK(local[2] == "_con.txt");
    CHECK(local[3] == "_COM1.log");
    CHECK(local[4] == "COMPASS");
    CHECK(local[5] == "trailing");
    CHECK(local[6] == "_");
    CHECK(local[7] == "pipe_d");
  }

  // A name taken already, ignoring case, is left out, with everything inside it
  file_manifest clash;
  clash.entries = {entry("Report.pdf", 1), entry("report.PDF", 2), entry("ab:c", 1), entry("ab_c", 1), entry("Dir", 0, true),
                   entry("Dir/x", 1), entry("dir", 0, true), entry("dir/y", 1), entry("dir/sub", 0, true), entry("dir/sub/z", 1), entry("other", 1)};
  local = local_paths(clash);
  CHECK(local.size() == clash.entries.size());
  if (local.size() == clash.entries.size()) {
    CHECK(local[0] == "Report.pdf");
    CHECK(local[1].empty());
    CHECK(local[2] == "ab_c");
    CHECK(local[3].empty());
    CHECK(local[4] == "Dir");
    CHECK(local[5] == "Dir/x");
    CHECK(local[6].empty());
    CHECK(local[7].empty());
    CHECK(local[8].empty());
    CHECK(local[9].empty());
    CHECK(local[10] == "other");
  }

  // A file in a folder the list doesn't name gets a sanitized folder path too
  file_manifest loose;
  loose.entries = {entry("x?/y.txt", 1)};
  local = local_paths(loose);
  CHECK(local.size() == 1 && local[0] == "x_/y.txt");
}

int main() {
  test_manifest_round_trip();
  test_transfer_id();
  test_bad_manifests();
  test_paths();
  test_round_trip();
  test_corrupt_input();
  test_etag();
  test_tables();
  test_signatures();
  test_line_endings();
  test_cf_html();
  test_html_helpers();
  test_trimming();
  test_dib();
  test_picture_text();
  test_rules();
  test_limit();

  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All clipbundle tests passed\n");
  return 0;
}
