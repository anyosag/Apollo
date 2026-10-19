/**
 * @file src/fork/clipboard_bundle.h
 * @brief Fork-only: `/actions/clipboard?type=bundle`, rich clipboard sync for LuzLunar.
 *
 * Text, RTF, HTML, images and Office formats travel in one clipbundle (see clipbundle.h), and so does
 * the list of copied files, whose contents come in chunks (type=file, clipboard_files.h). Clipboard
 * work runs on its own thread, because reading can wait on the app that copied (Office renders some
 * formats on demand) and nvhttp's HTTPS server handles requests on a single thread.
 */
#pragma once

// standard includes
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace fork_tweaks::clipboard {
  /** Sends the response; may be called from the clipboard thread */
  using reply_fn = std::function<void(int status, std::string body)>;

  /** Request bodies are bundles of up to 64 MiB */
  inline constexpr std::size_t max_request_bytes = (std::size_t(64) << 20) + (std::size_t(1) << 20);

  /** True for the clipboard types this build adds ("bundle", and "file" and "files-done" for file copies), when enabled */
  bool accepts_type(std::string_view type);

  /** A request to /actions/clipboard, its arguments read up front since it's answered later */
  struct request_t {
    std::string device;
    std::string type;  ///< bundle, file or files-done
    std::string since;  ///< GET bundle: the state token the client saw last
    std::string have;  ///< GET bundle: hex hash of the content the client believes the PC holds
    bool automatic = false;  ///< GET bundle: triggered by focus changes rather than a shortcut; private content is withheld
    bool files = false;  ///< GET bundle: the client takes file copies
    std::string transfer;  ///< file and files-done: the copy (clipbundle::file_manifest::transfer)
    std::string index;  ///< file: the manifest entry
    std::string offset;  ///< file: where the chunk starts
    bool may_download_files = false;  ///< The client has the File Download permission
    bool may_upload_files = false;  ///< The client has the File Upload permission
    std::string body;  ///< POST
  };

  /** Reads a request's arguments; arg returns one, or an empty string when it's missing */
  request_t make_request(std::string device, std::string type, const std::function<std::string(const char *name)> &arg, bool may_download_files, bool may_upload_files);

  /**
   * GET bundle: replies with the PC's clipboard, or a small "unchanged" bundle when the client already
   * has it. GET file: a chunk of a copied file.
   */
  void get(request_t request, reply_fn reply);

  /**
   * POST bundle: replaces the PC's clipboard with the bundle and replies with the new state token and
   * hash; a bundle with files waits for them instead. POST file: a chunk of them; files-done: they're
   * all here, so they go on the clipboard.
   */
  void set(request_t request, reply_fn reply);
}  // namespace fork_tweaks::clipboard
