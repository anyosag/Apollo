/**
 * @file src/fork/clipboard_bundle.cpp
 * @brief Fork-only: `/actions/clipboard?type=bundle`, rich clipboard sync for LuzLunar.
 */
// standard includes
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
    return type == "bundle"sv && clipboard_rich;
#else
    (void) type;
    return false;
#endif
  }

  void get(std::string device, std::string since, std::string have, bool automatic, reply_fn reply) {
#ifdef _WIN32
    run_job(std::move(reply), [device = std::move(device), since = std::move(since), have = std::move(have), automatic](const reply_fn &reply) {
      // Unchanged since the client last looked: answer without opening the clipboard
      std::string token = token_for(clipboard_win::sequence());
      if (!token.empty() && token == since) {
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

      if (snapshot.concealed && automatic) {
        BOOST_LOG(info) << "clipboard: not sending private content to ["sv << device << "] automatically"sv;
        reply(k_ok, state_reply(clipbundle::flag_concealed, 0, token));
        return;
      }

      // The same content under a new sequence number (copied again, or rendered on demand)
      std::uint64_t etag = clipbundle::etag(snapshot.items);
      std::uint64_t client_has = 0;
      if (etag != 0 && clipbundle::parse_etag_hex(have, client_has) && client_has == etag) {
        BOOST_LOG(debug) << "clipboard: ["sv << device << "] already has this content"sv;
        reply(k_ok, state_reply(clipbundle::flag_unchanged, etag, token));
        return;
      }

      clipbundle::bundle b;
      b.flags = snapshot.concealed ? clipbundle::flag_concealed : 0;
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
    (void) device;
    (void) since;
    (void) have;
    (void) automatic;
    reply(400, {});
#endif
  }

  void set(std::string device, std::string body, reply_fn reply) {
#ifdef _WIN32
    run_job(std::move(reply), [device = std::move(device), body = std::move(body)](const reply_fn &reply) {
      clipbundle::bundle b;
      std::string error;
      if (!clipbundle::decode(body, b, &error) || b.items.empty()) {
        BOOST_LOG(warning) << "clipboard: bad bundle from ["sv << device << "]: "sv << (error.empty() ? "no items"s : error);
        reply(k_bad_bundle, {});
        return;
      }

      std::uint32_t sequence = clipboard_win::write(b.items, (b.flags & clipbundle::flag_concealed) != 0);
      if (sequence == 0) {
        BOOST_LOG(warning) << "clipboard: couldn't write the clipboard for ["sv << device << ']';
        reply(k_server_error, {});
        return;
      }
      BOOST_LOG(info) << "clipboard: received "sv << describe(b.items) << " from ["sv << device << ']';

      // Report the content as a later GET will see it, so it isn't sent back to the client
      clipboard_win::snapshot snapshot;
      std::uint64_t etag = 0;
      std::string token = token_for(sequence);
      if (clipboard_win::read(snapshot)) {
        etag = clipbundle::etag(snapshot.items);
        token = token_for(snapshot.sequence);
      }
      reply(k_ok, state_reply(0, etag, token));
    });
#else
    (void) device;
    (void) body;
    reply(400, {});
#endif
  }
}  // namespace fork_tweaks::clipboard
