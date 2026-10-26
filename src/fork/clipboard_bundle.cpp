/**
 * @file src/fork/clipboard_bundle.cpp
 * @brief Fork-only: `/actions/clipboard?type=bundle`, rich clipboard sync for LuzLunar.
 */
// standard includes
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include "clipboard_bundle.h"
#include "clipboard_files.h"
#include "clipbundle.h"
#include "fork_config.h"
#include "src/logging.h"

#ifdef _WIN32
  // platform includes
  #include <objbase.h>

  // local includes
  #include "clipboard_win.h"
#endif

using namespace std::literals;

namespace fork_tweaks::clipboard {
#ifdef _WIN32
  namespace {
    constexpr int k_ok = 200;
    constexpr int k_unauthorized = 401;
    constexpr int k_conflict = 409;
    constexpr int k_too_large = 413;
    constexpr int k_bad_bundle = 422;
    constexpr int k_server_error = 500;
    constexpr int k_busy = 503;

    // Changes each time Apollo starts, so a token from before a restart never matches
    const std::string &instance() {
      static const std::string id = [] {
        std::random_device random;
        char buffer[9];
        std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned>(random()));
        return std::string(buffer);
      }();
      return id;
    }

    std::string token_for(std::uint32_t sequence) {
      return sequence != 0 ? instance() + "-" + std::to_string(sequence) : std::string();
    }

    // A bundle with no items that only reports the PC's clipboard state
    std::string state_reply(std::uint16_t flags, std::uint64_t etag, std::string token) {
      clipbundle::bundle b;
      b.flags = flags;
      b.etag = etag;
      b.token = std::move(token);
      return clipbundle::encode(b);
    }

    // Types and sizes for the log; never the content
    std::string describe(const std::vector<clipbundle::item> &items) {
      std::string types;
      std::size_t total = 0;
      for (const clipbundle::item &it : items) {
        types += (types.empty() ? "" : ", ") + it.type;
        total += it.data.size();
      }
      return (types.empty() ? "nothing"s : types) + " ("s + std::to_string(total) + " bytes)"s;
    }

    std::string join(const std::vector<std::string> &names) {
      std::string out;
      for (const std::string &name : names) {
        out += (out.empty() ? "" : ", ") + name;
      }
      return out;
    }

    // Runs one clipboard job at a time on a thread with COM initialized; others are turned away as busy
    class worker {
    public:
      static worker &instance() {
        // Never destroyed: the detached thread may outlive static destruction
        static worker *w = new worker();
        return *w;
      }

      bool submit(std::function<void()> job) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_job || m_running) {
          return false;
        }
        m_job = std::move(job);
        m_ready.notify_one();
        return true;
      }

    private:
      worker() {
        std::thread([this]() {
          run();
        }).detach();
      }

      void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock, [this]() {
              return bool(m_job);
            });
            job = std::move(m_job);
            m_job = nullptr;
            m_running = true;
          }
          job();
          std::lock_guard<std::mutex> lock(m_mutex);
          m_running = false;
        }
      }

      std::mutex m_mutex;
      std::condition_variable m_ready;
      std::function<void()> m_job;
      bool m_running = false;
    };

    void run_job(reply_fn reply, std::function<void(const reply_fn &)> job) {
      auto shared_reply = std::make_shared<reply_fn>(std::move(reply));
      bool queued = worker::instance().submit([shared_reply, job = std::move(job)]() {
        try {
          job(*shared_reply);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "clipboard: "sv << e.what();
          (*shared_reply)(k_server_error, {});
        } catch (...) {
          (*shared_reply)(k_server_error, {});
        }
      });
      if (!queued) {
        BOOST_LOG(debug) << "clipboard: busy with another request"sv;
        (*shared_reply)(k_busy, {});
      }
    }
  }  // namespace
