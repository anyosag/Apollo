// Tests for src/fork/cursor_shape_policy.h: LuzLunar's pointer drawn by the Mac. Desktop
// Duplication's three shape types converted and hashed, when the capture leaves the pointer out
// and skips a frame, the STATE a stream gets, its pacing, the draw request, and the shapes going
// through luzcursor.h's chunks and cache. A standalone program, so CI builds it without the rest
// of Apollo:
//   g++ -std=c++23 -iquote src -Ithird-party tests/fork/header/cursor_shape_policy_test.cpp

#include "fork/cursor_shape_policy.h"

#include <cstdio>
#include <vector>

using namespace fork_tweaks::cursor_shape_policy;
using namespace std::chrono_literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static std::vector<std::uint8_t> pixel(const luzcursor::shape_t &shape, int x, int y) {
  std::size_t at = (static_cast<std::size_t>(y) * shape.width + x) * 4;
  return {shape.bgra[at], shape.bgra[at + 1], shape.bgra[at + 2], shape.bgra[at + 3]};
}

static const std::vector<std::uint8_t> k_black {0, 0, 0, 0xFF};
static const std::vector<std::uint8_t> k_white {0xFF, 0xFF, 0xFF, 0xFF};
static const std::vector<std::uint8_t> k_clear {0, 0, 0, 0};

static void test_monochrome() {
  // 4 pixels across, 1 row, as Windows has it: a 2-byte pitch, the AND row then the XOR row.
  // Pixels: AND 0/XOR 0 black, AND 0/XOR 1 white, AND 1/XOR 0 clear, AND 1/XOR 1 inverts.
  std::vector<std::uint8_t> data {
    0b00110000, 0x00,  // AND
    0b01010000, 0x00,  // XOR
  };
  auto shape = convert(shape_monochrome, data.data(), data.size(), 4, 2, 2, 1, 0);
  CHECK(shape.has_value());
  if (!shape) {
    return;
  }
  CHECK(shape->width == 4 && shape->height == 1);
  CHECK(shape->hotspot_x == 1 && shape->hotspot_y == 0);
  CHECK(pixel(*shape, 0, 0) == k_black);
  CHECK(pixel(*shape, 1, 0) == k_white);
  CHECK(pixel(*shape, 2, 0) == k_clear && !luzcursor::inverts(*shape, 2, 0));
  CHECK(pixel(*shape, 3, 0) == k_clear && luzcursor::inverts(*shape, 3, 0));
  CHECK(!luzcursor::inverts(*shape, 0, 0) && !luzcursor::inverts(*shape, 1, 0));
  CHECK(shape->hash == luzcursor::hash_of(*shape));
  CHECK(luzcursor::well_formed(*shape));

  // Windows' I-beam: 32x32 with a pitch of 4, a stem of inverting pixels in a clear frame
  std::vector<std::uint8_t> beam(4 * 64, 0);
  for (int y = 0; y < 32; ++y) {
    for (int b = 0; b < 4; ++b) {
      beam[y * 4 + b] = 0xFF;  // AND: all clear...
    }
  }
  for (int y = 8; y < 24; ++y) {
    beam[(32 + y) * 4 + 1] = 0x80;  // ...XOR: x = 8 inverts
  }
  shape = convert(shape_monochrome, beam.data(), beam.size(), 32, 64, 4, 8, 16);
  CHECK(shape && shape->width == 32 && shape->height == 32);
  if (shape) {
    int inverting = 0;
    for (int y = 0; y < 32; ++y) {
      for (int x = 0; x < 32; ++x) {
        inverting += luzcursor::inverts(*shape, x, y) ? 1 : 0;
        CHECK(pixel(*shape, x, y) == k_clear);
      }
    }
    CHECK(inverting == 16 && luzcursor::inverts(*shape, 8, 8) && luzcursor::inverts(*shape, 8, 23));
    // As the Mac draws it: a white stem with a black outline
    std::vector<std::uint8_t> shown = luzcursor::display_pixels(*shape);
    std::size_t stem = (static_cast<std::size_t>(12) * 32 + 8) * 4;
    std::size_t beside = (static_cast<std::size_t>(12) * 32 + 9) * 4;
    CHECK(shown[stem + 3] == 0xFF && shown[stem] == 0xFF);
    CHECK(shown[beside + 3] == 0xFF && shown[beside] == 0);
  }

  // A pitch too small for the width, or a buffer too short
  CHECK(!convert(shape_monochrome, data.data(), data.size(), 9, 2, 1, 0, 0));
  CHECK(!convert(shape_monochrome, data.data(), 3, 4, 2, 2, 0, 0));
}

