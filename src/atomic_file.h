/**
 * @file src/atomic_file.h
 * @brief Writes a file so that a crash leaves either the old contents or the new, never a mix.
 *
 * Header-only, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace file_handler {
  /**
   * @brief Writes contents to a temporary file next to path, then renames it over path.
   *
   * The rename replaces the old file in one step (std::filesystem::rename uses MoveFileExW with
   * MOVEFILE_REPLACE_EXISTING on Windows), so a crash or power loss mid-write can't leave a truncated
   * file behind; Apollo treats a credentials file it can't read as a reason to stop, not to start over.
   * @param path The file to write.
   * @param contents What to write.
   * @param owner_only On POSIX, make the file readable and writable by its owner only (0600).
   * @return True on success.
   */
  inline bool write_file_atomic(const std::filesystem::path &path, std::string_view contents, bool owner_only = false) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    std::error_code ec;

    {
      std::ofstream out(temp, std::ios::trunc);
      if (!out.is_open()) {
        return false;
      }
      out << contents;
      out.flush();
      if (!out) {
        out.close();
        std::filesystem::remove(temp, ec);
        return false;
      }
    }

    if (owner_only) {
      std::filesystem::permissions(temp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
      std::filesystem::remove(temp, ec);
      return false;
    }
    return true;
  }
}  // namespace file_handler
