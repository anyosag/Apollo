/**
 * @file src/platform/windows/nvprefs/undo_file.cpp
 * @brief Definitions for the nvidia undo file.
 */
// local includes
#include "undo_file.h"

// platform includes (sddl.h needs windows.h, which undo_file.h includes)
#include <sddl.h>

namespace {

  using namespace nvprefs;

  /// SYSTEM and Administrators only, owned by Administrators, and not inherited from %ProgramData%,
  /// where every user may create files and folders
  constexpr auto protected_folder_sddl = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

  bool owner_is_trusted(PSID owner) {
    return owner != nullptr && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
  }

}  // namespace

namespace nvprefs {

  bool prepare_undo_folder(const std::filesystem::path &folder) {
    PSECURITY_DESCRIPTOR raw_sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(protected_folder_sddl, SDDL_REVISION_1, &raw_sd, nullptr)) {
      error_message("Couldn't build the undo folder's permissions");
      return false;
    }
    safe_hlocal<PSECURITY_DESCRIPTOR> sd {raw_sd};

    SECURITY_ATTRIBUTES sa {sizeof(sa), sd.get(), FALSE};
    if (CreateDirectoryW(folder.c_str(), &sa)) {
      return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      error_message("Couldn't create undo folder");
      return false;
    }

    // It exists. SYSTEM creates, reads and deletes files in it, so use it only if it's a real folder
    // (not a link elsewhere) that SYSTEM or Administrators own; another user could have made it first.
    safe_handle dir {CreateFileW(folder.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!dir) {
      error_message("Couldn't open undo folder");
      return false;
    }

    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(dir.get(), &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      error_message(L"Not using " + folder.wstring() + L": it isn't a plain folder. Delete it to let Apollo recreate it.");
      return false;
    }

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR raw_current = nullptr;
    if (GetSecurityInfo(dir.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &raw_current) != ERROR_SUCCESS) {
      error_message("Couldn't read the undo folder's owner");
      return false;
    }
    safe_hlocal<PSECURITY_DESCRIPTOR> current {raw_current};
    if (!owner_is_trusted(owner)) {
      error_message(L"Not using " + folder.wstring() + L": it's owned by another account, not SYSTEM or Administrators. Delete it to let Apollo recreate it.");
      return false;
    }

    // Folders made by earlier versions inherited %ProgramData%'s permissions; replace them
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(sd.get(), &present, &dacl, &defaulted) ||
        SetSecurityInfo(dir.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) != ERROR_SUCCESS) {
      error_message("Couldn't restrict the undo folder's permissions");
      return false;
    }
    return true;
  }

  std::optional<undo_file_t> undo_file_t::open_existing_file(std::filesystem::path file_path, bool &access_denied) {
    undo_file_t file;
    // Not following links: SYSTEM deletes this file, so it must be the file itself
    file.file_handle.reset(CreateFileW(file_path.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.file_handle) {
      BY_HANDLE_FILE_INFORMATION info {};
      if (!GetFileInformationByHandle(file.file_handle.get(), &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1) {
        error_message("The undo file isn't a plain file; not using it");
        file.file_handle.reset();
        access_denied = true;
        return std::nullopt;
      }
      access_denied = false;
      return file;
    } else {
      auto last_error = GetLastError();
      access_denied = (last_error != ERROR_FILE_NOT_FOUND && last_error != ERROR_PATH_NOT_FOUND);
      return std::nullopt;
    }
  }

  std::optional<undo_file_t> undo_file_t::create_new_file(std::filesystem::path file_path) {
    undo_file_t file;
    // It inherits the undo folder's permissions: SYSTEM and Administrators only
    file.file_handle.reset(CreateFileW(file_path.c_str(), GENERIC_WRITE | STANDARD_RIGHTS_ALL, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));

    if (file.file_handle) {
      return file;
    } else {
      return std::nullopt;
    }
  }

  bool undo_file_t::delete_file() {
    if (!file_handle) {
      return false;
    }

    FILE_DISPOSITION_INFO delete_file_info = {TRUE};
    if (SetFileInformationByHandle(file_handle.get(), FileDispositionInfo, &delete_file_info, sizeof(delete_file_info))) {
      file_handle.reset();
      return true;
    } else {
      return false;
    }
  }

  bool undo_file_t::write_undo_data(const undo_data_t &undo_data) {
    if (!file_handle) {
      return false;
    }

    std::string buffer;
    try {
      buffer = undo_data.write();
    } catch (...) {
      error_message("Couldn't serialize undo data");
      return false;
    }

    if (!SetFilePointerEx(file_handle.get(), {}, nullptr, FILE_BEGIN) || !SetEndOfFile(file_handle.get())) {
      error_message("Couldn't clear undo file");
      return false;
    }

    DWORD bytes_written = 0;
    if (!WriteFile(file_handle.get(), buffer.data(), buffer.size(), &bytes_written, nullptr) || bytes_written != buffer.size()) {
      error_message("Couldn't write undo file");
      return false;
    }

    if (!FlushFileBuffers(file_handle.get())) {
      error_message("Failed to flush undo file");
    }

    return true;
  }

  std::optional<undo_data_t> undo_file_t::read_undo_data() {
    if (!file_handle) {
      return std::nullopt;
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle.get(), &file_size)) {
      error_message("Couldn't get undo file size");
      return std::nullopt;
    }

    if ((size_t) file_size.QuadPart > 1024) {
      error_message("Undo file size is unexpectedly large, aborting");
      return std::nullopt;
    }

    std::vector<char> buffer(file_size.QuadPart);
    DWORD bytes_read = 0;
    if (!ReadFile(file_handle.get(), buffer.data(), buffer.size(), &bytes_read, nullptr) || bytes_read != buffer.size()) {
      error_message("Couldn't read undo file");
      return std::nullopt;
    }

    undo_data_t undo_data;
    try {
      undo_data.read(buffer);
    } catch (...) {
      error_message("Couldn't parse undo file");
      return std::nullopt;
    }
    return undo_data;
  }

}  // namespace nvprefs
