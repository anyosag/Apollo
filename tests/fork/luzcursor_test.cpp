// Tests for luzcursor.h (the PC's pointer drawn by the Mac, shared by anyosag/Apollo and
// anyosag/LuzLunar). Identical in both repos. A standalone program, so CI builds it without either app:
//   g++ -std=c++17 -I<dir with luzcursor.h> luzcursor_test.cpp

#include "luzcursor.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace luzcursor;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static const std::uint8_t *bytes(const std::string &s) {
  return reinterpret_cast<const std::uint8_t *>(s.data());
}

static std::uint64_t read_le(const std::string &s, std::size_t at, int count) {
  std::uint64_t value = 0;
  for (int i = 0; i < count; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(s[at + i])) << (8 * i);
  }
  return value;
}

// A shape whose pixels depend on seed, with a few inverting pixels
static shape_t make_shape(int width, int height, int seed) {
  shape_t shape = blank_shape(width, height, seed % width, (seed / 3) % height);
  for (std::size_t i = 0; i < shape.bgra.size(); ++i) {
    shape.bgra[i] = static_cast<std::uint8_t>((i * 31 + seed * 7) & 0xFF);
  }
  set_inverts(shape, (seed + 1) % width, (seed + 2) % height);
  shape.hash = hash_of(shape);
  return shape;
}

static void test_constants() {
  CHECK(k_type_shape >= k_type_first && k_type_shape <= k_type_last);
  CHECK(k_type_state >= k_type_first && k_type_state <= k_type_last);
  CHECK(k_type_shape == 0x4C01 && k_type_state == 0x4C02);
  CHECK(k_channel == 0x07);
  CHECK(k_host_feature_flag == 0x10000000);
  CHECK(k_serverinfo_version == 2);
  CHECK(k_cache_size == 64);
  // A whole message fits the control stream's 16-bit lengths, with the V2 header (4), the
  // encrypted header's sequence number (4) and the AES-GCM tag (16)
  CHECK(4 + k_shape_header_size + k_max_chunk + 4 + 16 <= 0xFFFF);
  CHECK(mask_stride(1) == 1 && mask_stride(8) == 1 && mask_stride(9) == 2 && mask_stride(32) == 4);
  CHECK(data_size(32, 32) == 32 * 32 * 4 + 4 * 32);
  CHECK(valid_size(1, 1) && valid_size(k_max_side, k_max_side));
  CHECK(!valid_size(0, 4) && !valid_size(4, 0) && !valid_size(k_max_side + 1, 4));
}

static void test_state_layout() {
  state_t state;
  state.flags = flag_visible | flag_on_display | flag_clipped;
  state.hash = 0x0123456789ABCDEFull;
  state.x = -5;
  state.y = 1079;
  state.width = 2560;
  state.height = 1600;
  state.sum_dx = -1234567890123ll;
  state.sum_dy = 42;
  state.clip_left = 10;
  state.clip_top = -20;
  state.clip_right = 2000;
  state.clip_bottom = 1500;

  std::string message = state_message(state);
  CHECK(message.size() == k_state_size);
  CHECK(read_le(message, 0, 1) == k_version);
  CHECK(read_le(message, 1, 1) == (flag_visible | flag_on_display | flag_clipped));
  CHECK(read_le(message, 2, 2) == 0);
  CHECK(read_le(message, 4, 8) == 0x0123456789ABCDEFull);
  CHECK(static_cast<std::int32_t>(read_le(message, 12, 4)) == -5);
  CHECK(read_le(message, 16, 4) == 1079);
  CHECK(read_le(message, 20, 4) == 2560);
  CHECK(read_le(message, 24, 4) == 1600);
  CHECK(static_cast<std::int64_t>(read_le(message, 28, 8)) == -1234567890123ll);
  CHECK(read_le(message, 36, 8) == 42);
  CHECK(read_le(message, 44, 4) == 10);
  CHECK(static_cast<std::int32_t>(read_le(message, 48, 4)) == -20);
  CHECK(read_le(message, 52, 4) == 2000);
  CHECK(read_le(message, 56, 4) == 1500);

  std::optional<state_t> parsed = parse_state(bytes(message), message.size());
  CHECK(parsed && *parsed == state);
  CHECK(parsed && parsed->visible() && parsed->on_display() && !parsed->host_draws() && parsed->clipped());
  CHECK(parsed && parsed->client_draws());

  // A later version's extra fields are left alone; another version, or a short one, isn't read
  std::string longer = message + std::string(8, '\x7f');
  parsed = parse_state(bytes(longer), longer.size());
  CHECK(parsed && *parsed == state);
  CHECK(!parse_state(bytes(message), message.size() - 1));
  std::string other = message;
  other[0] = 2;
  CHECK(!parse_state(bytes(other), other.size()));
  CHECK(!parse_state(nullptr, 0));

  // What the client draws
  state_t hidden = state;
  hidden.flags = flag_on_display;
  CHECK(!hidden.client_draws());
  state_t off = state;
  off.flags = flag_visible;
  CHECK(!off.client_draws());
  state_t host = state;
  host.flags |= flag_host_draws;
  CHECK(!host.client_draws());
  state_t no_shape = state;
  no_shape.hash = 0;
  CHECK(!no_shape.client_draws());
  CHECK(state_t {}.host_draws());
}

