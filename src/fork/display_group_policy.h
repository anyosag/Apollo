/**
 * @file src/fork/display_group_policy.h
 * @brief The "Desktop (multiple displays)" app for LuzLunar, apart from any I/O: the group LuzLunar
 * declares, where its displays go on the Windows desktop, which slot a joining stream takes, and the
 * JSON of /actions/displaygroup and /actions/pairdevice.
 *
 * LuzLunar streams one Windows display per Mac display. Slot 0 (the Mac's main display) launches
 * or resumes the app with `luzGroup=<base64url JSON>`, which declares every slot:
 *
 *   {"v":1,"id":"<16 hex>","slots":[
 *    {"slot":0,"x":0,"y":0},
 *    {"slot":1,"w":1920,"h":1080,"fps":60,"x":3024,"y":442,"external":true,"hdr":false,
 *     "screen":{"mm":"476x268","panel":"1920x1080","safe":"1920x1080","hz":"60","gamut":"srgb",
 *               "hdr":"none","name":"DisplayExtend (iMac)"}}]}
 *
 * Slot 0's own mode and screen come from the launch's `mode` and `luz*` arguments, as for any app.
 * The other slots join with `luzGroupId=<id>&luzSlot=k`; a client that sends neither (stock
 * Moonlight on the iMac) takes the first free `external` slot.
 *
 * Header-only, without Boost or Windows, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "client_display.h"

namespace display_group_policy {
  /// LuzVDA has at most this many displays (LUZVDA_MAX_DISPLAYS), slot 0's included
  constexpr int max_slots = 16;

  /// A declaration longer than this, in base64url, is refused
  constexpr std::size_t max_declaration = 16384;

  /// Windows keeps the desktop within these coordinates
  constexpr int max_coordinate = 32767;

  /// A display slower than this many hertz is refused
  constexpr double max_fps = client_display::max_hz;

  /// One slot of the declaration
  struct slot_t {
    int slot = 0;
    int x = 0;
    int y = 0;
    /// The stream's size and rate; 0 for slot 0, whose mode comes from the launch
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_mhz = 0;
    /// The slot a client without luz* arguments may take (the iMac's stock Moonlight)
    bool external = false;
    /// The display runs HDR
    bool hdr = false;
    /// The Mac display it streams to, as luz* arguments (mm, panel, safe, hz, gamut, hdr, name)
    std::map<std::string, std::string> screen_args;
    /// The same, checked; none when the declaration had no usable screen
    std::optional<client_display::screen_t> screen;

    /// Everything but the parsed screen, which comes from screen_args
    bool same(const slot_t &o) const {
      return slot == o.slot && x == o.x && y == o.y && width == o.width && height == o.height && fps_mhz == o.fps_mhz &&
             external == o.external && hdr == o.hdr && screen_args == o.screen_args;
    }
  };

  /// What slot 0 declared
  struct declaration_t {
    std::string id;  ///< 16 lowercase hex digits
    std::vector<slot_t> slots;  ///< By slot number; slot 0 first

    const slot_t *find(int slot) const {
      for (const auto &s : slots) {
        if (s.slot == slot) {
          return &s;
        }
      }
      return nullptr;
    }
  };

  /// The same displays in the same places; the id may differ
  inline bool same_layout(const declaration_t &a, const declaration_t &b) {
    if (a.slots.size() != b.slots.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.slots.size(); ++i) {
      if (!a.slots[i].same(b.slots[i])) {
        return false;
      }
    }
    return true;
  }

  /// base64url, with or without padding (standard base64's + and / are taken too)
  inline std::optional<std::string> base64url_decode(std::string_view in) {
    auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') {
        return c - 'A';
      }
      if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
      }
      if (c >= '0' && c <= '9') {
        return c - '0' + 52;
      }
      if (c == '-' || c == '+') {
        return 62;
      }
      if (c == '_' || c == '/') {
        return 63;
      }
      return -1;
    };

    while (!in.empty() && in.back() == '=') {
      in.remove_suffix(1);
    }
    if (in.size() % 4 == 1) {
      return std::nullopt;
    }

    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : in) {
      int v = value(c);
      if (v < 0) {
        return std::nullopt;
      }
      buffer = (buffer << 6) | (uint32_t) v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back((char) ((buffer >> bits) & 0xFF));
      }
    }
    return out;
  }

  /// "0123456789abcdef", lowercased; empty if it isn't 16 hex digits
  inline std::string normalize_id(std::string_view id) {
    if (id.size() != 16) {
      return {};
    }
    std::string out;
    for (char c : id) {
      if (c >= '0' && c <= '9') {
        out.push_back(c);
      } else if (c >= 'a' && c <= 'f') {
        out.push_back(c);
      } else if (c >= 'A' && c <= 'F') {
        out.push_back((char) (c - 'A' + 'a'));
      } else {
        return {};
      }
    }
    return out;
  }

  namespace detail {
    inline std::optional<int> integer(const nlohmann::json &json, const char *key, int fallback, bool required = false) {
      auto it = json.find(key);
      if (it == json.end()) {
        return required ? std::nullopt : std::optional<int> {fallback};
      }
      if (!it->is_number_integer()) {
        return std::nullopt;
      }
      auto v = it->get<int64_t>();
      if (v < -max_coordinate || v > max_coordinate) {
        return std::nullopt;
      }
      return (int) v;
    }

    inline std::optional<bool> boolean(const nlohmann::json &json, const char *key) {
      auto it = json.find(key);
      if (it == json.end()) {
        return false;
      }
      if (!it->is_boolean()) {
        return std::nullopt;
      }
      return it->get<bool>();
    }
  }  // namespace detail

  /**
   * @brief Reads `luzGroup`.
   * @param error Set to what's wrong when it isn't valid, for the log.
   */
  inline std::optional<declaration_t> parse(std::string_view encoded, std::string &error) {
    if (encoded.empty() || encoded.size() > max_declaration) {
      error = "the declaration is empty or too long";
      return std::nullopt;
    }
    auto text = base64url_decode(encoded);
    if (!text) {
      error = "the declaration isn't base64url";
      return std::nullopt;
    }
    auto json = nlohmann::json::parse(*text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      error = "the declaration isn't a JSON object";
      return std::nullopt;
    }

    auto v = json.find("v");
    if (v == json.end() || !v->is_number_integer() || v->get<int64_t>() != 1) {
      error = "unknown declaration version";
      return std::nullopt;
    }

    declaration_t out;
    auto id = json.find("id");
    out.id = id != json.end() && id->is_string() ? normalize_id(id->get_ref<const std::string &>()) : std::string {};
    if (out.id.empty()) {
      error = "the id isn't 16 hex digits";
      return std::nullopt;
    }

    auto slots = json.find("slots");
    if (slots == json.end() || !slots->is_array() || slots->empty() || slots->size() > (std::size_t) max_slots) {
      error = "slots must be an array of 1 to " + std::to_string(max_slots);
      return std::nullopt;
    }

    for (const auto &node : *slots) {
      if (!node.is_object()) {
        error = "a slot isn't an object";
        return std::nullopt;
      }
      slot_t s;
      auto number = detail::integer(node, "slot", 0, true);
      if (!number || *number < 0 || *number >= max_slots) {
        error = "a slot number is missing or out of range";
        return std::nullopt;
      }
      s.slot = *number;
      if (out.find(s.slot)) {
        error = "slot " + std::to_string(s.slot) + " is declared twice";
        return std::nullopt;
      }

      auto x = detail::integer(node, "x", 0);
      auto y = detail::integer(node, "y", 0);
      if (!x || !y) {
        error = "slot " + std::to_string(s.slot) + " has a bad position";
        return std::nullopt;
      }
      s.x = *x;
      s.y = *y;

      if (s.slot == 0) {
        // Slot 0 is the primary display, which Windows puts at the origin
        if (s.x != 0 || s.y != 0) {
          error = "slot 0 must be at 0,0";
          return std::nullopt;
        }
        out.slots.push_back(std::move(s));
        continue;
      }

      auto w = detail::integer(node, "w", 0, true);
      auto h = detail::integer(node, "h", 0, true);
      if (!w || !h || *w <= 0 || *h <= 0 || !client_display::dimension_ok((uint32_t) *w, (uint32_t) *h)) {
        error = "slot " + std::to_string(s.slot) + " has a bad size";
        return std::nullopt;
      }
      s.width = (uint32_t) *w;
      s.height = (uint32_t) *h;
      if ((int64_t) s.x + s.width > max_coordinate || (int64_t) s.y + s.height > max_coordinate) {
        error = "slot " + std::to_string(s.slot) + " lies off the desktop";
        return std::nullopt;
      }

      double fps = 60;
      if (auto it = node.find("fps"); it != node.end()) {
        if (!it->is_number()) {
          error = "slot " + std::to_string(s.slot) + " has a bad fps";
          return std::nullopt;
        }
        fps = it->get<double>();
      }
      // Hertz, or millihertz as Moonlight's mode argument may have it
      if (fps >= 1000) {
        fps /= 1000;
      }
      if (!(fps >= 1 && fps <= max_fps)) {
        error = "slot " + std::to_string(s.slot) + " has a bad fps";
        return std::nullopt;
      }
      s.fps_mhz = (uint32_t) std::lround(fps * 1000);

      auto external = detail::boolean(node, "external");
      auto hdr = detail::boolean(node, "hdr");
      if (!external || !hdr) {
        error = "slot " + std::to_string(s.slot) + " has a bad external or hdr";
        return std::nullopt;
      }
      s.external = *external;
      s.hdr = *hdr;

      if (auto screen = node.find("screen"); screen != node.end()) {
        if (!screen->is_object()) {
          error = "slot " + std::to_string(s.slot) + " has a bad screen";
          return std::nullopt;
        }
        // The screen's fields are LuzLunar's luz* launch arguments without the prefix
        for (auto key : {"mm", "panel", "safe", "hz", "gamut", "hdr", "name"}) {
          auto it = screen->find(key);
          if (it == screen->end()) {
            continue;
          }
          if (it->is_string()) {
            s.screen_args[key] = it->get<std::string>();
          } else if (it->is_number_unsigned()) {
            s.screen_args[key] = std::to_string(it->get<uint64_t>());
          } else {
            error = "slot " + std::to_string(s.slot) + "'s screen has a bad " + key;
            return std::nullopt;
          }
        }
        s.screen = client_display::parse([&](std::string_view key) -> std::optional<std::string> {
          if (key == "luzDisplay") {
            return std::string {"1"};
          }
          if (key.substr(0, 3) != "luz" || key.size() < 4) {
            return std::nullopt;
          }
          std::string name {key.substr(3)};
          name[0] = (char) std::tolower((unsigned char) name[0]);
          auto it = s.screen_args.find(name);
          if (it == s.screen_args.end()) {
            return std::nullopt;
          }
          return it->second;
        });
      }

      out.slots.push_back(std::move(s));
    }

    if (!out.find(0)) {
      error = "slot 0 isn't declared";
      return std::nullopt;
    }
    std::sort(out.slots.begin(), out.slots.end(), [](const slot_t &a, const slot_t &b) {
      return a.slot < b.slot;
    });
    return out;
  }

  /// A display on the Windows desktop, in pixels
  struct rect_t {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const rect_t &) const = default;
  };

  inline bool overlaps(const rect_t &a, const rect_t &b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
  }

  /// Sharing part of an edge; corners alone don't count
  inline bool touches(const rect_t &a, const rect_t &b) {
    bool side_by_side = (a.x + a.width == b.x || b.x + b.width == a.x) && a.y < b.y + b.height && b.y < a.y + a.height;
    bool stacked = (a.y + a.height == b.y || b.y + b.height == a.y) && a.x < b.x + b.width && b.x < a.x + a.width;
    return side_by_side || stacked;
  }

  /// The first display at the origin, none overlapping another, and each reachable from the first
  /// through displays that touch
  inline bool valid_layout(const std::vector<rect_t> &displays) {
    if (displays.empty()) {
      return false;
    }
    if (displays[0].x != 0 || displays[0].y != 0) {
      return false;
    }
    for (std::size_t i = 0; i < displays.size(); ++i) {
      if (displays[i].width <= 0 || displays[i].height <= 0) {
        return false;
      }
      for (std::size_t j = i + 1; j < displays.size(); ++j) {
        if (overlaps(displays[i], displays[j])) {
          return false;
        }
      }
    }

    std::vector<bool> reached(displays.size(), false);
    std::vector<std::size_t> todo {0};
    reached[0] = true;
    while (!todo.empty()) {
      auto i = todo.back();
      todo.pop_back();
      for (std::size_t j = 0; j < displays.size(); ++j) {
        if (!reached[j] && touches(displays[i], displays[j])) {
          reached[j] = true;
          todo.push_back(j);
        }
      }
    }
    return std::all_of(reached.begin(), reached.end(), [](bool r) {
      return r;
    });
  }

  /// The displays in a row from the origin, tops aligned, in the order given
  inline std::vector<rect_t> row_layout(std::vector<rect_t> displays) {
    int x = 0;
    for (auto &d : displays) {
      d.x = x;
      d.y = 0;
      x += d.width;
    }
    return displays;
  }

  /**
   * @brief Where the displays go: as declared when that's a valid desktop (valid_layout()), else in
   * a row. The first is slot 0, the primary display.
   * @param declared Set to whether the declared places were kept.
   */
  inline std::vector<rect_t> arrange(const std::vector<rect_t> &displays, bool &declared) {
    declared = valid_layout(displays);
    return declared ? displays : row_layout(displays);
  }

  /// Slot k's display identity: slot 0's with k mixed into its last byte, so each slot keeps its own
  /// place and scale in Windows' settings
  inline std::array<uint8_t, 16> slot_guid(std::array<uint8_t, 16> base, int slot) {
    base[15] ^= (uint8_t) slot;
    return base;
  }

  /// What a launch or resume says about the group
  struct request_t {
    enum class kind_e {
      none,  ///< No luz group arguments
      lead,  ///< Slot 0, with the declaration (luzGroup)
      join,  ///< Another slot (luzGroupId, luzSlot)
    } kind = kind_e::none;

    std::optional<declaration_t> declaration;  ///< For lead; none when it didn't parse
    std::string error;  ///< Why it didn't
    std::string id;  ///< For join; empty when it isn't 16 hex digits
    int slot = -1;  ///< For join; -1 when it isn't a number

    bool bad() const {
      return kind == kind_e::lead && !declaration;
    }
  };

  /// A launch argument, if the client sent it
  using lookup_t = std::function<std::optional<std::string>(std::string_view)>;

  inline request_t parse_request(const lookup_t &arg) {
    request_t out;
    if (auto group = arg("luzGroup")) {
      out.kind = request_t::kind_e::lead;
      out.declaration = parse(*group, out.error);
      return out;
    }
    if (auto id = arg("luzGroupId")) {
      out.kind = request_t::kind_e::join;
      out.id = normalize_id(*id);
      if (auto slot = arg("luzSlot")) {
        if (auto value = client_display::parse_uint(*slot); value && *value < (uint32_t) max_slots) {
          out.slot = (int) *value;
        }
      }
    }
    return out;
  }

  /// A slot of the running group, k >= 1
  struct slot_state_t {
    int slot = 0;
    bool external = false;
    std::string claimed_by;  ///< The device streaming it, by its uuid; empty while free
  };

  /// The running group, for deciding who may join
  struct group_view_t {
    std::string id;
    std::vector<slot_state_t> slots;  ///< Slots 1 and up, by number
  };

  /// Which slot a joining client takes, or why it can't
  struct join_t {
    int status = 200;  ///< 200, 409 (the slot is taken or unknown) or 410 (no such group)
    int slot = -1;
    std::string why;  ///< For the log and the status message
  };

  /**
   * @brief The slot a join takes. A slot the same device holds already is taken over: a stream that
   * dropped may come back before its old session has timed out.
   * @param group The running group, or none.
   * @param device The joining client's uuid.
   */
  inline join_t check_join(const group_view_t *group, const request_t &request, std::string_view device) {
    using kind_e = request_t::kind_e;
    if (request.kind == kind_e::join) {
      if (!group || request.id.empty() || request.id != group->id) {
        return {410, -1, "there's no such display group"};
      }
      for (const auto &s : group->slots) {
        if (s.slot != request.slot) {
          continue;
        }
        if (!s.claimed_by.empty() && s.claimed_by != device) {
          return {409, s.slot, "slot " + std::to_string(s.slot) + " is taken"};
        }
        return {200, s.slot, {}};
      }
      return {409, request.slot, "the group has no slot " + std::to_string(request.slot)};
    }

    if (request.kind == kind_e::none && group) {
      // A group without other slots is a single display, resumed as any app is
      if (group->slots.empty()) {
        return {200, 0, {}};
      }
      for (const auto &s : group->slots) {
        if (s.external && s.claimed_by == device) {
          return {200, s.slot, {}};
        }
      }
      for (const auto &s : group->slots) {
        if (s.external && s.claimed_by.empty()) {
          return {200, s.slot, {}};
        }
      }
      return {409, -1, "no external slot is free"};
    }

    return {200, 0, {}};
  }

  /// What slot 0's launch or resume does to the group already running
  enum class lead_e {
    adopt,  ///< The same layout: the group takes the new id
    rebuild,  ///< Another layout, and nobody streams: the slots' displays are made again
    conflict,  ///< Another layout while someone streams: 409
  };

  /**
   * @param running The running group's declaration; none for a group launched without one.
   */
  inline lead_e lead_action(const declaration_t *running, const declaration_t &incoming, bool streaming) {
    if (running && same_layout(*running, incoming)) {
      return lead_e::adopt;
    }
    return streaming ? lead_e::conflict : lead_e::rebuild;
  }

  /// A slot in GET /actions/displaygroup
  struct slot_status_t {
    int slot = 0;
    std::string display;  ///< Its GDI name, e.g. \\.\DISPLAY7
    uint32_t width = 0;
    uint32_t height = 0;
    int x = 0;
    int y = 0;
    bool external = false;
    bool claimed = false;
    std::string client;  ///< The device streaming it
  };

  /// {"active", "id", "slots": [{"slot", "display", "w", "h", "x", "y", "external", "claimed", "client"}]}
  inline std::string status_json(bool active, const std::string &id, const std::vector<slot_status_t> &slots) {
    nlohmann::json json;
    json["active"] = active;
    json["id"] = active ? id : std::string {};
    json["slots"] = nlohmann::json::array();
    for (const auto &s : slots) {
      json["slots"].push_back({
        {"slot", s.slot},
        {"display", s.display},
        {"w", s.width},
        {"h", s.height},
        {"x", s.x},
        {"y", s.y},
        {"external", s.external},
        {"claimed", s.claimed},
        {"client", s.client},
      });
    }
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }

  /// POST /actions/pairdevice: the PIN for the device waiting at an address
  struct pair_request_t {
    std::string pin;
    std::string name;
    std::string address;
  };

  /// A device name is at most this many bytes
  constexpr std::size_t max_pair_name = 63;

  /**
   * @brief Reads {"pin": "4821", "name": "iMac (DisplayExtend)", "address": "192.168.1.31"}.
   * @param error Set to what's wrong when it isn't valid.
   */
  inline std::optional<pair_request_t> parse_pair_request(std::string_view body, std::string &error) {
    auto json = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      error = "the body isn't a JSON object";
      return std::nullopt;
    }
    pair_request_t out;
    auto pin = json.find("pin");
    if (pin == json.end() || !pin->is_string()) {
      error = "the pin must be 4 digits";
      return std::nullopt;
    }
    out.pin = pin->get<std::string>();
    if (out.pin.size() != 4 || !std::all_of(out.pin.begin(), out.pin.end(), [](char c) {
          return c >= '0' && c <= '9';
        })) {
      error = "the pin must be 4 digits";
      return std::nullopt;
    }
    if (auto name = json.find("name"); name != json.end()) {
      if (!name->is_string()) {
        error = "invalid name";
        return std::nullopt;
      }
      out.name = client_display::truncate_utf8(name->get<std::string>(), max_pair_name);
    }
    auto address = json.find("address");
    if (address == json.end() || !address->is_string() || address->get_ref<const std::string &>().empty() ||
        address->get_ref<const std::string &>().size() > 64) {
      error = "invalid address";
      return std::nullopt;
    }
    out.address = address->get<std::string>();
    return out;
  }

  /// Which of the devices waiting for a PIN is at the address
  struct waiting_match_t {
    std::optional<std::size_t> index;  ///< The one there, if exactly one is
    std::size_t count = 0;  ///< How many are there
  };

  inline waiting_match_t waiting_at(const std::vector<std::string> &waiting, std::string_view address) {
    waiting_match_t out;
    for (std::size_t i = 0; i < waiting.size(); ++i) {
      if (waiting[i] == address) {
        out.count++;
        out.index = i;
      }
    }
    if (out.count != 1) {
      out.index.reset();
    }
    return out;
  }

  /// The 409 when no device, or more than one, waits at the address: {"error", "waiting": [addresses]}
  inline std::string not_waiting_json(const std::vector<std::string> &waiting, std::size_t at_address) {
    nlohmann::json json;
    json["error"] = at_address > 1 ? "several devices are waiting at that address" : "no device is waiting";
    json["waiting"] = waiting;
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }
}  // namespace display_group_policy
