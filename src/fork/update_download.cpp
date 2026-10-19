/**
 * @file src/fork/update_download.cpp
 * @brief Downloads for over-the-air updates.
 */
// standard includes
#include <fstream>
#include <functional>

// lib includes
#include <curl/curl.h>

// local includes
#include "update_download.h"
#include "update_manifest.h"

namespace fork_update {
  namespace {
    constexpr int max_redirects = 5;

    struct sink_t {
      std::function<bool(const char *, std::size_t)> write;
      uint64_t written = 0;
      uint64_t max_bytes = 0;
      bool too_large = false;
      const std::atomic<bool> *cancel = nullptr;
    };

    int progress_callback(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
      auto *sink = static_cast<sink_t *>(user);
      return sink->cancel && sink->cancel->load() ? 1 : 0;  // Non-zero aborts the transfer
    }

    std::size_t write_callback(char *data, std::size_t size, std::size_t count, void *user) {
      auto *sink = static_cast<sink_t *>(user);
      std::size_t bytes = size * count;
      if (sink->written + bytes > sink->max_bytes) {
        sink->too_large = true;
        return 0;  // Makes curl stop with CURLE_WRITE_ERROR
      }
      if (!sink->write(data, bytes)) {
        return 0;
      }
      sink->written += bytes;
      return bytes;
    }

    /**
     * @brief Follow redirects by hand, so each address is checked against the allowed hosts before
     * anything is sent to it.
     * @param start_sink Called before each request, to empty what an earlier redirect's body wrote.
     */
    bool perform(const std::string &url, sink_t &sink, const std::function<bool()> &start_sink, long timeout_seconds, std::string *error) {
      auto fail = [error](std::string message) {
        if (error) {
          *error = std::move(message);
        }
        return false;
      };

      std::string current = url;
      for (int hop = 0; hop <= max_redirects; hop++) {
        if (!allowed_download_url(current)) {
          return fail("refused to download from " + current);
        }
        if (!start_sink()) {
          return fail("couldn't write the download");
        }
        sink.written = 0;
        sink.too_large = false;

        CURL *curl = curl_easy_init();
        if (!curl) {
          return fail("couldn't start curl");
        }
        curl_easy_setopt(curl, CURLOPT_URL, current.c_str());
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#ifdef _WIN32
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Apollo-updater");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
        // Give up on a transfer slower than 1 KB/s for a minute
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t) sink.max_bytes);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);

        CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        std::string location;
        if (char *redirect = nullptr; curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect) == CURLE_OK && redirect) {
          location = redirect;
        }
        curl_easy_cleanup(curl);

        if (sink.too_large) {
          return fail("the download is larger than expected");
        }
        if (result != CURLE_OK) {
          return fail(std::string {"download failed: "} + curl_easy_strerror(result));
        }
        if (status >= 300 && status < 400 && !location.empty()) {
          current = location;
          continue;
        }
        if (status != 200) {
          return fail("the server answered " + std::to_string(status) + " for " + current);
        }
        return true;
      }
      return fail("too many redirects");
    }
  }  // namespace

  std::optional<std::string> fetch(const std::string &url, std::size_t max_bytes, std::string *error, const std::atomic<bool> *cancel) {
    std::string body;
    sink_t sink;
    sink.max_bytes = max_bytes;
    sink.cancel = cancel;
    sink.write = [&body](const char *data, std::size_t size) {
      body.append(data, size);
      return true;
    };
    auto start = [&body]() {
      body.clear();
      return true;
    };
    if (!perform(url, sink, start, 60, error)) {
      return std::nullopt;
    }
    return body;
  }

  bool fetch_to_file(const std::string &url, const std::filesystem::path &file, uint64_t max_bytes, std::string *error, const std::atomic<bool> *cancel) {
    auto part = file;
    part += ".part";

    std::ofstream out;
    sink_t sink;
    sink.max_bytes = max_bytes;
    sink.cancel = cancel;
    sink.write = [&out](const char *data, std::size_t size) {
      out.write(data, (std::streamsize) size);
      return (bool) out;
    };
    auto start = [&out, &part]() {
      if (out.is_open()) {
        out.close();
      }
      out.open(part, std::ios::binary | std::ios::trunc);
      return out.is_open();
    };

    bool ok = perform(url, sink, start, 30 * 60, error);
    out.close();
    std::error_code ec;
    if (!ok) {
      std::filesystem::remove(part, ec);
      return false;
    }
    std::filesystem::rename(part, file, ec);
    if (ec) {
      std::filesystem::remove(part, ec);
      if (error) {
        *error = "couldn't save the download";
      }
      return false;
    }
    return true;
  }
}  // namespace fork_update