static void test_hash() {
  shape_t a = make_shape(32, 32, 3);
  shape_t b = a;
  CHECK(hash_of(a) == hash_of(b));
  CHECK(hash_of(a) != 0);
  b.bgra[100] ^= 1;
  CHECK(hash_of(a) != hash_of(b));
  b = a;
  b.hotspot_x += 1;
  CHECK(hash_of(a) != hash_of(b));
  b = a;
  b.invert[3] ^= 0x10;
  CHECK(hash_of(a) != hash_of(b));
  // The same pixels in another size aren't the same shape
  shape_t wide = blank_shape(64, 16, 0, 0);
  shape_t tall = blank_shape(16, 64, 0, 0);
  CHECK(hash_of(wide) != hash_of(tall));
}

static void test_chunks() {
  // A small one is one message: the header, then the pixels, then the mask
  shape_t small = make_shape(32, 32, 5);
  std::vector<std::string> messages = shape_messages(small);
  CHECK(messages.size() == 1);
  CHECK(messages[0].size() == k_shape_header_size + data_size(32, 32));
  CHECK(read_le(messages[0], 0, 1) == k_version);
  CHECK(read_le(messages[0], 4, 8) == small.hash);
  CHECK(read_le(messages[0], 12, 2) == 32 && read_le(messages[0], 14, 2) == 32);
  CHECK(read_le(messages[0], 16, 2) == static_cast<std::uint64_t>(small.hotspot_x));
  CHECK(read_le(messages[0], 18, 2) == static_cast<std::uint64_t>(small.hotspot_y));
  CHECK(read_le(messages[0], 20, 4) == data_size(32, 32) && read_le(messages[0], 24, 4) == 0);
  CHECK(std::memcmp(messages[0].data() + k_shape_header_size, small.bgra.data(), small.bgra.size()) == 0);
  CHECK(std::memcmp(messages[0].data() + k_shape_header_size + small.bgra.size(), small.invert.data(), small.invert.size()) == 0);

  // The largest Windows pointer, 256 pixels across, in chunks of at most k_max_chunk
  shape_t large = make_shape(256, 256, 9);
  messages = shape_messages(large);
  std::size_t total = data_size(256, 256);
  CHECK(messages.size() == (total + k_max_chunk - 1) / k_max_chunk);
  std::size_t offset = 0;
  for (const std::string &message : messages) {
    CHECK(message.size() <= k_shape_header_size + k_max_chunk);
    CHECK(read_le(message, 24, 4) == offset);
    CHECK(read_le(message, 20, 4) == total);
    offset += message.size() - k_shape_header_size;
  }
  CHECK(offset == total);

  receiver_t receiver;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    bool done = receiver.shape_message(bytes(messages[i]), messages[i].size());
    CHECK(done == (i + 1 == messages.size()));
  }
  std::shared_ptr<const shape_t> got = receiver.shape(large.hash);
  CHECK(got != nullptr);
  CHECK(got && got->width == 256 && got->height == 256 && got->hotspot_x == large.hotspot_x && got->hotspot_y == large.hotspot_y);
  CHECK(got && got->bgra == large.bgra && got->invert == large.invert && got->hash == large.hash);

  // Nothing for a shape that isn't well formed, or without its hash
  shape_t bad = small;
  bad.hotspot_x = 32;
  CHECK(shape_messages(bad).empty());
  shape_t unhashed = small;
  unhashed.hash = 0;
  CHECK(shape_messages(unhashed).empty());
}

