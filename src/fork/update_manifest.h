/**
 * @file src/fork/update_manifest.h
 * @brief The rules of Apollo's over-the-air updates, apart from any I/O: what a valid manifest is,
 * whether it's an update, when it may install, and the records Apollo and its install helper
 * (tools/apollo-updater.cpp) leave each other.
 *
 * A manifest is the update.json CI publishes with each master build (FORK.md, "Updates"):
 *   {"product": "apollo", "channel": "master", "run": 123, "version": "0.4.8.110",
 *    "commit": "<40 hex>", "published": "2026-10-18T20:00:00Z", "notes": "<commit subject>",
 *    "asset": {"name": "Apollo-windows-installer.exe", "url": "https://github.com/...",
 *              "sha256": "<64 hex>", "size": 12345678}}
 * Its signature is checked before it's parsed (update_verify.h).
 *
 * Header-only, without Boost, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

namespace fork_update {
  /// The largest installer an update may download
  inline constexpr uint64_t max_asset_size = 300ull * 1024 * 1024;

  /// The largest manifest Apollo reads
  inline constexpr std::size_t max_manifest_size = 64 * 1024;

  /// The build that's running, from the CI run that built it
  struct build_t {
    int64_t run = 0;  ///< The workflow run number; 0 for a local build
    std::string channel;  ///< "master" for a build of a push to master, anything else for a branch or local build
    std::string version;
    std::string commit;

    /// Whether this build came from master, so automatic updates may replace it
    bool is_master() const {
      return channel == "master" && run > 0;
    }
  };

  /// The file a manifest offers
  struct asset_t {
    std::string name;
    std::string url;
    std::string sha256;  ///< Lowercase hex
    uint64_t size = 0;
  };

  /// A checked update.json
  struct manifest_t {
    std::string product;
    std::string channel;
    int64_t run = 0;
    std::string version;
    std::string commit;
    std::string published;
    std::string notes;
    asset_t asset;
  };

  namespace detail {
    inline bool is_lower_hex(std::string_view s, std::size_t length) {
      if (s.size() != length) {
        return false;
      }
      for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
          return false;
        }
      }
      return true;
    }

    /// A file name with no path in it: letters, digits, '.', '_' and '-', not starting with '.'
    inline bool is_plain_file_name(std::string_view s) {
      if (s.empty() || s.size() > 128 || s.front() == '.') {
        return false;
      }
      for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-')) {
          return false;
        }
      }
      return true;
    }

    /// Printable ASCII, so a version or date can't carry control characters into logs or the UI
    inline bool is_printable(std::string_view s) {
      for (char c : s) {
        if (c < 0x20 || c > 0x7e) {
          return false;
        }
      }
      return true;
    }

    inline bool fail(std::string *error, const char *message) {
      if (error) {
        *error = message;
      }
      return false;
    }

    inline bool get_string(const nlohmann::json &object, const char *key, std::string &out, std::size_t max_size) {
      auto it = object.find(key);
      if (it == object.end() || !it->is_string()) {
        return false;
      }
      out = it->get<std::string>();
      return out.size() <= max_size;
    }

    inline bool get_integer(const nlohmann::json &object, const char *key, int64_t &out) {
      auto it = object.find(key);
      if (it == object.end() || !it->is_number_integer()) {
        return false;
      }
      out = it->get<int64_t>();
      return true;
    }
  }  // namespace detail

  /**
   * @brief Parse and check a manifest whose signature already checked out.
   * @param text The manifest's bytes.
   * @param product The product it must be for.
   * @param url_prefix The address its asset must be under.
   * @param error Set to what's wrong when it's rejected.
   * @return The manifest, or nothing if anything about it is wrong.
   */
  inline std::optional<manifest_t> parse_manifest(std::string_view text, std::string_view product, std::string_view url_prefix, std::string *error = nullptr) {
    if (text.size() > max_manifest_size) {
      detail::fail(error, "the manifest is too large");
      return std::nullopt;
    }

    auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      detail::fail(error, "the manifest isn't a JSON object");
      return std::nullopt;
    }

    manifest_t m;
    if (!detail::get_string(json, "product", m.product, 32) || m.product != product) {
      detail::fail(error, "the manifest is for another product");
      return std::nullopt;
    }
    if (!detail::get_string(json, "channel", m.channel, 32) || m.channel != "master") {
      detail::fail(error, "the manifest isn't for a master build");
      return std::nullopt;
    }
    if (!detail::get_integer(json, "run", m.run) || m.run <= 0) {
      detail::fail(error, "the manifest has no run number");
      return std::nullopt;
    }
    if (!detail::get_string(json, "version", m.version, 64) || m.version.empty() || !detail::is_printable(m.version)) {
      detail::fail(error, "the manifest has no valid version");
      return std::nullopt;
    }
    if (!detail::get_string(json, "commit", m.commit, 40) || !detail::is_lower_hex(m.commit, 40)) {
      detail::fail(error, "the manifest has no valid commit");
      return std::nullopt;
    }
    if (!detail::get_string(json, "published", m.published, 64) || !detail::is_printable(m.published)) {
      detail::fail(error, "the manifest has no valid date");
      return std::nullopt;
    }
    // Notes may hold any text, but not much of it
    if (!detail::get_string(json, "notes", m.notes, 4000)) {
      detail::fail(error, "the manifest has no valid notes");
      return std::nullopt;
    }

    auto asset = json.find("asset");
    if (asset == json.end() || !asset->is_object()) {
      detail::fail(error, "the manifest has no asset");
      return std::nullopt;
    }
    if (!detail::get_string(*asset, "name", m.asset.name, 128) || !detail::is_plain_file_name(m.asset.name)) {
      detail::fail(error, "the asset's name isn't a plain file name");
      return std::nullopt;
    }
    if (!detail::get_string(*asset, "url", m.asset.url, 512) || !detail::is_printable(m.asset.url)) {
      detail::fail(error, "the asset has no valid address");
      return std::nullopt;
    }
    // <prefix><tag>/<name>, where the tag is a plain name too
    std::string_view url = m.asset.url;
    if (url.substr(0, url_prefix.size()) != url_prefix) {
      detail::fail(error, "the asset isn't from this repository's releases");
      return std::nullopt;
    }
    auto rest = url.substr(url_prefix.size());
    auto slash = rest.find('/');
    if (slash == std::string_view::npos || !detail::is_plain_file_name(rest.substr(0, slash)) || rest.substr(slash + 1) != m.asset.name) {
      detail::fail(error, "the asset's address doesn't match its name");
      return std::nullopt;
    }
    if (!detail::get_string(*asset, "sha256", m.asset.sha256, 64) || !detail::is_lower_hex(m.asset.sha256, 64)) {
      detail::fail(error, "the asset has no valid SHA-256");
      return std::nullopt;
    }
    int64_t size = 0;
    if (!detail::get_integer(*asset, "size", size) || size <= 0 || (uint64_t) size > max_asset_size) {
      detail::fail(error, "the asset's size is missing or too large");
      return std::nullopt;
    }
    m.asset.size = (uint64_t) size;

    return m;
  }

  /**
   * @brief Whether a manifest offers something other than what's running: a newer master build, or
   * any master build when a branch or local build runs.
   */
  inline bool is_update(const manifest_t &m, const build_t &running) {
    return !running.is_master() || m.run > running.run;
  }

  /**
   * @brief Whether an update may install without anyone asking: only a newer master build over a
   * master build, so a branch build installed on purpose stays until someone clicks Install.
   */
  inline bool may_install_automatically(const manifest_t &m, const build_t &running, bool automatic) {
    return automatic && running.is_master() && m.run > running.run;
  }

  /**
   * @brief Whether a download may go to, or be redirected to, this address: HTTPS to GitHub or the
   * hosts GitHub serves release assets from.
   */
  inline bool allowed_download_url(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme) {
      return false;
    }
    auto rest = url.substr(scheme.size());
    auto end = rest.find_first_of("/?#");
    auto host = rest.substr(0, end);
    // No user info or port, so "github.com@elsewhere" or "github.com:1234" can't get through
    if (host.empty() || host.find_first_of("@:") != std::string_view::npos) {
      return false;
    }
    constexpr std::string_view assets = ".githubusercontent.com";
    return host == "github.com" || (host.size() > assets.size() && host.substr(host.size() - assets.size()) == assets);
  }

  /// A build's own release asset, e.g. the installer of the build that runs, to roll back to
  inline std::string release_asset_url(std::string_view url_prefix, std::string_view tag_prefix, int64_t run, std::string_view name) {
    return std::string {url_prefix} + std::string {tag_prefix} + std::to_string(run) + "/" + std::string {name};
  }

  /// What the updater is doing
  enum class state_e {
    idle,  ///< Up to date, or not checked yet
    checking,
    downloading,
    ready,  ///< An update is downloaded and checked, waiting to install
    waiting,  ///< Asked to install, waiting until nobody streams
    installing,  ///< The helper is installing; Apollo is about to stop
    failed,  ///< The last check or download failed; it retries at the next check
  };

  inline const char *to_string(state_e state) {
    switch (state) {
      case state_e::idle:
        return "idle";
      case state_e::checking:
        return "checking";
      case state_e::downloading:
        return "downloading";
      case state_e::ready:
        return "ready";
      case state_e::waiting:
        return "waiting";
      case state_e::installing:
        return "installing";
      case state_e::failed:
        return "failed";
    }
    return "idle";
  }

  /// What to do with a downloaded update
  enum class install_decision_e {
    none,  ///< Keep it until someone asks, or automatic updates are on
    install,
    wait,  ///< Install once nobody streams
  };

  /**
   * @brief Whether to install a downloaded update now.
   * @param ready An update is downloaded and checked.
   * @param requested Someone asked to install it (the Web UI or LuzLunar).
   * @param automatic It may install without anyone asking (may_install_automatically()).
   * @param streaming A client is streaming.
   * @param app_running An app is running, with or without a client.
   *
   * An install never ends a stream. Asked for, it ends a running app nobody streams; automatically,
   * it waits until no app runs either.
   */
  inline install_decision_e decide_install(bool ready, bool requested, bool automatic, bool streaming, bool app_running) {
    if (!ready) {
      return install_decision_e::none;
    }
    if (requested) {
      return streaming ? install_decision_e::wait : install_decision_e::install;
    }
    if (automatic) {
      return streaming || app_running ? install_decision_e::wait : install_decision_e::install;
    }
    return install_decision_e::none;
  }

  /// What each Apollo writes when it starts (updates\started.json), so the helper knows the new build is up
  struct started_t {
    int64_t run = 0;
    uint32_t pid = 0;
    int port = 0;  ///< The HTTP port, to check it answers
    std::string version;
  };

  inline std::string to_json(const started_t &s) {
    return nlohmann::json {{"run", s.run}, {"pid", s.pid}, {"port", s.port}, {"version", s.version}}.dump();
  }

  inline std::optional<started_t> parse_started(std::string_view text) {
    auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return std::nullopt;
    }
    started_t s;
    int64_t pid = 0;
    int64_t port = 0;
    if (!detail::get_integer(json, "run", s.run) || !detail::get_integer(json, "pid", pid) || !detail::get_integer(json, "port", port) || !detail::get_string(json, "version", s.version, 64)) {
      return std::nullopt;
    }
    if (pid <= 0 || pid > UINT32_MAX || port <= 0 || port > 65535) {
      return std::nullopt;
    }
    s.pid = (uint32_t) pid;
    s.port = (int) port;
    return s;
  }

  /// How an install ended
  enum class outcome_e {
    updated,
    rolled_back,  ///< The new build didn't come up, so the previous one was put back
    failed,  ///< Neither came up; the helper started the service as a last resort
  };

  /// What the helper writes when it's done (updates\result.json)
  struct result_t {
    int64_t target_run = 0;
    std::string target_version;
    outcome_e outcome = outcome_e::failed;
    std::string running_version;  ///< What runs afterwards, if anything does
    std::string message;
    int64_t time = 0;  ///< Seconds since 1970
  };

  inline const char *to_string(outcome_e outcome) {
    switch (outcome) {
      case outcome_e::updated:
        return "updated";
      case outcome_e::rolled_back:
        return "rolled_back";
      case outcome_e::failed:
        return "failed";
    }
    return "failed";
  }

  inline std::string to_json(const result_t &r) {
    return nlohmann::json {
      {"target_run", r.target_run},
      {"target_version", r.target_version},
      {"outcome", to_string(r.outcome)},
      {"running_version", r.running_version},
      {"message", r.message},
      {"time", r.time},
    }
      .dump();
  }

  inline std::optional<result_t> parse_result(std::string_view text) {
    auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return std::nullopt;
    }
    result_t r;
    std::string outcome;
    if (!detail::get_integer(json, "target_run", r.target_run) || !detail::get_string(json, "target_version", r.target_version, 64) || !detail::get_string(json, "outcome", outcome, 16) || !detail::get_string(json, "running_version", r.running_version, 64) || !detail::get_string(json, "message", r.message, 1000) || !detail::get_integer(json, "time", r.time)) {
      return std::nullopt;
    }
    if (outcome == "updated") {
      r.outcome = outcome_e::updated;
    } else if (outcome == "rolled_back") {
      r.outcome = outcome_e::rolled_back;
    } else if (outcome == "failed") {
      r.outcome = outcome_e::failed;
    } else {
      return std::nullopt;
    }
    return r;
  }

  /// One line for the Web UI and LuzLunar about how the last install ended
  inline std::string describe(const result_t &r) {
    switch (r.outcome) {
      case outcome_e::updated:
        return "Updated to " + r.target_version;
      case outcome_e::rolled_back:
        return "The update to " + r.target_version + " didn't start, so Apollo went back to " + r.running_version + (r.message.empty() ? "" : ": " + r.message);
      case outcome_e::failed:
        return "The update to " + r.target_version + " failed" + (r.message.empty() ? "" : ": " + r.message);
    }
    return {};
  }
}  // namespace fork_update
