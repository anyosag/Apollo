/**
 * @file src/fork/clipboard_files.h
 * @brief Fork-only: files copied to the clipboard, between the PC and LuzLunar (clipbundle.h, "File copies").
 *
 * PC to Mac: when the PC's clipboard lists files (Explorer's copy), share() walks them into the
 * manifest the bundle carries and keeps their paths, and the client fetches each file in chunks
 * with GET /actions/clipboard?type=file. Mac to PC: a bundle with a manifest starts receive(), the
 * chunks arrive with POST type=file into a folder in the signed-in user's temp folder, and
 * type=files-done puts the files on the clipboard (clipboard_bundle.cpp). Chunks are read and
 * written on a thread of their own, so they never wait on the clipboard.
 */
#pragma once

// standard includes
#include <functional>
#include <string>
#include <vector>

// local includes
#include "clipbundle.h"

namespace fork_tweaks::clipboard_files {
  /** Sends the response; called from the file thread */
  using reply_fn = std::function<void(int status, std::string body)>;

  /** Whether file copies are on: fork_clipboard_files and fork_clipboard_rich, on Windows */
  bool enabled();

#ifdef _WIN32
  /**
   * @brief Shares the files the PC's clipboard lists: walks folders into a manifest and keeps their paths.
   * @param paths CF_HDROP's full paths.
   * @param skipped Set when there were files but they aren't shared: too many, too large, or unreadable.
   * @return The manifest item's data; empty when nothing is shared.
   */
  std::string share(const std::vector<std::wstring> &paths, bool &skipped);

  /** Whether the clipboard's files are the last copy received from a client, which mustn't go back to it */
  bool is_received_copy(const std::vector<std::wstring> &paths);

  /**
   * @brief Starts receiving a copy from a client, into a new folder of the signed-in user's temp folder.
   * @param items The bundle's other items, which go on the clipboard with the files.
   * @return False when it can't: no user folder, or nothing in the manifest can be written.
   */
  bool receive(const std::string &device, clipbundle::file_manifest manifest, std::vector<clipbundle::item> items);

  /**
   * @brief Once every file of a received copy has arrived: the paths and items to put on the clipboard.
   * @return False when the copy is unknown, or still missing bytes.
   */
  bool finish(const std::string &transfer, std::vector<std::wstring> &paths, std::vector<clipbundle::item> &items);
#endif

  /** GET type=file: up to k_file_chunk_bytes of a shared file, from offset */
  void get_chunk(std::string device, std::string transfer, std::string index, std::string offset, reply_fn reply);

  /** POST type=file: the next chunk of a file being received */
  void put_chunk(std::string device, std::string transfer, std::string index, std::string offset, std::string body, reply_fn reply);
}  // namespace fork_tweaks::clipboard_files