static void test_mru() {
  mru_t list;
  CHECK(list.insert(1) == 0);
  CHECK(list.insert(2) == 0);
  CHECK(list.insert(3) == 0);
  CHECK(list.at(0) == 3 && list.at(1) == 2 && list.at(2) == 1);
  CHECK(list.touch(1));
  CHECK(list.at(0) == 1 && list.at(1) == 3 && list.at(2) == 2);
  CHECK(!list.touch(9));
  CHECK(list.insert(3) == 0);  // already there: made the most recent
  CHECK(list.size() == 3 && list.at(0) == 3);

  list.clear();
  for (std::uint64_t hash = 1; hash <= k_cache_size; ++hash) {
    CHECK(list.insert(hash) == 0);
  }
  CHECK(list.size() == k_cache_size);
  CHECK(list.touch(1));  // the oldest becomes the most recent, so 2 is the oldest
  CHECK(list.insert(1000) == 2);
  CHECK(!list.contains(2) && list.contains(1) && list.contains(1000));
  CHECK(list.size() == k_cache_size);
}

// The host's states go through both sides; the client always has the shape a STATE names, and
// both lists stay the same
static void test_exchange() {
  sender_t host;
  receiver_t client;
  std::vector<shape_t> shapes;
  for (int i = 0; i < 90; ++i) {
    shapes.push_back(make_shape(16 + i % 40, 20 + i % 13, i));
  }
  // Up through all 90, then back over the first ones (pushed out by then), then a spinner of 8
  std::vector<int> order;
  for (int i = 0; i < 90; ++i) {
    order.push_back(i);
  }
  for (int i = 0; i < 10; ++i) {
    order.push_back(i);
  }
  for (int turn = 0; turn < 5; ++turn) {
    for (int i = 80; i < 88; ++i) {
      order.push_back(i);
    }
  }

  int shape_messages_sent = 0;
  int spinner_shapes = 0;
  for (std::size_t step = 0; step < order.size(); ++step) {
    const shape_t &shape = shapes[order[step]];
    state_t state;
    state.flags = flag_visible | flag_on_display;
    state.hash = shape.hash;
    state.width = 1920;
    state.height = 1080;
    state.x = static_cast<std::int32_t>(step);
    for (const message_t &message : host.messages(&shape, state)) {
      if (message.type == k_type_shape) {
        ++shape_messages_sent;
        if (step >= 100 + 8) {
          ++spinner_shapes;  // after the spinner's first turn
        }
        client.shape_message(bytes(message.payload), message.payload.size());
      } else {
        CHECK(message.type == k_type_state);
        std::optional<state_t> got = client.state_message(bytes(message.payload), message.payload.size());
        CHECK(got && got->hash == shape.hash && got->x == state.x);
        std::shared_ptr<const shape_t> drawn = got ? client.shape(got->hash) : nullptr;
        CHECK(drawn && drawn->bgra == shape.bgra);
      }
    }
    bool same = host.order().size() == client.order().size();
    for (std::size_t i = 0; same && i < host.order().size(); ++i) {
      same = host.order().at(i) == client.order().at(i);
    }
    CHECK(same);
  }
  // Each shape went once, the ten pushed out once more, and the spinner never again
  CHECK(shape_messages_sent == 90 + 10);
  CHECK(spinner_shapes == 0);

  // A state with no shape, or one the host has no image for, names none
  state_t none;
  none.flags = flag_visible | flag_on_display;
  std::vector<message_t> out = host.messages(nullptr, none);
  CHECK(out.size() == 1 && out[0].type == k_type_state);
  state_t unknown = none;
  unknown.hash = 0xDEADBEEF;
  out = host.messages(nullptr, unknown);
  CHECK(out.size() == 1);
  std::optional<state_t> parsed = parse_state(bytes(out[0].payload), out[0].payload.size());
  CHECK(parsed && parsed->hash == 0);
  CHECK(!host.has(0xDEADBEEF));

  // A new stream starts from nothing on both sides
  host.reset();
  client.reset();
  CHECK(host.order().size() == 0 && client.order().size() == 0 && !client.shape(shapes[0].hash));
}

