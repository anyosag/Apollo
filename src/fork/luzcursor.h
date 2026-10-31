/**
 * @file luzcursor.h
 * @brief The PC's pointer drawn by the Mac: the messages anyosag/Apollo sends anyosag/LuzLunar.
 *
 * Shared, byte for byte, by anyosag/Apollo (src/fork/luzcursor.h) and anyosag/LuzLunar
 * (app/fork/luzcursor.h). Keep both copies identical; tests/fork/luzcursor_test.cpp checks the
 * same behavior in both repos' CI. Header-only C++17 with no dependencies.
 *
 * A client that draws the PC's pointer itself launches with luzCursor=1, on a host whose
 * /serverinfo says <LuzCursor> k_serverinfo_version or more. The host leaves the pointer out of
 * the picture while every stream of that display draws it, and sends two control messages,
 * encrypted and reliable, on ENet channel k_channel, so they arrive in the order they were sent.
 * Their types are in the range the fork keeps for its own host → client messages (k_type_first
 * to k_type_last); moonlight-common-c hands them to LuzLunar with its patch 0004.
 *
 * SHAPE (k_type_shape), the pointer's image in chunks of at most k_max_chunk bytes. A 28-byte
 * header, little-endian:
 *   u8 version (1), u8 flags (0), u16 reserved (0), u64 the shape's hash, u16 width, u16 height,
 *   u16 hotspot x, u16 hotspot y, u32 the image's size (data_size()), u32 this chunk's offset in it,
 * then the chunk. The image is width x height straight-alpha BGRA pixels, rows from the top, then
 * the invert mask: a bit per pixel in rows of mask_stride() bytes, the leftmost pixel in a byte's
 * top bit. A pixel whose bit is set inverts what's under it, as Windows' I-beam does; its BGRA is 0.
 *
 * STATE (k_type_state), where the pointer is. 60 bytes, little-endian:
 *   u8 version (1), u8 flags (state_flag), u16 reserved (0), u64 the shape's hash (0 for none),
 *   i32 x, i32 y: the hotspot from the display's top-left, in the display's pixels,
 *   i32 width, i32 height: the display's size in pixels,
 *   i64 sum_dx, i64 sum_dy: the relative moves the host has injected for this stream, added up,
 *   i32 left, top, right, bottom: where the host keeps the pointer, from the display's top-left
 *   (right and bottom exclusive): the rectangle an app holds it in (flag_clipped), else the bounds
 *   of all its displays, which may reach past this one.
 * Sums survive both sides merging moves, so a client that adds up what it sends knows which of its
 * moves the position already has (Mac trackpad mode, where the client predicts the pointer).
 *
 * The cache: both sides keep the same list of the k_cache_size most recently used hashes, empty
 * at each stream's start. A SHAPE's last chunk inserts its hash as the most recent, pushing out
 * the least recent when full, and a STATE with a hash makes it the most recent. The host sends a
 * shape only when its own copy of the list lacks the hash (sender_t), so the client has the shape
 * every STATE names without acknowledging anything; after one turn, each frame of the busy
 * spinner costs one STATE. The client inserts a shape it can't use too (receiver_t), so the two
 * lists never differ.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace luzcursor {

  constexpr std::uint32_t k_host_feature_flag = 0x10000000;  ///< In the host's x-ss-general.featureFlags
  constexpr int k_serverinfo_version = 2;  ///< /serverinfo's <LuzCursor>; 1 only answers GET /actions/cursor
  constexpr std::uint16_t k_type_first = 0x4C00;  ///< The fork's host → client control messages
  constexpr std::uint16_t k_type_last = 0x4CFF;
  constexpr std::uint16_t k_type_shape = 0x4C01;
  constexpr std::uint16_t k_type_state = 0x4C02;
  constexpr std::uint8_t k_channel = 0x07;  ///< Its own ENet channel, so big shapes hold up nothing else
  constexpr std::uint8_t k_version = 1;
  constexpr std::size_t k_shape_header_size = 28;
  constexpr std::size_t k_state_size = 60;
  constexpr std::size_t k_max_chunk = 60000;  ///< A control message's length is 16 bits
  constexpr std::size_t k_cache_size = 64;
  constexpr int k_max_side = 512;  ///< Windows' largest pointer is 256 pixels across

  enum state_flag : std::uint8_t {
    flag_visible = 1,  ///< The pointer shows: no app hid it, and neither did the host (Ctrl+Alt+Shift+N)
    flag_on_display = 2,  ///< It's on the display the stream shows
    flag_host_draws = 4,  ///< The host draws it into the picture (a secure desktop, Windows Graphics Capture, another stream of the display): the client draws none
    flag_clipped = 8,  ///< An app holds it in the clip rectangle (ClipCursor)
  };

  struct state_t {
    std::uint8_t flags = flag_host_draws;
    std::uint64_t hash = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int64_t sum_dx = 0;
    std::int64_t sum_dy = 0;
    std::int32_t clip_left = 0;
    std::int32_t clip_top = 0;
    std::int32_t clip_right = 0;
    std::int32_t clip_bottom = 0;

    bool visible() const {
      return (flags & flag_visible) != 0;
    }

    bool on_display() const {
      return (flags & flag_on_display) != 0;
    }

    bool host_draws() const {
      return (flags & flag_host_draws) != 0;
    }

    bool clipped() const {
      return (flags & flag_clipped) != 0;
    }

    /// The client draws the pointer now: it shows, on the display, in a shape, and the host doesn't
    bool client_draws() const {
      return visible() && on_display() && !host_draws() && hash != 0 && width > 0 && height > 0;
    }

    bool operator==(const state_t &other) const {
      return flags == other.flags && hash == other.hash && x == other.x && y == other.y && width == other.width &&
             height == other.height && sum_dx == other.sum_dx && sum_dy == other.sum_dy && clip_left == other.clip_left &&
             clip_top == other.clip_top && clip_right == other.clip_right && clip_bottom == other.clip_bottom;
    }

    bool operator!=(const state_t &other) const {
      return !(*this == other);
    }
  };

  /// A pointer image
  struct shape_t {
    int width = 0;
    int height = 0;
    int hotspot_x = 0;
    int hotspot_y = 0;
    std::vector<std::uint8_t> bgra;  ///< width x height straight-alpha BGRA pixels
    std::vector<std::uint8_t> invert;  ///< mask_stride(width) x height bytes
    std::uint64_t hash = 0;  ///< hash_of() the rest; never 0
  };

  /// One control message's type and payload (what follows the type)
  struct message_t {
    std::uint16_t type = 0;
    std::string payload;
  };

  inline std::size_t mask_stride(int width) {
    return (static_cast<std::size_t>(width) + 7) / 8;
  }

  inline std::size_t data_size(int width, int height) {
    return static_cast<std::size_t>(width) * height * 4 + mask_stride(width) * height;
  }

  inline bool valid_size(int width, int height) {
    return width >= 1 && height >= 1 && width <= k_max_side && height <= k_max_side;
  }

  /// A shape whose fields agree with each other
  inline bool well_formed(const shape_t &shape) {
    return valid_size(shape.width, shape.height) && shape.hotspot_x >= 0 && shape.hotspot_y >= 0 &&
           shape.hotspot_x < shape.width && shape.hotspot_y < shape.height &&
           shape.bgra.size() == static_cast<std::size_t>(shape.width) * shape.height * 4 &&
           shape.invert.size() == mask_stride(shape.width) * shape.height;
  }

  /// An empty shape of that size: every pixel clear, none inverting
  inline shape_t blank_shape(int width, int height, int hotspot_x, int hotspot_y) {
    shape_t shape;
    shape.width = width;
    shape.height = height;
    shape.hotspot_x = hotspot_x;
    shape.hotspot_y = hotspot_y;
    shape.bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);
    shape.invert.assign(mask_stride(width) * height, 0);
    return shape;
  }

  inline bool inverts(const shape_t &shape, int x, int y) {
    std::size_t byte = static_cast<std::size_t>(y) * mask_stride(shape.width) + static_cast<std::size_t>(x) / 8;
    return (shape.invert[byte] & (0x80 >> (x % 8))) != 0;
  }

  /// Marks a pixel as inverting, with its BGRA cleared as the format has it
  inline void set_inverts(shape_t &shape, int x, int y) {
    std::size_t byte = static_cast<std::size_t>(y) * mask_stride(shape.width) + static_cast<std::size_t>(x) / 8;
    shape.invert[byte] = static_cast<std::uint8_t>(shape.invert[byte] | (0x80 >> (x % 8)));
    std::size_t pixel = (static_cast<std::size_t>(y) * shape.width + x) * 4;
    for (int i = 0; i < 4; ++i) {
      shape.bgra[pixel + i] = 0;
    }
  }

  /// 64-bit FNV-1a of the size, the hotspot, the pixels and the mask; never 0, which means no shape
  inline std::uint64_t hash_of(const shape_t &shape) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&hash](std::uint8_t byte) {
      hash ^= byte;
      hash *= 0x100000001b3ull;
    };
    for (int value : {shape.width, shape.height, shape.hotspot_x, shape.hotspot_y}) {
      mix(static_cast<std::uint8_t>(value & 0xFF));
      mix(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    }
    for (std::uint8_t byte : shape.bgra) {
      mix(byte);
    }
    for (std::uint8_t byte : shape.invert) {
      mix(byte);
    }
    return hash != 0 ? hash : 1;
  }

  namespace detail {
    inline void put(std::string &out, std::uint64_t value, int bytes) {
      for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
      }
    }

    inline std::uint64_t get(const std::uint8_t *data, int bytes) {
      std::uint64_t value = 0;
      for (int i = 0; i < bytes; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
      }
      return value;
    }

    inline std::int32_t get_i32(const std::uint8_t *data) {
      return static_cast<std::int32_t>(static_cast<std::uint32_t>(get(data, 4)));
    }

    inline std::int64_t get_i64(const std::uint8_t *data) {
      return static_cast<std::int64_t>(get(data, 8));
    }
  }  // namespace detail

  inline std::string state_message(const state_t &state) {
    std::string out;
    out.reserve(k_state_size);
    detail::put(out, k_version, 1);
    detail::put(out, state.flags, 1);
    detail::put(out, 0, 2);
    detail::put(out, state.hash, 8);
    for (std::int32_t value : {state.x, state.y, state.width, state.height}) {
      detail::put(out, static_cast<std::uint32_t>(value), 4);
    }
    detail::put(out, static_cast<std::uint64_t>(state.sum_dx), 8);
    detail::put(out, static_cast<std::uint64_t>(state.sum_dy), 8);
    for (std::int32_t value : {state.clip_left, state.clip_top, state.clip_right, state.clip_bottom}) {
      detail::put(out, static_cast<std::uint32_t>(value), 4);
    }
    return out;
  }

  /// A STATE's payload; none for another version or a short one. A later version that only adds
  /// fields keeps version 1 and puts them past k_state_size, where this leaves them.
  inline std::optional<state_t> parse_state(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size < k_state_size || data[0] != k_version) {
      return std::nullopt;
    }
    state_t state;
    state.flags = data[1];
    state.hash = detail::get(data + 4, 8);
    state.x = detail::get_i32(data + 12);
    state.y = detail::get_i32(data + 16);
    state.width = detail::get_i32(data + 20);
    state.height = detail::get_i32(data + 24);
    state.sum_dx = detail::get_i64(data + 28);
    state.sum_dy = detail::get_i64(data + 36);
    state.clip_left = detail::get_i32(data + 44);
    state.clip_top = detail::get_i32(data + 48);
    state.clip_right = detail::get_i32(data + 52);
    state.clip_bottom = detail::get_i32(data + 56);
    return state;
  }

  /// The SHAPE messages' payloads for a well-formed shape whose hash is set, in order
  inline std::vector<std::string> shape_messages(const shape_t &shape, std::size_t max_chunk = k_max_chunk) {
    std::vector<std::string> messages;
    if (!well_formed(shape) || shape.hash == 0 || max_chunk == 0) {
      return messages;
    }
    std::size_t total = data_size(shape.width, shape.height);
    std::size_t pixels = shape.bgra.size();
    for (std::size_t offset = 0; offset < total; offset += max_chunk) {
      std::size_t length = total - offset < max_chunk ? total - offset : max_chunk;
      std::string message;
      message.reserve(k_shape_header_size + length);
      detail::put(message, k_version, 1);
      detail::put(message, 0, 1);
      detail::put(message, 0, 2);
      detail::put(message, shape.hash, 8);
      for (int value : {shape.width, shape.height, shape.hotspot_x, shape.hotspot_y}) {
        detail::put(message, static_cast<std::uint16_t>(value), 2);
      }
      detail::put(message, static_cast<std::uint32_t>(total), 4);
      detail::put(message, static_cast<std::uint32_t>(offset), 4);
      for (std::size_t i = offset; i < offset + length; ++i) {
        message.push_back(static_cast<char>(i < pixels ? shape.bgra[i] : shape.invert[i - pixels]));
      }
      messages.push_back(std::move(message));
    }
    return messages;
  }

  /// The most recently used hashes, as both sides keep them
  class mru_t {
  public:
    bool contains(std::uint64_t hash) const {
      return find(hash) < m_count;
    }

    /// Makes it the most recent; false when it isn't in the list
    bool touch(std::uint64_t hash) {
      std::size_t at = find(hash);
      if (at >= m_count) {
        return false;
      }
      move_to_front(at);
      return true;
    }

    /// Adds it as the most recent (or makes it so); the hash pushed out, or 0 for none
    std::uint64_t insert(std::uint64_t hash) {
      if (touch(hash)) {
        return 0;
      }
      std::uint64_t evicted = 0;
      if (m_count == k_cache_size) {
        evicted = m_hashes[m_count - 1];
        --m_count;
      }
      for (std::size_t i = m_count; i > 0; --i) {
        m_hashes[i] = m_hashes[i - 1];
      }
      m_hashes[0] = hash;
      ++m_count;
      return evicted;
    }

    std::size_t size() const {
      return m_count;
    }

    /// From the most recent
    std::uint64_t at(std::size_t index) const {
      return index < m_count ? m_hashes[index] : 0;
    }

    void clear() {
      m_count = 0;
    }

  private:
    std::size_t find(std::uint64_t hash) const {
      for (std::size_t i = 0; i < m_count; ++i) {
        if (m_hashes[i] == hash) {
          return i;
        }
      }
      return m_count;
    }

    void move_to_front(std::size_t at) {
      std::uint64_t hash = m_hashes[at];
      for (std::size_t i = at; i > 0; --i) {
        m_hashes[i] = m_hashes[i - 1];
      }
      m_hashes[0] = hash;
    }

    std::array<std::uint64_t, k_cache_size> m_hashes {};
    std::size_t m_count = 0;
  };

  /// The host's side: its copy of the client's list, and the messages that bring it a state
  class sender_t {
  public:
    /// SHAPE chunks first when the client lacks state's shape, then the STATE. shape is the one the
    /// state's hash names (ignored for a hash of 0). Only for messages that go out: the list changes.
    std::vector<message_t> messages(const shape_t *shape, const state_t &state) {
      std::vector<message_t> out;
      state_t sent = state;
      if (state.hash != 0) {
        if (!m_order.contains(state.hash)) {
          std::vector<std::string> chunks;
          if (shape != nullptr && shape->hash == state.hash) {
            chunks = shape_messages(*shape);
          }
          if (chunks.empty()) {
            // Nothing to send it with: the state names no shape
            sent.hash = 0;
          } else {
            for (std::string &chunk : chunks) {
              out.push_back({k_type_shape, std::move(chunk)});
            }
            m_order.insert(state.hash);
          }
        }
        if (sent.hash != 0) {
          m_order.touch(sent.hash);
        }
      }
      out.push_back({k_type_state, state_message(sent)});
      return out;
    }

    bool has(std::uint64_t hash) const {
      return m_order.contains(hash);
    }

    const mru_t &order() const {
      return m_order;
    }

    void reset() {
      m_order.clear();
    }

  private:
    mru_t m_order;
  };

  namespace detail {
    /// A shape whose chunks are still arriving (receiver_t)
    struct partial_shape_t {
      std::uint64_t hash = 0;
      std::size_t total = 0;
      std::size_t received = 0;
      bool usable = false;  ///< Its header made sense, and so far its chunks did
      shape_t shape;
    };
  }  // namespace detail

  /// The client's side: puts chunks back together and keeps the shapes the list has
  class receiver_t {
  public:
    /// A SHAPE message; true when it completed a shape (usable or not, it's in the list now)
    bool shape_message(const std::uint8_t *data, std::size_t size) {
      if (data == nullptr || size < k_shape_header_size || data[0] != k_version) {
        m_partial.reset();
        return false;
      }
      std::uint64_t hash = detail::get(data + 4, 8);
      int width = static_cast<int>(detail::get(data + 12, 2));
      int height = static_cast<int>(detail::get(data + 14, 2));
      int hotspot_x = static_cast<int>(detail::get(data + 16, 2));
      int hotspot_y = static_cast<int>(detail::get(data + 18, 2));
      std::size_t total = static_cast<std::size_t>(detail::get(data + 20, 4));
      std::size_t offset = static_cast<std::size_t>(detail::get(data + 24, 4));
      const std::uint8_t *chunk = data + k_shape_header_size;
      std::size_t length = size - k_shape_header_size;
      // The host puts a hash in its list as it sends the shape's last chunk, so the client does too,
      // whatever came before
      bool last = offset + length >= total;

      if (offset == 0) {
        // A new shape: one that didn't finish is dropped
        m_partial.emplace();
        m_partial->hash = hash;
        m_partial->total = total;
        m_partial->usable = valid_size(width, height) && total == data_size(width, height);
        if (m_partial->usable) {
          m_partial->shape = blank_shape(width, height, hotspot_x, hotspot_y);
          m_partial->shape.hash = hash;
        }
      } else if (!m_partial || m_partial->hash != hash || m_partial->received != offset || m_partial->total != total) {
        m_partial.reset();
        if (last) {
          add(hash, nullptr);
        }
        return last;
      }

      detail::partial_shape_t &partial = *m_partial;
      if (length > partial.total - partial.received) {
        partial.usable = false;
        length = partial.total - partial.received;
      }
      if (partial.usable) {
        std::size_t pixels = partial.shape.bgra.size();
        for (std::size_t i = 0; i < length; ++i) {
          std::size_t at = partial.received + i;
          if (at < pixels) {
            partial.shape.bgra[at] = chunk[i];
          } else {
            partial.shape.invert[at - pixels] = chunk[i];
          }
        }
      }
      partial.received += length;
      if (partial.received < partial.total && !last) {
        return false;
      }

      // Complete: a shape whose hash doesn't match or whose hotspot is outside it is kept as unusable
      bool usable = partial.usable && partial.received == partial.total && well_formed(partial.shape) && hash_of(partial.shape) == hash;
      std::shared_ptr<const shape_t> done = usable ? std::make_shared<const shape_t>(std::move(partial.shape)) : nullptr;
      m_partial.reset();
      add(hash, std::move(done));
      return true;
    }

    /// A STATE message: makes its shape the most recent; none when it's malformed
    std::optional<state_t> state_message(const std::uint8_t *data, std::size_t size) {
      std::optional<state_t> state = parse_state(data, size);
      if (state && state->hash != 0) {
        m_order.touch(state->hash);
      }
      return state;
    }

    /// The shape a hash names while it's in the list; null for none, or one that couldn't be used
    std::shared_ptr<const shape_t> shape(std::uint64_t hash) const {
      auto found = m_shapes.find(hash);
      return found != m_shapes.end() ? found->second : nullptr;
    }

    bool has(std::uint64_t hash) const {
      return m_order.contains(hash);
    }

    const mru_t &order() const {
      return m_order;
    }

    void reset() {
      m_order.clear();
      m_shapes.clear();
      m_partial.reset();
    }

  private:
    void add(std::uint64_t hash, std::shared_ptr<const shape_t> shape) {
      std::uint64_t evicted = m_order.insert(hash);
      if (evicted != 0) {
        m_shapes.erase(evicted);
      }
      m_shapes[hash] = std::move(shape);
    }

    mru_t m_order;
    std::unordered_map<std::uint64_t, std::shared_ptr<const shape_t>> m_shapes;
    std::optional<detail::partial_shape_t> m_partial;
  };

  /**
   * @brief The shape as the client draws it, straight-alpha BGRA: a client can't invert what's
   * under its pointer, so an inverting pixel is white, and a clear pixel beside one (of the 8 around
   * it) is black, as an outline that shows on any background.
   */
  inline std::vector<std::uint8_t> display_pixels(const shape_t &shape) {
    std::vector<std::uint8_t> out = shape.bgra;
    if (!well_formed(shape)) {
      return out;
    }
    for (int y = 0; y < shape.height; ++y) {
      for (int x = 0; x < shape.width; ++x) {
        std::size_t pixel = (static_cast<std::size_t>(y) * shape.width + x) * 4;
        if (inverts(shape, x, y)) {
          out[pixel] = out[pixel + 1] = out[pixel + 2] = out[pixel + 3] = 0xFF;
          continue;
        }
        if (shape.bgra[pixel + 3] != 0) {
          continue;
        }
        bool beside = false;
        for (int dy = -1; dy <= 1 && !beside; ++dy) {
          for (int dx = -1; dx <= 1 && !beside; ++dx) {
            int nx = x + dx;
            int ny = y + dy;
            beside = (dx != 0 || dy != 0) && nx >= 0 && ny >= 0 && nx < shape.width && ny < shape.height && inverts(shape, nx, ny);
          }
        }
        if (beside) {
          out[pixel] = out[pixel + 1] = out[pixel + 2] = 0;
          out[pixel + 3] = 0xFF;
        }
      }
    }
    return out;
  }

}  // namespace luzcursor
