/**
 * @file src/fork/update_download.h
 * @brief Downloads for over-the-air updates. Unlike http::download_file(), these follow GitHub's
 * redirects to its asset hosts, but only over HTTPS to the hosts allowed_download_url() accepts,
 * and stop at a size limit and on stalled transfers.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace fork_update {
  /**
   * @brief Download a small file into memory.
   * @param url Where from; it and every redirect must pass allowed_download_url().
   * @param max_bytes Fail if it's larger.
   * @param error Set to what went wrong.
   * @param cancel Stops the download when it becomes true.
   * @return The body of a 200 response, or nothing.
   */
  std::optional<std::string> fetch(const std::string &url, std::size_t max_bytes, std::string *error, const std::atomic<bool> *cancel = nullptr);

  /**
   * @brief Download into a file: written to `<file>.part`, then renamed over @p file.
   * @param url Where from; it and every redirect must pass allowed_download_url().
   * @param file Where to.
   * @param max_bytes Fail if it's larger.
   * @param error Set to what went wrong.
   * @param cancel Stops the download when it becomes true.
   */
  bool fetch_to_file(const std::string &url, const std::filesystem::path &file, uint64_t max_bytes, std::string *error, const std::atomic<bool> *cancel = nullptr);
}  // namespace fork_update