static void test_color() {
  // 2x2 straight-alpha BGRA with a pitch of 12 (4 bytes of padding a row)
  std::vector<std::uint8_t> data {
    10, 20, 30, 255, 1, 2, 3, 128, 0xEE, 0xEE, 0xEE, 0xEE,
    0, 0, 0, 0, 200, 100, 50, 0, 0xEE, 0xEE, 0xEE, 0xEE,
  };
  auto shape = convert(shape_color, data.data(), data.size(), 2, 2, 12, 0, 1);
  CHECK(shape.has_value());
  if (shape) {
    CHECK((pixel(*shape, 0, 0) == std::vector<std::uint8_t> {10, 20, 30, 255}));
    CHECK((pixel(*shape, 1, 0) == std::vector<std::uint8_t> {1, 2, 3, 128}));
    CHECK(pixel(*shape, 0, 1) == k_clear);
    CHECK((pixel(*shape, 1, 1) == std::vector<std::uint8_t> {200, 100, 50, 0}));
    CHECK(shape->hotspot_y == 1);
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        CHECK(!luzcursor::inverts(*shape, x, y));
      }
    }
  }
  CHECK(!convert(shape_color, data.data(), data.size(), 4, 2, 12, 0, 0));  // pitch under 4 a pixel
  CHECK(!convert(shape_color, data.data(), data.size() - 1, 2, 2, 12, 0, 0));
}

static void test_masked_color() {
  // Alpha 0 draws the color; 0xFF XORs it: black stays clear, white inverts, another color too
  std::vector<std::uint8_t> data {
    10, 20, 30, 0,
    0, 0, 0, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0x40, 0x00, 0x00, 0xFF,
  };
  auto shape = convert(shape_masked_color, data.data(), data.size(), 4, 1, 16, 3, 0);
  CHECK(shape.has_value());
  if (shape) {
    CHECK((pixel(*shape, 0, 0) == std::vector<std::uint8_t> {10, 20, 30, 0xFF}));
    CHECK(pixel(*shape, 1, 0) == k_clear && !luzcursor::inverts(*shape, 1, 0));
    CHECK(pixel(*shape, 2, 0) == k_clear && luzcursor::inverts(*shape, 2, 0));
    CHECK(luzcursor::inverts(*shape, 3, 0));
  }
}

static void test_convert_limits() {
  std::vector<std::uint8_t> data(4 * 600 * 600, 0);
  CHECK(!convert(3, data.data(), data.size(), 4, 4, 16, 0, 0));  // no such type
  CHECK(!convert(shape_color, data.data(), data.size(), 600, 4, 2400, 0, 0));  // past k_max_side
  CHECK(!convert(shape_color, data.data(), data.size(), 0, 4, 16, 0, 0));
  CHECK(!convert(shape_color, nullptr, 0, 4, 4, 16, 0, 0));
  CHECK(!convert(shape_monochrome, data.data(), data.size(), 4, 1, 4, 0, 0));  // half a row
  CHECK(convert(shape_color, data.data(), data.size(), 256, 256, 1024, 0, 0).has_value());
  // A hotspot on the far edge is kept inside the image
  auto edge = convert(shape_color, data.data(), data.size(), 32, 32, 128, 32, 40);
  CHECK(edge && edge->hotspot_x == 31 && edge->hotspot_y == 31 && luzcursor::well_formed(*edge));
  // Each shape has its own hash
  auto a = convert(shape_color, data.data(), data.size(), 32, 32, 128, 0, 0);
  data[5] = 9;
  auto b = convert(shape_color, data.data(), data.size(), 32, 32, 128, 0, 0);
  CHECK(a && b && a->hash != b->hash && a->hash != 0);
}

