/**
 * @file src/fork/clipboard_files.cpp
 * @brief Fork-only: files copied to the clipboard, between the PC and LuzLunar.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

// local includes
#include "clipboard_files.h"
#include "clipboard_transfer.h"
#include "fork_config.h"
#include "src/logging.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <userenv.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::clipboard_files {
  bool enabled() {
#ifdef _WIN32
    return clipboard_file_copies && clipboard_rich;
#else
    return false;
#endif
  }

#ifdef _WIN32
  namespace {
    constexpr int k_ok = 200;
    constexpr int k_bad_request = 400;
    constexpr int k_not_found = 404;
    constexpr int k_conflict = 409;
    constexpr int k_too_large = 413;
    constexpr int k_out_of_range = 416;
    constexpr int k_server_error = 500;

    /// A share lasts this long after its last chunk, so a slow copy isn't cut off
    constexpr auto k_share_lifetime = 15min;
    constexpr std::size_t k_shares_kept = 2;
    constexpr std::size_t k_receiving_kept = 2;

    /// Received copies stay for pasting: the newest few, for a day
    constexpr std::size_t k_received_folders_kept = 3;
    constexpr auto k_received_folder_age = 24h;

    const wchar_t *const k_folder_name = L"LuzLunar Clipboard";

    using clock = std::chrono::steady_clock;

    struct shared_file_t {
      std::wstring path;
      std::uint64_t size = 0;
      std::int64_t mtime = 0;
      bool directory = false;
    };

    struct share_t {
      std::string transfer;
      std::vector<shared_file_t> files;  ///< One per manifest entry
      clock::time_point used;
      bool logged = false;
    };

    struct receiving_t {
      std::string device;
      std::filesystem::path folder;
      clipboard_transfer::incoming_t progress;
      std::vector<clipbundle::item> items;
    };

    std::mutex state_mutex;
    std::deque<share_t> shares;  ///< Newest last
    std::deque<std::pair<std::string, std::shared_ptr<receiving_t>>> receiving;  ///< Newest last
    std::vector<std::wstring> last_received;  ///< The top-level paths of the last copy put on the clipboard

    std::string utf8(std::wstring_view text) {
      if (text.empty()) {
        return {};
      }
      int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      std::string out(size, '\0');
      WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
      return out;
    }

    std::wstring wide(std::string_view text) {
      if (text.empty()) {
        return {};
      }
      int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring out(size, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size);
      return out;
    }

    /// 100 ns intervals between 1601 and 1970
    constexpr std::int64_t k_epoch_difference = 116444736000000000LL;

    std::int64_t unix_seconds(const FILETIME &time) {
      ULARGE_INTEGER value;
      value.LowPart = time.dwLowDateTime;
      value.HighPart = time.dwHighDateTime;
      return (static_cast<std::int64_t>(value.QuadPart) - k_epoch_difference) / 10000000;
    }

    FILETIME file_time(std::int64_t seconds) {
      ULARGE_INTEGER value;
      value.QuadPart = static_cast<ULONGLONG>(seconds * 10000000 + k_epoch_difference);
      FILETIME time;
      time.dwLowDateTime = value.LowPart;
      time.dwHighDateTime = value.HighPart;
      return time;
    }

    std::uint64_t file_size(DWORD high, DWORD low) {
      return (static_cast<std::uint64_t>(high) << 32) | low;
    }

    /// A path Windows' file functions take past MAX_PATH
    std::wstring long_path(const std::wstring &path) {
      if (path.size() < MAX_PATH - 12 || path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
      }
      if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
      }
      return L"\\\\?\\" + path;
    }

    /// The last part of a path, without a trailing separator; empty for a drive's root
    std::wstring last_part(std::wstring path) {
      while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
      }
      std::size_t separator = path.find_last_of(L"\\/");
      std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
      return name.find(L':') != std::wstring::npos ? std::wstring() : name;
    }

    /// Links and junctions inside a folder: following them could leave it, or loop
    bool name_surrogate(const WIN32_FIND_DATAW &data) {
      return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(data.dwReserved0);
    }

    /// Builds a manifest from CF_HDROP's paths, walking folders; false when over the limits
    class walker_t {
    public:
      bool add_root(const std::wstring &path) {
        std::wstring name = last_part(path);
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (name.empty() || !GetFileAttributesExW(long_path(path).c_str(), GetFileExInfoStandard, &data)) {
          m_unreadable++;
          return true;
        }
        bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!add(path, utf8(name), directory, file_size(data.nFileSizeHigh, data.nFileSizeLow), unix_seconds(data.ftLastWriteTime))) {
          return false;
        }
        return !directory || walk(path, utf8(name));
      }

      clipbundle::file_manifest manifest;
      std::vector<shared_file_t> files;
      std::size_t unreadable() const {
        return m_unreadable;
      }

    private:
      bool add(const std::wstring &path, std::string relative, bool directory, std::uint64_t size, std::int64_t mtime) {
        if (manifest.entries.size() >= clipbundle::k_max_file_entries || relative.size() > clipbundle::k_max_path_bytes) {
          return false;
        }
        if (!directory && size > clipbundle::k_max_file_bytes - m_bytes) {
          return false;
        }
        m_bytes += directory ? 0 : size;

        clipbundle::file_entry entry;
        entry.path = std::move(relative);
        entry.size = directory ? 0 : size;
        entry.mtime = mtime;
        entry.directory = directory;
        manifest.entries.push_back(std::move(entry));
        files.push_back({path, directory ? 0 : size, mtime, directory});
        return true;
      }

      bool walk(const std::wstring &folder, const std::string &relative) {
        // Depth first, each folder listed before what it holds
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExW(long_path(folder + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
          m_unreadable++;
          return true;
        }
        bool ok = true;
        do {
          std::wstring name = data.cFileName;
          if (name == L"." || name == L".." || name_surrogate(data)) {
            continue;
          }
          std::wstring path = folder + L"\\" + name;
          std::string child = relative + "/" + utf8(name);
          bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
          if (!add(path, child, directory, file_size(data.nFileSizeHigh, data.nFileSizeLow), unix_seconds(data.ftLastWriteTime)) ||
              (directory && !walk(path, child))) {
            ok = false;
            break;
          }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return ok;
      }

      std::uint64_t m_bytes = 0;
      std::size_t m_unreadable = 0;
    };

    /// Runs chunk reads and writes in order on a thread of their own
    class worker_t {
    public:
      static worker_t &instance() {
        // Never destroyed: the detached thread may outlive static destruction
        static worker_t *w = new worker_t();
        return *w;
      }

      void submit(std::function<void()> job) {
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          m_jobs.push_back(std::move(job));
        }
        m_ready.notify_one();
      }

    private:
      worker_t() {
        std::thread([this]() {
          run();
        }).detach();
      }

      void run() {
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock, [this]() {
              return !m_jobs.empty();
            });
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
          }
          try {
            job();
          } catch (const std::exception &e) {
            BOOST_LOG(error) << "clipboard: file transfer: "sv << e.what();
          }
        }
      }

      std::mutex m_mutex;
      std::condition_variable m_ready;
      std::deque<std::function<void()>> m_jobs;
    };

    /// The signed-in user's temp folder: Apollo runs as SYSTEM, whose own temp the user can't open
    std::filesystem::path user_temp() {
      if (platf::is_running_as_system()) {
        HANDLE token = platf::retrieve_users_token(false);
        if (!token) {
          return {};
        }
        wchar_t buffer[MAX_PATH * 2];
        BOOL ok = ExpandEnvironmentStringsForUserW(token, L"%TEMP%", buffer, static_cast<DWORD>(std::size(buffer)));
        CloseHandle(token);
        if (!ok || buffer[0] == L'%') {
          return {};
        }
        return std::filesystem::path(buffer);
      }
      wchar_t buffer[MAX_PATH + 1];
      DWORD length = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
      return length > 0 && length <= MAX_PATH ? std::filesystem::path(std::wstring(buffer, length)) : std::filesystem::path();
    }

    /// Deletes received copies past the newest few, or older than a day, except those still arriving
    void tidy(const std::filesystem::path &base) {
      std::error_code ec;
      std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> folders;
      for (const auto &entry : std::filesystem::directory_iterator(base, ec)) {
        if (entry.is_directory(ec) && clipbundle::valid_transfer_id(entry.path().filename().string())) {
          folders.emplace_back(entry.last_write_time(ec), entry.path());
        }
      }
      std::sort(folders.begin(), folders.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
      });

      auto now = std::filesystem::file_time_type::clock::now();
      for (std::size_t i = 0; i < folders.size(); i++) {
        std::string name = folders[i].second.filename().string();
        bool arriving = std::any_of(receiving.begin(), receiving.end(), [&name](const auto &r) {
          return r.first == name;
        });
        if (!arriving && (i >= k_received_folders_kept || now - folders[i].first > k_received_folder_age)) {
          std::filesystem::remove_all(folders[i].second, ec);
        }
      }
    }

    void write_chunk(const std::shared_ptr<receiving_t> &r, std::size_t index, std::uint64_t offset, const std::string &body, const reply_fn &reply) {
      const clipbundle::file_entry &entry = r->progress.manifest().entries[index];
      std::filesystem::path path = r->folder / wide(r->progress.local()[index]);
      HANDLE file = CreateFileW(long_path(path.make_preferred().wstring()).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        BOOST_LOG(warning) << "clipboard: couldn't write a received file: error "sv << GetLastError();
        reply(k_server_error, {});
        return;
      }

      LARGE_INTEGER position;
      position.QuadPart = static_cast<LONGLONG>(offset);
      bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0;
      std::size_t written = 0;
      while (ok && written < body.size()) {
        DWORD count = 0;
        DWORD part = static_cast<DWORD>(std::min<std::size_t>(body.size() - written, 1 << 20));
        ok = WriteFile(file, body.data() + written, part, &count, nullptr) != 0 && count == part;
        written += count;
      }
      DWORD error = ok ? 0 : GetLastError();
      if (ok && offset + body.size() == entry.size && entry.mtime != 0) {
        FILETIME modified = file_time(entry.mtime);
        SetFileTime(file, nullptr, nullptr, &modified);
      }
      CloseHandle(file);
      if (!ok) {
        BOOST_LOG(warning) << "clipboard: couldn't write a received file: error "sv << error;
        reply(k_server_error, {});
        return;
      }

      {
        std::lock_guard<std::mutex> lock(state_mutex);
        r->progress.received(index, body.size());
      }
      reply(k_ok, {});
    }

    void read_chunk(shared_file_t file, std::uint64_t offset, const reply_fn &reply) {
      HANDLE handle = CreateFileW(long_path(file.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        BOOST_LOG(warning) << "clipboard: couldn't open a file to send: error "sv << GetLastError();
        reply(k_server_error, {});
        return;
      }

      // A file that changed since it was copied would arrive as a mix of the two
      BY_HANDLE_FILE_INFORMATION info;
      if (!GetFileInformationByHandle(handle, &info) || file_size(info.nFileSizeHigh, info.nFileSizeLow) != file.size ||
          unix_seconds(info.ftLastWriteTime) != file.mtime) {
        CloseHandle(handle);
        reply(k_conflict, {});
        return;
      }

      auto length = clipboard_transfer::chunk_length(file.size, offset);
      if (!length) {
        CloseHandle(handle);
        reply(k_out_of_range, {});
        return;
      }

      std::string data(*length, '\0');
      LARGE_INTEGER position;
      position.QuadPart = static_cast<LONGLONG>(offset);
      bool ok = SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0;
      std::size_t done = 0;
      while (ok && done < data.size()) {
        DWORD count = 0;
        DWORD part = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1 << 20));
        ok = ReadFile(handle, &data[done], part, &count, nullptr) != 0 && count > 0;
        done += count;
      }
      DWORD error = ok ? 0 : GetLastError();
      CloseHandle(handle);
      if (!ok) {
        BOOST_LOG(warning) << "clipboard: couldn't read a file to send: error "sv << error;
        reply(k_server_error, {});
        return;
      }
      reply(k_ok, std::move(data));
    }
  }  // namespace

  std::string share(const std::vector<std::wstring> &paths, bool &skipped) {
    skipped = false;
    walker_t walker;
    for (const std::wstring &path : paths) {
      if (!walker.add_root(path)) {
        BOOST_LOG(info) << "clipboard: the copied files are too many or too large to send (over "sv << clipbundle::k_max_file_entries << " files or "sv
                        << (clipbundle::k_max_file_bytes >> 30) << " GB)"sv;
        skipped = true;
        return {};
      }
    }
    if (walker.manifest.entries.empty()) {
      skipped = !paths.empty();
      return {};
    }
    if (walker.unreadable() > 0) {
      BOOST_LOG(info) << "clipboard: "sv << walker.unreadable() << " of the copied files or folders couldn't be read, and aren't sent"sv;
    }

    walker.manifest.transfer = clipbundle::transfer_id(walker.manifest.entries);
    std::string encoded = clipbundle::encode_manifest(walker.manifest);

    std::lock_guard<std::mutex> lock(state_mutex);
    shares.erase(std::remove_if(shares.begin(), shares.end(), [&walker](const share_t &s) {
                   return s.transfer == walker.manifest.transfer;
                 }),
                 shares.end());
    shares.push_back({walker.manifest.transfer, std::move(walker.files), clock::now(), false});
    while (shares.size() > k_shares_kept) {
      shares.pop_front();
    }
    return encoded;
  }

  bool is_received_copy(const std::vector<std::wstring> &paths) {
    std::lock_guard<std::mutex> lock(state_mutex);
    return !last_received.empty() && paths == last_received;
  }

  bool receive(const std::string &device, clipbundle::file_manifest manifest, std::vector<clipbundle::item> items) {
    std::filesystem::path temp = user_temp();
    if (temp.empty()) {
      BOOST_LOG(warning) << "clipboard: nobody is signed in to receive ["sv << device << "]'s files for"sv;
      return false;
    }
    std::filesystem::path base = temp / k_folder_name;
    std::filesystem::path folder = base / manifest.transfer;
    std::vector<std::string> local = clipbundle::local_paths(manifest);

    std::error_code ec;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      receiving.erase(std::remove_if(receiving.begin(), receiving.end(), [&manifest](const auto &r) {
                        return r.first == manifest.transfer;
                      }),
                      receiving.end());
      tidy(base);
    }
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    if (ec) {
      BOOST_LOG(warning) << "clipboard: couldn't make a folder for ["sv << device << "]'s files: "sv << ec.message();
      return false;
    }

    // Folders and empty files now, as no chunk comes for them
    bool any = false;
    for (std::size_t i = 0; i < manifest.entries.size(); i++) {
      if (local[i].empty()) {
        continue;
      }
      std::filesystem::path path = (folder / wide(local[i])).make_preferred();
      const clipbundle::file_entry &entry = manifest.entries[i];
      if (entry.directory) {
        std::filesystem::create_directories(long_path(path.wstring()), ec);
      } else {
        std::filesystem::create_directories(long_path(path.parent_path().wstring()), ec);
        HANDLE file = CreateFileW(long_path(path.wstring()).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
          local[i].clear();
          continue;
        }
        if (entry.size == 0 && entry.mtime != 0) {
          FILETIME modified = file_time(entry.mtime);
          SetFileTime(file, nullptr, nullptr, &modified);
        }
        CloseHandle(file);
      }
      any = true;
    }
    if (!any) {
      std::filesystem::remove_all(folder, ec);
      return false;
    }

    std::uint64_t bytes = clipbundle::manifest_bytes(manifest);
    std::size_t entries = manifest.entries.size();
    std::string transfer = manifest.transfer;
    auto r = std::make_shared<receiving_t>(receiving_t {device, folder, clipboard_transfer::incoming_t(std::move(manifest), std::move(local)), std::move(items)});
    if (r->progress.left_out() > 0) {
      BOOST_LOG(info) << "clipboard: "sv << r->progress.left_out() << " of ["sv << device << "]'s files have names already taken, and are left out"sv;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    receiving.emplace_back(transfer, std::move(r));
    while (receiving.size() > k_receiving_kept) {
      receiving.pop_front();
    }
    BOOST_LOG(info) << "clipboard: receiving "sv << entries << " files and folders ("sv << bytes << " bytes) from ["sv << device << ']';
    return true;
  }

  bool finish(const std::string &transfer, std::vector<std::wstring> &paths, std::vector<clipbundle::item> &items) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto it = std::find_if(receiving.begin(), receiving.end(), [&transfer](const auto &r) {
      return r.first == transfer;
    });
    if (it == receiving.end() || !it->second->progress.complete()) {
      return false;
    }

    const receiving_t &r = *it->second;
    paths.clear();
    for (std::size_t index : clipbundle::top_level(r.progress.manifest())) {
      const std::string &name = r.progress.local()[index];
      if (!name.empty()) {
        paths.push_back((r.folder / wide(name)).make_preferred().wstring());
      }
    }
    items = r.items;
    last_received = paths;
    BOOST_LOG(info) << "clipboard: received ["sv << r.device << "]'s files into "sv << utf8(r.folder.wstring());
    receiving.erase(it);
    return !paths.empty();
  }

  void get_chunk(std::string device, std::string transfer, std::string index, std::string offset, reply_fn reply) {
    worker_t::instance().submit([device = std::move(device), transfer = std::move(transfer), index = std::move(index), offset = std::move(offset), reply = std::move(reply)]() {
      auto entry = clipboard_transfer::parse_number(index);
      auto from = clipboard_transfer::parse_number(offset);
      if (!entry || !from) {
        reply(k_bad_request, {});
        return;
      }

      shared_file_t file;
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto now = clock::now();
        shares.erase(std::remove_if(shares.begin(), shares.end(), [now](const share_t &s) {
                       return now - s.used > k_share_lifetime;
                     }),
                     shares.end());
        auto it = std::find_if(shares.begin(), shares.end(), [&transfer](const share_t &s) {
          return s.transfer == transfer;
        });
        if (it == shares.end() || *entry >= it->files.size() || it->files[*entry].directory) {
          reply(k_not_found, {});
          return;
        }
        it->used = now;
        if (!it->logged) {
          it->logged = true;
          BOOST_LOG(info) << "clipboard: ["sv << device << "] is copying the PC's copied files"sv;
        }
        file = it->files[*entry];
      }
      read_chunk(std::move(file), *from, reply);
    });
  }

  void put_chunk(std::string device, std::string transfer, std::string index, std::string offset, std::string body, reply_fn reply) {
    worker_t::instance().submit([device = std::move(device), transfer = std::move(transfer), index = std::move(index), offset = std::move(offset), body = std::move(body), reply = std::move(reply)]() {
      auto entry = clipboard_transfer::parse_number(index);
      auto from = clipboard_transfer::parse_number(offset);
      if (!entry || !from) {
        reply(k_bad_request, {});
        return;
      }

      std::shared_ptr<receiving_t> r;
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto it = std::find_if(receiving.begin(), receiving.end(), [&transfer](const auto &item) {
          return item.first == transfer;
        });
        if (it == receiving.end() || it->second->device != device) {
          reply(k_not_found, {});
          return;
        }
        r = it->second;
        switch (r->progress.check(static_cast<std::size_t>(*entry), *from, body.size())) {
          case clipboard_transfer::chunk_e::ok:
            break;
          case clipboard_transfer::chunk_e::out_of_order:
            reply(k_conflict, {});
            return;
          case clipboard_transfer::chunk_e::too_long:
            reply(k_too_large, {});
            return;
          case clipboard_transfer::chunk_e::unknown_entry:
          case clipboard_transfer::chunk_e::left_out:
          default:
            reply(k_not_found, {});
            return;
        }
      }
      write_chunk(r, static_cast<std::size_t>(*entry), *from, body, reply);
    });
  }
#else
  void get_chunk(std::string, std::string, std::string, std::string, reply_fn reply) {
    reply(400, {});
  }

  void put_chunk(std::string, std::string, std::string, std::string, std::string, reply_fn reply) {
    reply(400, {});
  }
#endif
}  // namespace fork_tweaks::clipboard_files
