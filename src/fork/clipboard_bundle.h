/**
 * @file src/fork/clipboard_bundle.h
 * @brief Fork-only: `/actions/clipboard?type=bundle`, rich clipboard sync for LuzLunar.
 *
 * Text, RTF, HTML, images and Office formats travel in one clipbundle (see clipbundle.h). Clipboard
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

  /** True for the clipboard types this build adds ("bundle"), when enabled */
  bool accepts_type(std::string_view type);

  /**
   * GET: replies with the PC's clipboard, or a small "unchanged" bundle when the client already has it.
   * @param since The state token the client saw last.
   * @param have Hex hash of the content the client believes the PC holds.
   * @param automatic Sync triggered by focus changes rather than a shortcut; private content is withheld.
   */
  void get(std::string device, std::string since, std::string have, bool automatic, reply_fn reply);

  /** POST: replaces the PC's clipboard with the bundle and replies with the new state token and hash */
  void set(std::string device, std::string body, reply_fn reply);
}  // namespace fork_tweaks::clipboard