static void test_capture_rules() {
  CHECK(client_draws(true, true, false));
  CHECK(!client_draws(false, true, false));  // a stream of the display that doesn't draw it
  CHECK(!client_draws(true, false, false));  // no usable shape yet
  CHECK(!client_draws(true, true, true));  // UAC or the lock screen

  CHECK(skip_pointer_only_frame(true, false, false, false, true));
  CHECK(!skip_pointer_only_frame(false, false, false, false, true));
  CHECK(!skip_pointer_only_frame(true, true, false, false, true));  // the picture changed
  CHECK(!skip_pointer_only_frame(true, false, true, false, true));  // the pulse runs
  CHECK(!skip_pointer_only_frame(true, false, false, true, true));  // the pointer is drawn
  CHECK(!skip_pointer_only_frame(true, false, false, false, false));  // the last one had the pointer
}

static pointer_t sample_pointer() {
  pointer_t pointer;
  pointer.known = true;
  pointer.x = 100;
  pointer.y = 200;
  pointer.width = 2560;
  pointer.height = 1440;
  pointer.showing = true;
  // The virtual screen: another display left of this one
  pointer.clip_left = -1920;
  pointer.clip_top = 0;
  pointer.clip_right = 2560;
  pointer.clip_bottom = 1440;
  return pointer;
}

static capture_t drawing_capture() {
  capture_t capture;
  capture.fresh = true;
  capture.client_draws = true;
  capture.pointer_visible = true;
  capture.hash = 0xABCDEF;
  return capture;
}

static void test_state() {
  luzcursor::state_t state = state_for(drawing_capture(), sample_pointer(), 7, -3);
  CHECK(state.client_draws());
  CHECK(state.hash == 0xABCDEF && state.x == 100 && state.y == 200);
  CHECK(state.width == 2560 && state.height == 1440);
  CHECK(state.sum_dx == 7 && state.sum_dy == -3);
  // Unclipped, the clip rectangle is where Windows keeps it, the virtual screen
  CHECK(!state.clipped() && state.clip_left == -1920 && state.clip_top == 0 && state.clip_right == 2560 && state.clip_bottom == 1440);

  // The host draws it, so no shape: a stale capture (Windows Graphics Capture), one that draws it,
  // one with no shape
  capture_t stale = drawing_capture();
  stale.fresh = false;
  state = state_for(stale, sample_pointer(), 0, 0);
  CHECK(state.host_draws() && state.hash == 0 && !state.client_draws());
  capture_t drawn = drawing_capture();
  drawn.client_draws = false;
  CHECK(state_for(drawn, sample_pointer(), 0, 0).host_draws());
  capture_t shapeless = drawing_capture();
  shapeless.hash = 0;
  CHECK(state_for(shapeless, sample_pointer(), 0, 0).host_draws());

  // Hidden: by an app, by the host's shortcut, or as Desktop Duplication has it on this display
  pointer_t hidden = sample_pointer();
  hidden.showing = false;
  CHECK(!state_for(drawing_capture(), hidden, 0, 0).visible());
  capture_t host_hid = drawing_capture();
  host_hid.hidden_by_host = true;
  CHECK(!state_for(host_hid, sample_pointer(), 0, 0).visible());
  capture_t dda_hid = drawing_capture();
  dda_hid.pointer_visible = false;
  CHECK(!state_for(dda_hid, sample_pointer(), 0, 0).visible());

  // On another display: still showing, as Desktop Duplication only speaks of its own
  pointer_t away = sample_pointer();
  away.x = 2560;
  state = state_for(dda_hid, away, 0, 0);
  CHECK(state.visible() && !state.on_display() && !state.client_draws());
  away.x = -1;
  CHECK(!state_for(drawing_capture(), away, 0, 0).on_display());
  pointer_t unknown;
  CHECK(!state_for(drawing_capture(), unknown, 0, 0).visible());

  // A clip rectangle
  pointer_t clipped = sample_pointer();
  clipped.clipped = true;
  clipped.clip_left = 10;
  clipped.clip_top = 20;
  clipped.clip_right = 300;
  clipped.clip_bottom = 400;
  state = state_for(drawing_capture(), clipped, 0, 0);
  CHECK(state.clipped() && state.clip_left == 10 && state.clip_top == 20 && state.clip_right == 300 && state.clip_bottom == 400);
}