#endif

  bool accepts_type(std::string_view type) {
#ifdef _WIN32
    if (type == "file"sv || type == "files-done"sv) {
      return clipboard_files::enabled();
    }
    return type == "bundle"sv && clipboard_rich;
#else
    (void) type;
    return false;
#endif
  }

  request_t make_request(std::string device, std::string type, const std::function<std::string(const char *name)> &arg, bool may_download_files, bool may_upload_files) {
    request_t r;
    r.device = std::move(device);
    r.type = std::move(type);
    r.since = arg("since");
    r.have = arg("have");
    r.automatic = arg("auto") == "1";
    r.files = arg("files") == "1";
    r.transfer = arg("transfer");
    r.index = arg("index");
    r.offset = arg("offset");
    r.may_download_files = may_download_files;
    r.may_upload_files = may_upload_files;
    return r;
  }

  void get(request_t request, reply_fn reply) {
#ifdef _WIN32
    if (request.type == "file"sv) {
      if (!request.may_download_files) {
        BOOST_LOG(debug) << "clipboard: ["sv << request.device << "] may not download files"sv;
        reply(k_unauthorized, {});
        return;
      }
      clipboard_files::get_chunk(std::move(request.device), std::move(request.transfer), std::move(request.index), std::move(request.offset), std::move(reply));
      return;
    }

    run_job(std::move(reply), [request = std::move(request)](const reply_fn &reply) {
      const std::string &device = request.device;

      // Unchanged since the client last looked: answer without opening the clipboard
      std::string token = token_for(clipboard_win::sequence());
      if (!token.empty() && token == request.since) {
        BOOST_LOG(debug) << "clipboard: ["sv << device << "] already has the PC's clipboard"sv;
        reply(k_ok, state_reply(clipbundle::flag_unchanged, 0, token));
        return;
      }

      clipboard_win::snapshot snapshot;
      if (!clipboard_win::read(snapshot)) {
        BOOST_LOG(warning) << "clipboard: couldn't open the clipboard for ["sv << device << ']';
        reply(k_busy, {});
        return;
      }
      token = token_for(snapshot.sequence);
      BOOST_LOG(debug) << "clipboard: formats offered: "sv << join(snapshot.formats);
      if (snapshot.files_through_ole) {
        BOOST_LOG(debug) << "clipboard: the copied files came from the clipboard's OLE data object"sv;
      }
      if (snapshot.items.empty() && snapshot.files.empty() && !snapshot.formats.empty()) {
        // Something is on the clipboard, but nothing that can be sent: which formats, so it can be added
        BOOST_LOG(info) << "clipboard: nothing on the PC's clipboard can be sent; formats: "sv << join(snapshot.formats);
      }
      if (snapshot.image_not_sent) {
        BOOST_LOG(info) << "clipboard: an image was offered but isn't sent (a text copy, or it couldn't be converted); formats: "sv << join(snapshot.formats);
      }

      if (snapshot.concealed && request.automatic) {
        BOOST_LOG(info) << "clipboard: not sending private content to ["sv << device << "] automatically"sv;
        reply(k_ok, state_reply(clipbundle::flag_concealed, 0, token));
        return;
      }

      // Copied files: their list goes first, so it's the last thing dropped from a bundle that's too large
      bool files_skipped = false;
      if (!snapshot.files.empty()) {
        if (clipboard_files::is_received_copy(snapshot.files)) {
          // The files a client just sent: they mustn't go back to it
          reply(k_ok, state_reply(clipbundle::flag_unchanged, 0, token));
          return;
        }
        if (request.files && request.may_download_files && clipboard_files::enabled()) {
          std::string manifest = clipboard_files::share(snapshot.files, files_skipped);
          if (!manifest.empty()) {
            snapshot.items.insert(snapshot.items.begin(), clipbundle::item {clipbundle::k_files_type, std::move(manifest)});
          }
        } else {
          // So the client can say why (its setting, or the File Download permission); older ones ignore the flag
          files_skipped = true;
        }
      }

      // The same content under a new sequence number (copied again, or rendered on demand)
      std::uint64_t etag = clipbundle::etag(snapshot.items);
      std::uint64_t client_has = 0;
      if (etag != 0 && clipbundle::parse_etag_hex(request.have, client_has) && client_has == etag) {
        BOOST_LOG(debug) << "clipboard: ["sv << device << "] already has this content"sv;
        reply(k_ok, state_reply(clipbundle::flag_unchanged, etag, token));
        return;
      }

      clipbundle::bundle b;
      b.flags = (snapshot.concealed ? clipbundle::flag_concealed : 0) | (files_skipped ? clipbundle::flag_files_skipped : 0);
      b.etag = etag;
      b.token = token;
      b.items = std::move(snapshot.items);
      std::string body;
      if (!clipbundle::encode_within_limit(b, body)) {
        BOOST_LOG(warning) << "clipboard: the clipboard is too large to send to ["sv << device << ']';
        reply(k_too_large, {});
        return;
      }

      BOOST_LOG(info) << "clipboard: sent "sv << describe(b.items) << " to ["sv << device << "], "sv << body.size() << " bytes on the wire"sv;
      reply(k_ok, std::move(body));
    });
#else
    (void) request;
    reply(400, {});
#endif
  }

