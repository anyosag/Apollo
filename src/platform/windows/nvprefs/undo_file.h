/**
 * @file src/platform/windows/nvprefs/undo_file.h
 * @brief Declarations for the nvidia undo file.
 */
#pragma once

// standard includes
#include <filesystem>

// local includes
#include "nvprefs_common.h"
#include "undo_data.h"

namespace nvprefs {

  /**
   * @brief Creates the undo file's folder with SYSTEM- and Administrators-only permissions, or checks
   * that an existing one is a plain folder owned by SYSTEM or Administrators (and restricts it).
   * @return False if the folder can't be used.
   */
  bool prepare_undo_folder(const std::filesystem::path &folder);

  class undo_file_t {
  public:
    static std::optional<undo_file_t> open_existing_file(std::filesystem::path file_path, bool &access_denied);

    static std::optional<undo_file_t> create_new_file(std::filesystem::path file_path);

    bool delete_file();

    bool write_undo_data(const undo_data_t &undo_data);

    std::optional<undo_data_t> read_undo_data();

  private:
    undo_file_t() = default;
    safe_handle file_handle;
  };

}  // namespace nvprefs
