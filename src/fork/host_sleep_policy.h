/**
 * @file src/fork/host_sleep_policy.h
 * @brief Putting this PC to sleep from LuzLunar, apart from any I/O: when it may sleep, the
 * single-use tokens a request needs, and the JSON LuzLunar gets.
 *
 * LuzLunar asks GET /actions/sleep, which says whether the PC can sleep now, names the app still
 * running if there is one, and hands out a token good for one POST within 30 seconds. POST
 * {"token": ..., "quitApp": true} then quits that app, as Quit does, and puts the PC to sleep.
 * Never while anything streams.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

namespace host_sleep_policy {
  using clock = std::chrono::steady_clock;

  /// How long a token from GET stays good
  constexpr auto token_life = std::chrono::seconds(30);

  /// After answering POST, how long before the PC sleeps, so the answer gets out first
  constexpr auto settle = std::chrono::milliseconds(1500);

  /// Tokens kept at once, one per client; the oldest goes first
  constexpr std::size_t max_tokens = 32;

  /// A token is 1 to this many characters
  constexpr std::size_t max_token = 64;

  /// What the PC is doing now
  struct facts_t {
    bool streaming = false;  ///< A stream runs, to any client
    bool installing = false;  ///< Apollo is installing an update and will restart
    bool sleeping = false;  ///< A request to sleep is already being carried out
    bool app_running = false;  ///< An app runs, kept for a resume say
    std::string app;  ///< Its name
  };

  /// Why the PC can't sleep now
  enum class reason_e {
    none,  ///< It can
    streaming,
    updating,
    sleeping,
    app_running,  ///< Only until the request says to quit the app
  };

  inline std::string_view key(reason_e reason) {
    switch (reason) {
      case reason_e::none:
        return "none";
      case reason_e::streaming:
        return "streaming";
      case reason_e::updating:
        return "updating";
      case reason_e::sleeping:
        return "sleeping";
      case reason_e::app_running:
        return "app_running";
    }
    return "none";
  }

  /**
   * @param quit_app The request says to quit a running app first.
   * @return Why the PC can't sleep now, or none.
   */
  inline reason_e blocker(const facts_t &facts, bool quit_app) {
    if (facts.streaming) {
      return reason_e::streaming;
    }
    if (facts.installing) {
      return reason_e::updating;
    }
    if (facts.sleeping) {
      return reason_e::sleeping;
    }
    if (facts.app_running && !quit_app) {
      return reason_e::app_running;
    }
    return reason_e::none;
  }

  /// Whether the PC can sleep now, quitting a running app if need be
  inline bool available(const facts_t &facts) {
    return blocker(facts, true) == reason_e::none;
  }

  /**
   * @brief GET's answer: {"available", "reason", "app", "token"}. The reason is "app_running"
   * when an app would be quit; the token is there only when the PC can sleep.
   */
  inline std::string status_json(const facts_t &facts, const std::string &token) {
    nlohmann::json json;
    json["available"] = available(facts);
    json["reason"] = std::string {key(blocker(facts, false))};
    json["app"] = facts.app_running ? facts.app : std::string {};
    if (available(facts) && !token.empty()) {
      json["token"] = token;
    }
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }

  /// POST's answer once the PC is about to sleep
  inline std::string accepted_json(bool quit_app) {
    return nlohmann::json {{"sleeping", true}, {"quitApp", quit_app}}.dump();
  }

  /// A refusal: {"error", "app"}, with the app when it's the reason
  inline std::string refusal_json(std::string_view error, const std::string &app = {}) {
    nlohmann::json json {{"error", std::string {error}}};
    if (!app.empty()) {
      json["app"] = app;
    }
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }

  /// What POST asks for
  struct request_t {
    std::string token;
    bool quit_app = false;
  };

  /**
   * @brief Reads POST /actions/sleep's body: {"token": ..., "quitApp": true|false}.
   * @param error Set to what's wrong when it isn't valid.
   */
  inline std::optional<request_t> parse_request(std::string_view body, std::string &error) {
    auto json = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      error = "the body isn't a JSON object";
      return std::nullopt;
    }
    auto token = json.find("token");
    if (token == json.end() || !token->is_string() || token->get_ref<const std::string &>().empty() ||
        token->get_ref<const std::string &>().size() > max_token) {
      error = "invalid token";
      return std::nullopt;
    }
    request_t request;
    request.token = token->get<std::string>();
    if (auto quit = json.find("quitApp"); quit != json.end()) {
      if (!quit->is_boolean()) {
        error = "invalid quitApp";
        return std::nullopt;
      }
      request.quit_app = quit->get<bool>();
    }
    return request;
  }

  /// The tokens GET hands out: one per client, used up by the next POST, right or wrong
  class tokens_t {
  public:
    /// Remembers a new token for the client, replacing its previous one
    void issue(const std::string &client, std::string token, clock::time_point now) {
      prune(now);
      entries_[client] = entry_t {std::move(token), now};
      while (entries_.size() > max_tokens) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
          if (it->second.issued < oldest->second.issued) {
            oldest = it;
          }
        }
        entries_.erase(oldest);
      }
    }

    /// Whether the client's token is this one and still good; it's used up either way
    bool redeem(const std::string &client, std::string_view token, clock::time_point now) {
      auto it = entries_.find(client);
      if (it == entries_.end()) {
        return false;
      }
      auto good = !token.empty() && it->second.token == token && now - it->second.issued < token_life;
      entries_.erase(it);
      return good;
    }

    std::size_t size() const {
      return entries_.size();
    }

  private:
    struct entry_t {
      std::string token;
      clock::time_point issued;
    };

    void prune(clock::time_point now) {
      for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.issued >= token_life) {
          it = entries_.erase(it);
        } else {
          ++it;
        }
      }
    }

    std::map<std::string, entry_t> entries_;
  };
}  // namespace host_sleep_policy
