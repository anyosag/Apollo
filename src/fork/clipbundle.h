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
    flag_files_skipped = 4,  ///< Files were copied but aren't in the bundle (too many, too large, or unreadable)
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
  /** False for an image whose bytes don't start with its type's signature, so a paste can't be handed another format */
  bool data_matches_type(std::string_view type, std::string_view data);

  std::string to_lf(std::string_view text);  ///< CRLF and lone CR to LF
  std::string to_crlf(std::string_view text);  ///< LF to CRLF
  std::string strip_trailing_nuls(std::string_view data);

  /** Windows "HTML Format": wraps HTML in its header of byte offsets */
  std::string cf_html_build(std::string_view html);
  /** Windows "HTML Format": the HTML document without the header */
  bool cf_html_parse(std::string_view cf_html, std::string &html);

  /** True if the HTML has images pointing at local files (Word's temp copies), which can't cross machines */
  bool html_has_local_file_images(std::string_view html);
  /** True if the HTML shows no text: only tags (like a lone <img>), comments, styles, scripts and whitespace */
  bool html_visible_text_empty(std::string_view html);
  /** True if plain text carries nothing of its own beside a picture: empty, whitespace, or a single URL */
  bool picture_only_text(std::string_view text);
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
   * Rule R3: include an image only for a picture copy. That is: there's no text, the source app offered
   * an image before its text, or the text carries nothing of its own (plain text that's empty or a lone
   * URL, and HTML that's empty or shows no text, like a browser's "Copy image" with its <img> tag first).
   * An Excel range has real text first and a rendered bitmap later, which isn't sent.
   * plain_text is the copy's plain text and html its HTML document (empty when not offered).
   */
  bool include_image(const std::vector<kind> &source_order, std::string_view plain_text, std::string_view html);

  /**
   * Orders items for sending and writing: Office formats, then images and RTF/HTML (images first for a
   * picture copy), then plain text, keeping the source's order within each group. Applies rules R4 and
   * R6 (a picture copy with an image drops HTML that shows no text: it only points at the picture's URL).
   */
  void prepare_for_send(std::vector<item> &items, bool image_first);

  /**
   * Rule R5: encodes the bundle, dropping items from the end (never plain text) until it fits
   * k_max_bundle_bytes. Returns false if it can't fit, or if only text would remain of a richer copy.
   */
  bool encode_within_limit(bundle &b, std::string &encoded, bool compress = true);

  // ---- File copies ---------------------------------------------------------------------------
  //
  // Files copied in Explorer or Finder travel as a list in the bundle (an item of k_files_type), and
  // their contents in chunks of up to k_file_chunk_bytes, a request each (/actions/clipboard with
  // type=file). The other side's clipboard changes only once every file has arrived. Peers without
  // this ignore the item, as an unknown type.

  constexpr const char *k_files_type = "files/manifest";
  constexpr std::size_t k_max_file_entries = 10000;
  constexpr std::uint64_t k_max_file_bytes = std::uint64_t(20) << 30;  ///< All the files of one copy
  constexpr std::uint64_t k_max_auto_file_bytes = std::uint64_t(1) << 30;  ///< What a sync on its own (not a shortcut) moves
  constexpr std::size_t k_file_chunk_bytes = std::size_t(8) << 20;
  constexpr std::size_t k_max_path_bytes = 1024;  ///< A relative path, in UTF-8 bytes

  struct file_entry {
    std::string path;  ///< Relative to the copy, parts separated by '/'; a top-level entry has none
    std::uint64_t size = 0;  ///< 0 for a folder
    std::int64_t mtime = 0;  ///< Last modified, in seconds since 1970 UTC; 0 if unknown
    bool directory = false;
  };

  struct file_manifest {
    std::string transfer;  ///< transfer_id() of the entries: names the copy on the machine that has the files
    std::vector<file_entry> entries;  ///< A folder comes before what it holds
  };

  /** 32 hex digits made from the entries: the same files give the same id, so a copy read again matches */
  std::string transfer_id(const std::vector<file_entry> &entries);
  bool valid_transfer_id(std::string_view id);

  /** The manifest as an item's data, and back. decode_manifest rejects any path that isn't well_formed_path(). */
  std::string encode_manifest(const file_manifest &m);
  bool decode_manifest(std::string_view data, file_manifest &out, std::string *error = nullptr);

  /** Bytes of all the files */
  std::uint64_t manifest_bytes(const file_manifest &m);

  /** A plain relative path: not empty, no root or drive, no '\', no empty, '.' or '..' part, no control characters */
  bool well_formed_path(std::string_view path);

  /**
   * The names to write the entries under here, one per entry. In each part of a path, what Windows
   * can't have in a name (<>:"\|?* and control characters) becomes '_', trailing dots and spaces go,
   * and Windows' reserved names (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with any extension) get a '_' in
   * front. An entry whose name is taken already (ignoring ASCII case, as macOS and Windows do), or
   * that's inside one left out, is left out: its name is empty.
   */
  std::vector<std::string> local_paths(const file_manifest &m);

  /** The indexes of the entries at the top of the copy, which are what the clipboard lists */
  std::vector<std::size_t> top_level(const file_manifest &m);

}  // namespace clipbundle