// What the client does with chunks that don't add up: the list still gets the hash when the host's
// does (its last chunk), without an image
static void test_receiver_rejects() {
  shape_t shape = make_shape(200, 200, 4);
  std::vector<std::string> messages = shape_messages(shape, 50000);
  CHECK(messages.size() == 4);

  // A wrong hash: in the list, unusable
  {
    receiver_t client;
    shape_t lying = shape;
    lying.hash = shape.hash ^ 1;
    for (const std::string &message : shape_messages(lying, 50000)) {
      client.shape_message(bytes(message), message.size());
    }
    CHECK(client.has(lying.hash) && client.shape(lying.hash) == nullptr);
  }
  // A chunk missing in the middle: the shape's last chunk still puts it in the list
  {
    receiver_t client;
    CHECK(!client.shape_message(bytes(messages[0]), messages[0].size()));
    CHECK(!client.shape_message(bytes(messages[2]), messages[2].size()));
    CHECK(client.shape_message(bytes(messages[3]), messages[3].size()));
    CHECK(client.has(shape.hash) && client.shape(shape.hash) == nullptr);
  }
  // A new shape starting drops one that didn't finish
  {
    receiver_t client;
    shape_t other = make_shape(8, 8, 1);
    std::vector<std::string> small = shape_messages(other);
    client.shape_message(bytes(messages[0]), messages[0].size());
    CHECK(client.shape_message(bytes(small[0]), small[0].size()));
    CHECK(client.shape(other.hash) != nullptr && !client.has(shape.hash));
  }
  // A size the format doesn't allow
  {
    receiver_t client;
    std::string message = shape_messages(make_shape(8, 8, 2))[0];
    message[12] = 0;  // width 0
    message[13] = 0;
    CHECK(client.shape_message(bytes(message), message.size()));
    std::uint64_t hash = read_le(message, 4, 8);
    CHECK(client.has(hash) && client.shape(hash) == nullptr);
  }
  // Too short for a header, or another version: nothing
  {
    receiver_t client;
    CHECK(!client.shape_message(bytes(messages[0]), k_shape_header_size - 1));
    std::string other = messages[0];
    other[0] = 9;
    CHECK(!client.shape_message(bytes(other), other.size()));
    CHECK(client.order().size() == 0);
  }
  // A STATE naming a shape that isn't in the list changes nothing
  {
    receiver_t client;
    state_t state;
    state.hash = 77;
    std::string message = state_message(state);
    CHECK(client.state_message(bytes(message), message.size()));
    CHECK(!client.has(77));
  }
}

static void test_display_pixels() {
  // An I-beam's stem: a column of inverting pixels in a clear 5x5
  shape_t beam = blank_shape(5, 5, 2, 2);
  for (int y = 1; y <= 3; ++y) {
    set_inverts(beam, 2, y);
  }
  // A red pixel in the corner that's neither
  beam.bgra[(4 * 5 + 4) * 4 + 2] = 0xFF;
  beam.bgra[(4 * 5 + 4) * 4 + 3] = 0xFF;
  beam.hash = hash_of(beam);

  std::vector<std::uint8_t> out = display_pixels(beam);
  auto pixel = [&out](int x, int y) {
    std::size_t at = (static_cast<std::size_t>(y) * 5 + x) * 4;
    return std::vector<std::uint8_t> {out[at], out[at + 1], out[at + 2], out[at + 3]};
  };
  std::vector<std::uint8_t> white {0xFF, 0xFF, 0xFF, 0xFF};
  std::vector<std::uint8_t> black {0, 0, 0, 0xFF};
  std::vector<std::uint8_t> clear {0, 0, 0, 0};
  CHECK(pixel(2, 1) == white && pixel(2, 2) == white && pixel(2, 3) == white);
  // Its outline, all around, corners included
  CHECK(pixel(1, 0) == black && pixel(2, 0) == black && pixel(3, 0) == black);
  CHECK(pixel(1, 2) == black && pixel(3, 2) == black);
  CHECK(pixel(1, 4) == black && pixel(2, 4) == black && pixel(3, 4) == black);
  // Two away stays clear, and a pixel with its own color keeps it
  CHECK(pixel(0, 2) == clear && pixel(4, 0) == clear);
  CHECK((pixel(4, 4) == std::vector<std::uint8_t> {0, 0, 0xFF, 0xFF}));
  // set_inverts() cleared the pixel's color, as the format has it
  CHECK(beam.bgra[(1 * 5 + 2) * 4 + 3] == 0);
}

int main() {
  test_constants();
  test_state_layout();
  test_hash();
  test_chunks();
  test_mru();
  test_exchange();
  test_receiver_rejects();
  test_display_pixels();

  if (s_Failures == 0) {
    std::printf("luzcursor: all tests passed\n");
    return 0;
  }
  std::printf("luzcursor: %d failure(s)\n", s_Failures);
  return 1;
}
