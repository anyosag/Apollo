/**
 * @file src/fork/debug_log.cpp
 * @brief Apollo's log for LuzLunar's Mac.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "debug_log.h"
#include "debug_log_policy.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"

using namespace std::literals;

namespace fork_tweaks::debug_log {
  namespace policy = debug_log_policy;

  namespace {
    using clock = std::chrono::steady_clock;

    std::mutex level_mutex;
    std::optional<int> changed_level;  ///< The level the Mac set, until it lapses
    clock::time_point changed_until;
    std::uint64_t generation = 0;  ///< Bumped by every change, so a lapse that was replaced does nothing

    reply_t refusal(int status, std::string_view error) {
      return {status, nlohmann::json {{"error", std::string {error}}}.dump()};
    }

    /// Logs the change where it shows: at the new level if that shows info, else at the old one
    void set_level(int level, const std::string &note) {
      if (level > 2) {
        BOOST_LOG(info) << note;
      }
      logging::set_min_log_level(level);
      if (level <= 2) {
        BOOST_LOG(info) << note;
      }
    }

    /// Back to the configured level; with level_mutex held
    void restore(std::string_view why) {
      if (!changed_level) {
        return;
      }
      changed_level.reset();
      generation++;
      set_level(config::sunshine.min_log_level, "fork: Log: back to the configured level ("s + std::string {policy::level_name(config::sunshine.min_log_level)} + ", " + std::string {why} + ')');
    }

    nlohmann::json state_json() {
      std::lock_guard lock(level_mutex);
      auto configured = config::sunshine.min_log_level;
      long long left = 0;
      if (changed_level) {
        left = std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(changed_until - clock::now()).count());
      }
      return {
        {"level", std::string {policy::level_name(changed_level.value_or(configured))}},
        {"configuredLevel", std::string {policy::level_name(configured)}},
        {"secondsLeft", left},
      };
    }

    std::string dump(const nlohmann::json &json) {
      // The log is in the PC's code page, which needn't be UTF-8
      return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    reply_t get(const query_t &query) {
      std::string error;
      auto request = policy::parse_get(query.lines, query.grep, query.from, error);
      if (!request) {
        return refusal(400, error);
      }

      logging::log_flush();
      std::ifstream file(config::sunshine.log_file, std::ios::binary | std::ios::ate);
      if (!file) {
        return refusal(500, "the log can't be opened");
      }
      auto size = static_cast<std::uint64_t>(std::max<std::streamoff>(file.tellg(), 0));
      auto start = policy::read_start(*request, size);
      std::string text(static_cast<std::size_t>(policy::read_length(*request, start, size)), '\0');
      file.seekg(static_cast<std::streamoff>(start));
      file.read(text.data(), static_cast<std::streamsize>(text.size()));
      text.resize(static_cast<std::size_t>(std::max<std::streamsize>(file.gcount(), 0)));

      auto picked = policy::pick(*request, text, start, size);
      auto out = state_json();
      out["size"] = size;
      out["next"] = picked.next;
      out["restarted"] = picked.restarted;
      out["lines"] = picked.lines;
      return {200, dump(out)};
    }

    reply_t post(const client_t &client, const std::string &text) {
      auto body = nlohmann::json::parse(text, nullptr, false);
      if (body.is_discarded() || !body.is_object()) {
        return refusal(400, "the body must be a JSON object");
      }
      if (body.contains("level") && !body["level"].is_string()) {
        return refusal(400, "level must be a name");
      }
      if (body.contains("mark") && !body["mark"].is_string()) {
        return refusal(400, "mark must be text");
      }
      if (!body.contains("level") && !body.contains("mark")) {
        return refusal(400, "give a level, a mark or both");
      }
      auto minutes = std::chrono::minutes(policy::default_duration);
      if (body.contains("minutes")) {
        auto most = std::chrono::duration_cast<std::chrono::minutes>(policy::max_duration).count();
        if (!body["minutes"].is_number_integer() || body["minutes"].get<long long>() < 1 || body["minutes"].get<long long>() > most) {
          return refusal(400, "minutes must be 1 to " + std::to_string(most));
        }
        minutes = std::chrono::minutes(body["minutes"].get<long long>());
      }

      if (body.contains("level")) {
        auto name = body["level"].get<std::string>();
        std::lock_guard lock(level_mutex);
        if (name == "configured") {
          restore("asked by ["s + client.name + ']');
        } else {
          auto level = policy::parse_level(name);
          if (!level) {
            return refusal(400, "unknown level: " + name);
          }
          changed_level = level;
          changed_until = clock::now() + minutes;
          auto mine = ++generation;
          set_level(*level, "fork: Log: ["s + client.name + "] set the level to " + name + " for " + std::to_string(minutes.count()) + " minutes");
          auto lapse = [mine]() {
            std::lock_guard lock(level_mutex);
            if (generation == mine) {
              restore("the time asked for is up");
            }
          };
          task_pool.pushDelayed(std::move(lapse), minutes);
        }
      }
      if (body.contains("mark")) {
        BOOST_LOG(info) << "fork: Log: mark from ["sv << client.name << "]: "sv << policy::clean_mark(body["mark"].get<std::string>());
      }
      return {200, dump(state_json())};
    }
  }  // namespace

  reply_t handle(const client_t &client, const std::string &method, const query_t &query, const std::string &body) {
    if (!client.permitted) {
      BOOST_LOG(debug) << "Permission Read Log denied for ["sv << client.name << ']';
      return refusal(401, "permission");
    }
    if (method == "POST") {
      return post(client, body);
    }
    return get(query);
  }
}  // namespace fork_tweaks::debug_log
