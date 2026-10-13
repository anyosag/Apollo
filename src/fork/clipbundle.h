/**
 * @file clipbundle.h
 * @brief The clipboard bundle exchanged between LuzLunar and this Apollo build.
 *
 * Shared, byte for byte, by anyosag/Apollo (src/fork/clipbundle.*) and anyosag/LuzLunar
 * (app/fork/clipbundle.*). Keep both copies identical; tests/fork/clipbundle_test.cpp checks the
 * same behavior in both repos' CI. Plain C++17 with no dependencies other than zlib, which is
 * used when CLIPBUNDLE_WITH_ZLIB is defined.
 *
 * Wire format (little-endian):
 *   magic "LLCB", u16 version, u16 flags, u64 etag, u8 token length, token,
 *   u8 item count, then per item: u8 type length, type, u8 encoding (0 raw, 1 zlib),
 *   u32 raw size, u32 stored size, stored bytes.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clipbundle {

  constexpr std::uint16_t k_version = 1;
  constexpr std::size_t k_max_bundle_bytes = std::size_t(64) << 20;  ///< Encoded size limit
  constexpr std::size_t k_max_raw_bytes = std::size_t(128) << 20;  ///< Limit on all items before compression
  constexpr std::size_t k_max_items = 32;
  constexpr std::size_t k_max_token = 64;
  constexpr std::size_t k_max_type = 64;

  enum flag : std::uint16_t {
    flag_unchanged = 1,  ///< The receiver already has this content; no items
    flag_concealed = 2,  ///< A password or other private content
  };

  struct item {
    std::string type;  ///< Bundle type, like "text/plain" (see types())
    std::string data;  ///< Raw bytes
  };

  struct bundle {
    std::uint16_t flags = 0;
    std::uint64_t etag = 0;  ///< Hash of the content the sender holds; 0 when unknown
    std::string token;  ///< The host's clipboard state, echoed back as "since"
    std::vector<item> items;
  };

  /** Encodes a bundle. Text-like items are compressed when built with zlib and it saves space. */
  std::string encode(const bundle &b, bool compress = true);

  /** Decodes a bundle. Returns false for anything malformed, with a reason in error. */
  bool decode(std::string_view bytes, bundle &out, std::string *error = nullptr);

  /** Content hash (64-bit FNV-1a) of items, independent of their order and compression. 0 for no items. */
  std::uint64_t etag(const std::vector<item> &items);
  std::string etag_hex(std::uint64_t etag);
  bool parse_etag_hex(std::string_view hex, std::uint64_t &etag);

  /** A clipboard format both platforms understand */
  struct type_info {
    const char *type;  ///< Bundle type
    const char *windows_name;  ///< Registered Windows clipboard format, or nullptr (text uses CF_UNICODETEXT)
    const char *mac_type;  ///< macOS pasteboard type
    bool compressible;
  };

  const std::vector<type_info> &types();
  const type_info *find_type(std::string_view type);
  const type_info *find_windows(std::string_view windows_name);
  const type_info *find_mac(std::string_view mac_type);

  bool is_text_type(std::string_view type);  ///< text/plain, text/rtf, text/html
  bool is_image_type(std::string_view type);  ///< image/*
  bool is_office_type(std::string_view type);  ///< ms/*

  std::string to_lf(std::string_view text);  ///< CRLF and lone CR to LF
  std::string to_crlf(std::string_view text);  ///< LF to CRLF
  std::string strip_trailing_nuls(std::string_view data);

  /** Windows "HTML Format": wraps HTML in its header of byte offsets */
  std::string cf_html_build(std::string_view html);
  /** Windows "HTML Format": the HTML document without the header */
  bool cf_html_parse(std::string_view cf_html, std::string &html);

  /** True if the HTML has images pointing at local files (Word's temp copies), which can't cross machines */
  bool html_has_local_file_images(std::string_view html);
  /** Adds <meta charset="utf-8"> unless the document declares a charset */
  std::string ensure_utf8_meta(std::string_view html);

  std::size_t png_length(std::string_view png);  ///< Bytes up to and including IEND, or the full size
  std::size_t zip_length(std::string_view zip);  ///< Bytes up to the end of the central directory, or the full size
  std::size_t dib_pixel_offset(std::string_view dib);  ///< Header, masks and palette size of a DIB; 0 if invalid
  std::string dib_to_bmp(std::string_view dib);  ///< Adds a BITMAPFILEHEADER; empty if invalid

  /** What a format in the source clipboard is, for the image rule */
  enum class kind {
    text,
    image,
    other
  };

  /**
   * Rule R3: include an image only if there's no text, or the source app offered an image before its
   * text (a picture copy). An Excel range has text first and a rendered bitmap later, which isn't sent.
   */
  bool include_image(const std::vector<kind> &source_order);

  /**
   * Orders items for sending and writing: Office formats, then images and RTF/HTML (images first for a
   * picture copy), then plain text, keeping the source's order within each group. Applies rule R4.
   */
  void prepare_for_send(std::vector<item> &items, bool image_first);

  /**
   * Rule R5: encodes the bundle, dropping items from the end (never plain text) until it fits
   * k_max_bundle_bytes. Returns false if it can't fit, or if only text would remain of a richer copy.
   */
  bool encode_within_limit(bundle &b, std::string &encoded, bool compress = true);

}  // namespace clipbundle