#ifdef _WIN32
  namespace {
    /// Writes the clipboard and replies with its state as a later GET will see it, so it isn't sent back
    void write_and_reply(const std::string &device, const std::vector<clipbundle::item> &items, bool concealed, const std::vector<std::wstring> &files,
                         std::uint16_t flags, const reply_fn &reply) {
      std::uint32_t sequence = clipboard_win::write(items, concealed, files);
      if (sequence == 0) {
        BOOST_LOG(warning) << "clipboard: couldn't write the clipboard for ["sv << device << ']';
        reply(k_server_error, {});
        return;
      }
      BOOST_LOG(info) << "clipboard: received "sv << describe(items) << (files.empty() ? ""s : " and "s + std::to_string(files.size()) + " files or folders"s)
                      << " from ["sv << device << ']';

      clipboard_win::snapshot snapshot;
      std::uint64_t etag = 0;
      std::string token = token_for(sequence);
      if (clipboard_win::read(snapshot)) {
        etag = clipbundle::etag(snapshot.items);
        token = token_for(snapshot.sequence);
      }
      reply(k_ok, state_reply(flags, etag, token));
    }
  }  // namespace
#endif

  void set(request_t request, reply_fn reply) {
#ifdef _WIN32
    if (request.type == "file"sv) {
      if (!request.may_upload_files) {
        BOOST_LOG(debug) << "clipboard: ["sv << request.device << "] may not upload files"sv;
        reply(k_unauthorized, {});
        return;
      }
      clipboard_files::put_chunk(std::move(request.device), std::move(request.transfer), std::move(request.index), std::move(request.offset), std::move(request.body),
                                 std::move(reply));
      return;
    }

    if (request.type == "files-done"sv) {
      if (!request.may_upload_files) {
        reply(k_unauthorized, {});
        return;
      }
      run_job(std::move(reply), [request = std::move(request)](const reply_fn &reply) {
        std::vector<std::wstring> files;
        std::vector<clipbundle::item> items;
        if (!clipboard_files::finish(request.transfer, files, items)) {
          BOOST_LOG(warning) << "clipboard: ["sv << request.device << "]'s files aren't all here"sv;
          reply(k_conflict, {});
          return;
        }
        write_and_reply(request.device, items, false, files, 0, reply);
      });
      return;
    }

    run_job(std::move(reply), [request = std::move(request)](const reply_fn &reply) {
      const std::string &device = request.device;
      clipbundle::bundle b;
      std::string error;
      if (!clipbundle::decode(request.body, b, &error) || b.items.empty()) {
        BOOST_LOG(warning) << "clipboard: bad bundle from ["sv << device << "]: "sv << (error.empty() ? "no items"s : error);
        reply(k_bad_bundle, {});
        return;
      }

      // Copied files: the clipboard changes once they're all here (files-done)
      std::uint16_t flags = 0;
      auto files = std::find_if(b.items.begin(), b.items.end(), [](const clipbundle::item &it) {
        return it.type == clipbundle::k_files_type;
      });
      if (files != b.items.end()) {
        clipbundle::file_manifest manifest;
        bool decoded = clipbundle::decode_manifest(files->data, manifest, &error);
        b.items.erase(files);
        if (!decoded) {
          BOOST_LOG(warning) << "clipboard: bad file list from ["sv << device << "]: "sv << error;
        } else if (request.may_upload_files && clipboard_files::enabled() && clipboard_files::receive(device, std::move(manifest), b.items)) {
          reply(k_ok, state_reply(0, 0, {}));
          return;
        } else if (!request.may_upload_files) {
          BOOST_LOG(info) << "clipboard: ["sv << device << "] may not upload files, so only the rest of its copy is used"sv;
        }
        flags = clipbundle::flag_files_skipped;
        if (b.items.empty()) {
          reply(k_ok, state_reply(flags, 0, {}));
          return;
        }
      }

      write_and_reply(device, b.items, (b.flags & clipbundle::flag_concealed) != 0, {}, flags, reply);
    });
#else
    (void) request;
    reply(400, {});
#endif
  }
}  // namespace fork_tweaks::clipboard
