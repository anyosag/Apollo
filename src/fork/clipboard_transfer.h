/**
 * @file src/fork/clipboard_transfer.h
 * @brief Fork-only: the bookkeeping of a file copy coming in from LuzLunar chunk by chunk, and the
 * numbers in its requests (clipboard_files.h).
 *
 * Header-only and plain C++, so tests/fork/header/clipboard_transfer_test.cpp builds anywhere.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include "clipbundle.h"

namespace fork_tweaks::clipboard_transfer {
  enum class chunk_e {
    ok,
    unknown_entry,  ///< No such entry, or a folder
    left_out,  ///< An entry with no name here (clipbundle::local_paths())
    out_of_order,  ///< Not where the file stands: a file's chunks come in order
    too_long,  ///< Past the file's size
  };

  /// What an incoming copy has received
  class incoming_t {
  public:
    /// @param local The names to write the entries under (clipbundle::local_paths()), one per entry.
    incoming_t(clipbundle::file_manifest manifest, std::vector<std::string> local):
        m_manifest(std::move(manifest)),
        m_local(std::move(local)),
        m_received(m_manifest.entries.size(), 0) {
      m_local.resize(m_manifest.entries.size());
    }

    const clipbundle::file_manifest &manifest() const {
      return m_manifest;
    }

    const std::vector<std::string> &local() const {
      return m_local;
    }

    /// Whether a chunk of length bytes at offset in entry index can be written now
    chunk_e check(std::size_t index, std::uint64_t offset, std::size_t length) const {
      if (index >= m_manifest.entries.size() || m_manifest.entries[index].directory) {
        return chunk_e::unknown_entry;
      }
      if (m_local[index].empty()) {
        return chunk_e::left_out;
      }
      if (offset != m_received[index]) {
        return chunk_e::out_of_order;
      }
      if (length > m_manifest.entries[index].size - offset) {
        return chunk_e::too_long;
      }
      return chunk_e::ok;
    }

    /// A chunk that check() allowed was written
    void received(std::size_t index, std::size_t length) {
      m_received[index] += length;
    }

    bool file_complete(std::size_t index) const {
      return m_received[index] == m_manifest.entries[index].size;
    }

    /// Every file that has a name here has all its bytes
    bool complete() const {
      for (std::size_t i = 0; i < m_manifest.entries.size(); i++) {
        if (!m_manifest.entries[i].directory && !m_local[i].empty() && !file_complete(i)) {
          return false;
        }
      }
      return true;
    }

    /// Entries left out (no name here), for the log
    std::size_t left_out() const {
      return static_cast<std::size_t>(std::count_if(m_local.begin(), m_local.end(), [](const std::string &name) {
        return name.empty();
      }));
    }

  private:
    clipbundle::file_manifest m_manifest;
    std::vector<std::string> m_local;
    std::vector<std::uint64_t> m_received;
  };

  /// How many bytes to send for a chunk at offset of a file of size bytes; nothing when offset is past the end
  inline std::optional<std::size_t> chunk_length(std::uint64_t size, std::uint64_t offset) {
    if (offset > size) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(size - offset, clipbundle::k_file_chunk_bytes));
  }

  /// A request argument as a number: only plain decimal digits, up to 18 of them
  inline std::optional<std::uint64_t> parse_number(std::string_view text) {
    if (text.empty() || text.size() > 18) {
      return std::nullopt;
    }
    std::uint64_t value = 0;
    for (char c : text) {
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
  }
}  // namespace fork_tweaks::clipboard_transfer
