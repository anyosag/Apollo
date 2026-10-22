/**
 * @file src/fork/mac_drive_policy.h
 * @brief LuzLunar's Mac as a network drive on this PC, apart from any I/O: what a request may ask
 * for, where the drive points and which letter it takes, what Windows' answers mean, when the drive
 * goes, and the lines Apollo and its helper (tools/apollo-mac-drive.cpp) exchange.
 *
 * On a LAN stream, LuzLunar posts the Mac's account and the shares to try, its whole disk first and
 * its home folder after, to /actions/drive (mac_drive.h). The helper, running as the signed-in
 * user, maps the first of them that the Mac offers, always at the address the request came from and
 * never at one a request names. The drive goes a minute after the client's last stream ends, once
 * nothing has files open on it, and at the latest 3 minutes after the stream ended.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace mac_drive_policy {
  using clock = std::chrono::steady_clock;

  /// Shares a request may list, tried in order
  constexpr std::size_t max_shares = 4;

  /// macOS account short names are at most 255 characters
  constexpr std::size_t max_user = 255;

  /// In bytes; Windows' credential prompts take 256 characters
  constexpr std::size_t max_password = 255;

  /// In bytes; Windows takes share names of up to 80 characters
  constexpr std::size_t max_share = 80;

  /// In bytes: the Mac's name, which Explorer shows for the drive
  constexpr std::size_t max_label = 100;

  /// After the client's last stream ended, how long the drive stays for a resume
  constexpr auto grace = std::chrono::seconds(60);

  /// While files are open on the drive, how often removing it is tried again
  constexpr auto retry = std::chrono::seconds(30);

  /// After the client's last stream ended, the drive goes even with files open on it
  constexpr auto force_after = std::chrono::minutes(3);

  /// What LuzLunar asks for: POST /actions/drive {"action": "map", ...}
  struct map_request_t {
    std::vector<std::string> shares;  ///< SMB share names on the Mac, in the order to try them
    std::string user;  ///< The Mac account's short name
    std::string password;  ///< Its password; never logged
    std::string label;  ///< The Mac's name, for Explorer
  };

  /// Overwrites a string that held a password before it goes
  inline void wipe(std::string &text) {
    std::fill(text.begin(), text.end(), '\0');
    text.clear();
  }

  namespace detail {
    inline bool has_control(std::string_view text) {
      return std::any_of(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7F;
      });
    }

    inline const nlohmann::json *member(const nlohmann::json &object, const char *key) {
      auto it = object.find(key);
      return it == object.end() ? nullptr : &*it;
    }

    inline const std::string *string_member(const nlohmann::json &object, const char *key) {
      auto value = member(object, key);
      return value && value->is_string() ? &value->get_ref<const std::string &>() : nullptr;
    }

    /// The password inside a parsed or built object, before the object goes
    inline void wipe_password(nlohmann::json &object) {
      auto it = object.find("password");
      if (it != object.end() && it->is_string()) {
        wipe(it->get_ref<std::string &>());
      }
    }

    inline std::string dump(const nlohmann::json &json) {
      // Every string came from parsed JSON or was checked, so it's UTF-8; replace keeps dump() from throwing anyway
      return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    inline bool hex_digit(char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    /// Four decimal parts of 0 to 255, without leading zeros
    inline bool ipv4(std::string_view text) {
      int parts = 0;
      std::size_t i = 0;
      while (true) {
        auto start = i;
        unsigned value = 0;
        while (i < text.size() && i - start < 3 && text[i] >= '0' && text[i] <= '9') {
          value = value * 10 + (text[i] - '0');
          i++;
        }
        if (i == start || value > 255 || (i - start > 1 && text[start] == '0')) {
          return false;
        }
        parts++;
        if (i == text.size()) {
          return parts == 4;
        }
        if (text[i] != '.' || parts == 4) {
          return false;
        }
        i++;
      }
    }

    /// Groups of hex digits separated by ':', in part of an IPv6 address; counts them into groups
    inline bool ipv6_groups(std::string_view part, int &groups) {
      if (part.empty()) {
        return true;
      }
      std::size_t start = 0;
      while (true) {
        auto end = part.find(':', start);
        auto group = part.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (group.empty() || group.size() > 4 || !std::all_of(group.begin(), group.end(), hex_digit)) {
          return false;
        }
        groups++;
        if (end == std::string_view::npos) {
          return true;
        }
        start = end + 1;
      }
    }

    /// An IPv6 address in hex groups with at most one "::", without a zone or an IPv4 part
    inline bool ipv6(std::string_view text) {
      int groups = 0;
      auto gap = text.find("::");
      if (gap == std::string_view::npos) {
        return ipv6_groups(text, groups) && groups == 8;
      }
      if (text.find("::", gap + 1) != std::string_view::npos) {
        return false;
      }
      return ipv6_groups(text.substr(0, gap), groups) && ipv6_groups(text.substr(gap + 2), groups) && groups < 8;
    }

    /// fe80::/10: the first group is FE8x to FEBx
    inline bool ipv6_link_local(std::string_view text) {
      auto first = text.substr(0, text.find(':'));
      return first.size() == 4 && (first[0] == 'f' || first[0] == 'F') && (first[1] == 'e' || first[1] == 'E') &&
             std::string_view {"89abAB"}.find(first[2]) != std::string_view::npos;
    }
  }  // namespace detail

  /// A macOS account short name: letters, digits, '.', '_' and '-', not starting with '.' or '-'
  inline bool valid_user(std::string_view user) {
    if (user.empty() || user.size() > max_user || user.front() == '.' || user.front() == '-') {
      return false;
    }
    return std::all_of(user.begin(), user.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
  }

  /// A share name Windows can map: none of the characters UNC paths reserve, not hidden ("C$"), not
  /// "." or "..", and no spaces at its ends
  inline bool valid_share(std::string_view share) {
    if (share.empty() || share.size() > max_share || share == "." || share == ".." || share.back() == '$' ||
        share.front() == ' ' || share.back() == ' ' || detail::has_control(share)) {
      return false;
    }
    return share.find_first_of("\\/:*?\"<>|") == std::string_view::npos;
  }

  inline bool valid_password(std::string_view password) {
    return !password.empty() && password.size() <= max_password && !detail::has_control(password);
  }

  inline bool valid_label(std::string_view label) {
    return label.size() <= max_label && !detail::has_control(label);
  }

  /// The host part of a UNC path Apollo builds: digits, letters, '.' and '-' (unc_host())
  inline bool valid_host(std::string_view host) {
    return !host.empty() && host.size() <= 128 && std::all_of(host.begin(), host.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    });
  }

  /**
   * @brief Reads POST /actions/drive's body: {"action": "map", "shares": [...], "user": ...,
   * "password": ..., "name": ...}. Without a name, Explorer calls the drive "Mac".
   * @param error Set to what's wrong when it isn't valid.
   */
  inline std::optional<map_request_t> parse_map_request(std::string_view body, std::string &error) {
    auto json = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      error = "the body isn't a JSON object";
      return std::nullopt;
    }

    auto result = [&]() -> std::optional<map_request_t> {
      auto action = detail::string_member(json, "action");
      if (!action || *action != "map") {
        error = "unknown action";
        return std::nullopt;
      }

      map_request_t request;
      auto shares = detail::member(json, "shares");
      if (!shares || !shares->is_array() || shares->empty() || shares->size() > max_shares) {
        error = "shares must list 1 to 4 share names";
        return std::nullopt;
      }
      for (const auto &share : *shares) {
        if (!share.is_string() || !valid_share(share.get_ref<const std::string &>())) {
          error = "invalid share name";
          return std::nullopt;
        }
        request.shares.push_back(share.get<std::string>());
      }

      auto user = detail::string_member(json, "user");
      if (!user || !valid_user(*user)) {
        error = "invalid user name";
        return std::nullopt;
      }
      request.user = *user;

      // Always required: an empty one would have Windows offer the PC user's own credentials instead
      auto password = detail::string_member(json, "password");
      if (!password || !valid_password(*password)) {
        error = "invalid password";
        return std::nullopt;
      }
      request.password = *password;

      request.label = "Mac";
      if (auto name = detail::member(json, "name")) {
        if (!name->is_string() || !valid_label(name->get_ref<const std::string &>())) {
          wipe(request.password);
          error = "invalid name";
          return std::nullopt;
        }
        if (!name->get_ref<const std::string &>().empty()) {
          request.label = name->get<std::string>();
        }
      }
      return request;
    }();
    detail::wipe_password(json);
    return result;
  }

  /**
   * @brief The host part of the drive's UNC path, for the client's address as Apollo normalizes it:
   * an IPv4 address as it is, an IPv6 one in Windows' ipv6-literal.net form, with '-' for ':' and its
   * zone after 's' ("fe80::1%4" is "fe80--1s4.ipv6-literal.net").
   * @return Empty for anything else, and for a link-local IPv6 address without a numeric zone, which
   * the PC couldn't reach.
   */
  inline std::string unc_host(std::string_view address) {
    if (detail::ipv4(address)) {
      return std::string {address};
    }

    std::string_view zone;
    if (auto percent = address.find('%'); percent != std::string_view::npos) {
      zone = address.substr(percent + 1);
      address = address.substr(0, percent);
      if (zone.empty() || zone.size() > 10 || zone.find_first_not_of("0123456789") != std::string_view::npos) {
        return {};
      }
    }
    if (!detail::ipv6(address) || (zone.empty() && detail::ipv6_link_local(address))) {
      return {};
    }

    // A name can't start or end with '-', so "::1" is written "0::1", the same address
    std::string host;
    if (address.substr(0, 2) == "::") {
      host += '0';
    }
    for (auto c : address) {
      host += c == ':' ? '-' : c;
    }
    if (address.size() >= 2 && address.substr(address.size() - 2) == "::") {
      host += '0';
    }
    if (!zone.empty()) {
      host += 's';
      host += zone;
    }
    return host + ".ipv6-literal.net";
  }

  /// \\host\share
  inline std::string unc_path(std::string_view host, std::string_view share) {
    return "\\\\" + std::string {host} + '\\' + std::string {share};
  }

  /// Explorer's key for a network drive's label under HKCU\...\Explorer\MountPoints2: the UNC path
  /// with '#' for each '\' ("##192.168.1.23#Macintosh HD")
  inline std::string label_key(std::string_view host, std::string_view share) {
    return "##" + std::string {host} + '#' + std::string {share};
  }

  /**
   * @brief The letter for a new drive: @p preferred if it's free, else the highest free one from Z
   * down to D, as `net use *` picks.
   * @param used The letters in use, bit 0 for A, as GetLogicalDrives() reports them.
   * @return 0 when D to Z are all taken.
   */
  inline char choose_letter(std::uint32_t used, char preferred = 'M') {
    auto taken = [used](char letter) {
      return ((used >> (letter - 'A')) & 1u) != 0;
    };
    if (preferred >= 'D' && preferred <= 'Z' && !taken(preferred)) {
      return preferred;
    }
    for (char letter = 'Z'; letter >= 'D'; letter--) {
      if (!taken(letter)) {
        return letter;
      }
    }
    return 0;
  }

  /// What a Windows error from mapping or removing the drive comes to
  enum class failure_e {
    none,
    logon,  ///< The Mac refused the account or its password
    ntlm_blocked,  ///< Windows' SMB NTLM blocking is on, and a Mac signs in with NTLM
    denied,  ///< The account may not open that share
    no_share,  ///< The Mac has no share by that name
    unreachable,  ///< The Mac's File Sharing didn't answer
    conflict,  ///< The user is connected to this Mac under another account already
    letter_taken,  ///< The letter is in use, or remembered for another drive
    no_letter,  ///< No letter is free
    in_use,  ///< Files are open on the drive
    gone,  ///< The drive was removed already
    other,
  };

  inline failure_e classify(unsigned long error) {
    switch (error) {
      case 0:
        return failure_e::none;
      case 86:  // ERROR_INVALID_PASSWORD
      case 1326:  // ERROR_LOGON_FAILURE
      case 1327:  // ERROR_ACCOUNT_RESTRICTION
      case 1330:  // ERROR_PASSWORD_EXPIRED
      case 1331:  // ERROR_ACCOUNT_DISABLED
      case 1909:  // ERROR_ACCOUNT_LOCKED_OUT
        return failure_e::logon;
      case 1937:  // ERROR_NTLM_BLOCKED
        return failure_e::ntlm_blocked;
      case 5:  // ERROR_ACCESS_DENIED
        return failure_e::denied;
      case 67:  // ERROR_BAD_NET_NAME
        return failure_e::no_share;
      case 53:  // ERROR_BAD_NETPATH
      case 59:  // ERROR_UNEXP_NET_ERR
      case 64:  // ERROR_NETNAME_DELETED
      case 121:  // ERROR_SEM_TIMEOUT
      case 1203:  // ERROR_NO_NET_OR_BAD_PATH
      case 1222:  // ERROR_NO_NETWORK
      case 1231:  // ERROR_NETWORK_UNREACHABLE
      case 1232:  // ERROR_HOST_UNREACHABLE
        return failure_e::unreachable;
      case 1219:  // ERROR_SESSION_CREDENTIAL_CONFLICT
        return failure_e::conflict;
      case 85:  // ERROR_ALREADY_ASSIGNED
      case 1202:  // ERROR_DEVICE_ALREADY_REMEMBERED
        return failure_e::letter_taken;
      case 1248:  // ERROR_NO_MORE_DEVICES
        return failure_e::no_letter;
      case 2401:  // ERROR_OPEN_FILES
      case 2404:  // ERROR_DEVICE_IN_USE
        return failure_e::in_use;
      case 2250:  // ERROR_NOT_CONNECTED
        return failure_e::gone;
      default:
        return failure_e::other;
    }
  }

  /// The name LuzLunar gets for a failure in the drive's state
  inline std::string_view key(failure_e failure) {
    switch (failure) {
      case failure_e::none:
        return "none";
      case failure_e::logon:
        return "logon";
      case failure_e::ntlm_blocked:
        return "ntlm_blocked";
      case failure_e::denied:
        return "denied";
      case failure_e::no_share:
        return "no_share";
      case failure_e::unreachable:
        return "unreachable";
      case failure_e::conflict:
        return "conflict";
      case failure_e::letter_taken:
        return "letter_taken";
      case failure_e::no_letter:
        return "no_letter";
      case failure_e::in_use:
        return "in_use";
      case failure_e::gone:
        return "gone";
      case failure_e::other:
        break;
    }
    return "other";
  }

  /// For the log: what a Windows error from mapping or removing the drive means, with its number
  inline std::string describe(unsigned long error) {
    std::string_view what;
    switch (classify(error)) {
      case failure_e::none:
        what = "done";
        break;
      case failure_e::logon:
        what = "the Mac refused the account or its password";
        break;
      case failure_e::ntlm_blocked:
        what = "Windows blocks NTLM sign-ins, which a Mac needs (SMB NTLM blocking)";
        break;
      case failure_e::denied:
        what = "the account may not open that share";
        break;
      case failure_e::no_share:
        what = "the Mac has no share by that name";
        break;
      case failure_e::unreachable:
        what = "the Mac's File Sharing didn't answer";
        break;
      case failure_e::conflict:
        what = "Windows is connected to this Mac under another account already";
        break;
      case failure_e::letter_taken:
        what = "the drive letter is taken";
        break;
      case failure_e::no_letter:
        what = "no drive letter is free";
        break;
      case failure_e::in_use:
        what = "files are open on the drive";
        break;
      case failure_e::gone:
        what = "the drive was gone already";
        break;
      case failure_e::other:
        what = "Windows refused";
        break;
    }
    return std::string {what} + " (error " + std::to_string(error) + ')';
  }

  /// What to do with a client's drive now
  enum class removal_e {
    keep,  ///< Leave it
    unmap,  ///< Remove it unless files are open on it
    unmap_force,  ///< Remove it, closing what's open on it
  };

  /**
   * @param since_end How long since the client's last stream ended; nullopt while it streams.
   * @param since_try How long since a removal last found files open; nullopt if none did yet.
   */
  inline removal_e removal(std::optional<clock::duration> since_end, std::optional<clock::duration> since_try) {
    if (!since_end || *since_end < grace) {
      return removal_e::keep;
    }
    if (*since_end >= force_after) {
      return removal_e::unmap_force;
    }
    if (since_try && *since_try < retry) {
      return removal_e::keep;
    }
    return removal_e::unmap;
  }

  /// Apollo to the helper, one JSON object per line, so nothing in a password can end a line
  struct command_t {
    enum class kind_e {
      bad,
      map,
      unmap,
    };

    kind_e kind = kind_e::bad;
    char letter = 0;  ///< map: the letter to prefer
    std::string host;  ///< map: unc_host() of the client's address
    map_request_t request;  ///< map
    bool force = false;  ///< unmap: even with files open
  };

  /// The map command; the caller wipes it once it's written
  inline std::string map_command(char letter, std::string_view host, const map_request_t &request) {
    nlohmann::json json {
      {"cmd", "map"},
      {"letter", std::string(1, letter)},
      {"host", std::string {host}},
      {"shares", request.shares},
      {"user", request.user},
      {"password", request.password},
      {"label", request.label},
    };
    auto line = detail::dump(json);
    detail::wipe_password(json);
    return line;
  }

  inline std::string unmap_command(bool force) {
    return force ? R"({"cmd":"unmap","force":true})" : R"({"cmd":"unmap"})";
  }

  /// One line from Apollo, as the helper reads it; kind bad for anything it doesn't take
  inline command_t parse_command(std::string_view line) {
    command_t command;
    auto json = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return command;
    }

    auto cmd = detail::string_member(json, "cmd");
    if (cmd && *cmd == "unmap") {
      auto force = detail::member(json, "force");
      command.force = force && force->is_boolean() && force->get<bool>();
      command.kind = command_t::kind_e::unmap;
      return command;
    }

    if (cmd && *cmd == "map") {
      [&]() {
        auto letter = detail::string_member(json, "letter");
        auto host = detail::string_member(json, "host");
        auto shares = detail::member(json, "shares");
        auto user = detail::string_member(json, "user");
        auto password = detail::string_member(json, "password");
        auto label = detail::string_member(json, "label");
        if (!letter || letter->size() != 1 || (*letter)[0] < 'D' || (*letter)[0] > 'Z' || !host || !valid_host(*host) ||
            !shares || !shares->is_array() || shares->empty() || shares->size() > max_shares || !user || !valid_user(*user) ||
            !password || !valid_password(*password) || !label || !valid_label(*label)) {
          return;
        }
        for (const auto &share : *shares) {
          if (!share.is_string() || !valid_share(share.get_ref<const std::string &>())) {
            command.request.shares.clear();
            return;
          }
          command.request.shares.push_back(share.get<std::string>());
        }
        command.letter = (*letter)[0];
        command.host = *host;
        command.request.user = *user;
        command.request.password = *password;
        command.request.label = *label;
        command.kind = command_t::kind_e::map;
      }();
    }
    detail::wipe_password(json);
    return command;
  }

  /// The helper's answers, one JSON object per line among its log lines ("<level>: <message>")
  struct result_t {
    enum class kind_e {
      none,  ///< Not an answer: a log line
      mapped,
      failed,
      in_use,
      unmapped,
    };

    kind_e kind = kind_e::none;
    char letter = 0;  ///< mapped
    std::string share;  ///< mapped: the share it is; failed: the last share tried
    unsigned long code = 0;  ///< failed and in_use: Windows' error
  };

  inline std::string mapped_result(char letter, std::string_view share) {
    return detail::dump({{"result", "mapped"}, {"letter", std::string(1, letter)}, {"share", std::string {share}}});
  }

  inline std::string failed_result(unsigned long code, std::string_view share) {
    return detail::dump({{"result", "failed"}, {"code", code}, {"share", std::string {share}}});
  }

  inline std::string in_use_result(unsigned long code) {
    return detail::dump({{"result", "in-use"}, {"code", code}});
  }

  inline std::string unmapped_result() {
    return R"({"result":"unmapped"})";
  }

  /// One of the helper's output lines, as Apollo reads it
  inline result_t parse_result(std::string_view line) {
    result_t result;
    if (line.empty() || line.front() != '{') {
      return result;
    }
    auto json = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return result;
    }
    auto kind = detail::string_member(json, "result");
    if (!kind) {
      return result;
    }
    auto share = detail::string_member(json, "share");
    auto code = detail::member(json, "code");
    if (code && code->is_number_unsigned()) {
      result.code = code->get<unsigned long>();
    }
    if (*kind == "mapped") {
      auto letter = detail::string_member(json, "letter");
      if (letter && letter->size() == 1 && (*letter)[0] >= 'D' && (*letter)[0] <= 'Z' && share) {
        result.kind = result_t::kind_e::mapped;
        result.letter = (*letter)[0];
        result.share = *share;
      }
    } else if (*kind == "failed") {
      result.kind = result_t::kind_e::failed;
      if (share) {
        result.share = *share;
      }
    } else if (*kind == "in-use") {
      result.kind = result_t::kind_e::in_use;
    } else if (*kind == "unmapped") {
      result.kind = result_t::kind_e::unmapped;
    }
    return result;
  }

  /// A client's drive, as GET and POST /actions/drive report it
  enum class state_e {
    none,  ///< No drive and no request
    mapping,  ///< The helper is mapping it
    mapped,
    failed,
  };

  struct status_t {
    state_e state = state_e::none;
    char letter = 0;  ///< mapped
    std::string share;  ///< mapped: the share it is
    bool added = false;  ///< mapped for this request, not kept from an earlier stream
    std::string error;  ///< failed: key() of Windows' failure, or "no_user" or "helper"
    unsigned long code = 0;  ///< failed: Windows' error, when it was Windows'
  };

  inline std::string status_json(const status_t &status) {
    nlohmann::json json;
    switch (status.state) {
      case state_e::none:
        json["state"] = "none";
        break;
      case state_e::mapping:
        json["state"] = "mapping";
        break;
      case state_e::mapped:
        json["state"] = "mapped";
        json["letter"] = std::string(1, status.letter);
        json["share"] = status.share;
        json["added"] = status.added;
        break;
      case state_e::failed:
        json["state"] = "failed";
        json["error"] = status.error;
        if (status.code) {
          json["code"] = status.code;
        }
        break;
    }
    return detail::dump(json);
  }
}  // namespace mac_drive_policy