static void test_pacer() {
  using clock = std::chrono::steady_clock;
  clock::time_point t0 {};
  pacer_t pacer;
  luzcursor::state_t state = state_for(drawing_capture(), sample_pointer(), 0, 0);
  CHECK(pacer.due(state, t0));
  pacer.sent(state, t0);
  CHECK(!pacer.due(state, t0 + 1s));  // nothing new

  // A move waits out position_interval; the last of it goes once that's over
  luzcursor::state_t moved = state;
  moved.x += 3;
  moved.sum_dx += 3;
  CHECK(!pacer.due(moved, t0 + 5ms));
  CHECK(pacer.due(moved, t0 + position_interval));
  pacer.sent(moved, t0 + position_interval);

  // Anything else goes at once
  luzcursor::state_t shape = moved;
  shape.hash = 0x1234;
  CHECK(pacer.due(shape, t0 + position_interval + 1ms));
  luzcursor::state_t hid = moved;
  hid.flags = static_cast<std::uint8_t>(hid.flags & ~luzcursor::flag_visible);
  CHECK(pacer.due(hid, t0 + position_interval + 1ms));
  luzcursor::state_t clip = moved;
  clip.clip_right -= 1;
  CHECK(pacer.due(clip, t0 + position_interval + 1ms));
  // With a move at the same time too
  shape.x += 1;
  CHECK(pacer.due(shape, t0 + position_interval + 1ms));
  CHECK(pacer.last() && *pacer.last() == moved);

  CHECK(control_interval <= 8ms);
}

static void test_draw_request() {
  CHECK(parse_draw(R"({"draw":"client"})") == std::optional<bool> {true});
  CHECK(parse_draw(R"({"draw":"host"})") == std::optional<bool> {false});
  CHECK(!parse_draw(R"({"draw":"mac"})"));
  CHECK(!parse_draw(R"({"draw":1})"));
  CHECK(!parse_draw(R"({"pulse":2})"));
  CHECK(!parse_draw("not json"));
  CHECK(is_draw_request(R"({"draw":"mac"})") && is_draw_request(R"({"draw":"host"})"));
  CHECK(!is_draw_request(R"({"pulse":2,"ifAtTop":true})") && !is_draw_request("[]"));
  CHECK(draw_json(true) == R"({"draw":"client"})" && draw_json(false) == R"({"draw":"host"})");
}

// A converted shape goes to the client in chunks, once, and the STATE after names it
static void test_shapes_to_client() {
  std::vector<std::uint8_t> data(256 * 256 * 4);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(i * 13);
  }
  auto shape = convert(shape_color, data.data(), data.size(), 256, 256, 1024, 4, 4);
  CHECK(shape.has_value());
  if (!shape) {
    return;
  }
  capture_t capture = drawing_capture();
  capture.hash = shape->hash;
  luzcursor::state_t state = state_for(capture, sample_pointer(), 0, 0);

  luzcursor::sender_t sender;
  luzcursor::receiver_t receiver;
  auto first = sender.messages(&*shape, state);
  CHECK(first.size() == 6);  // 5 chunks for 256x256, then the STATE
  for (const auto &message : first) {
    auto data_p = reinterpret_cast<const std::uint8_t *>(message.payload.data());
    if (message.type == luzcursor::k_type_shape) {
      CHECK(message.payload.size() <= luzcursor::k_shape_header_size + luzcursor::k_max_chunk);
      receiver.shape_message(data_p, message.payload.size());
    } else {
      auto got = receiver.state_message(data_p, message.payload.size());
      CHECK(got && got->hash == shape->hash);
    }
  }
  auto got = receiver.shape(shape->hash);
  CHECK(got && got->bgra == shape->bgra);
  // Again: the STATE only
  auto second = sender.messages(&*shape, state);
  CHECK(second.size() == 1 && second[0].type == luzcursor::k_type_state);
}

int main() {
  test_monochrome();
  test_color();
  test_masked_color();
  test_convert_limits();
  test_capture_rules();
  test_state();
  test_pacer();
  test_draw_request();
  test_shapes_to_client();

  if (s_Failures == 0) {
    std::printf("cursor_shape_policy: all tests passed\n");
    return 0;
  }
  std::printf("cursor_shape_policy: %d failure(s)\n", s_Failures);
  return 1;
}
