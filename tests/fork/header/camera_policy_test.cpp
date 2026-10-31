// Tests for src/fork/camera_policy.h: when the Mac's camera starts and stops, which frames reach the
// decoder, the placeholder's text, the shared memory the camera's media source reads, and the NV12
// drawing. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/camera_policy_test.cpp

#include "fork/camera_policy.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace camera_policy;
using namespace std::literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

using action = start_stop_t::action_e;

static void test_sizes() {
  CHECK((supported_size(1280, 720) == frame_size_t {1280, 720}));
  CHECK((supported_size(1920, 1080) == frame_size_t {1920, 1080}));
  // Others get the nearest, by pixels
  CHECK((supported_size(640, 480) == frame_size_t {1280, 720}));
  CHECK((supported_size(3840, 2160) == frame_size_t {1920, 1080}));
  CHECK((supported_size(1600, 900) == frame_size_t {1280, 720}));
  CHECK((supported_size(1760, 990) == frame_size_t {1920, 1080}));
  CHECK((supported_size(0, 0) == frame_size_t {1280, 720}));
  CHECK(nv12_bytes(1920, 1080) == 3110400);
  CHECK(nv12_bytes(1280, 720) == 1382400);
}

static void test_start_stop() {
  auto t0 = clock::now();
  start_stop_t policy;
  frame_size_t hd {1280, 720};
  frame_size_t full {1920, 1080};

  // Nothing to say without a Mac, whatever the apps want
  CHECK(policy.update(true, hd, false, t0) == action::none);
  CHECK(!policy.started());

  // A Mac connects while an app streams: START at the app's size, with its bitrate
  CHECK(policy.update(true, hd, true, t0) == action::start);
  CHECK(policy.started());
  luzcamera::start_t start = policy.start();
  CHECK(start.width == 1280 && start.height == 720 && start.fps == 30 && start.kbps == 2500);
  CHECK(policy.update(true, hd, true, t0 + 1s) == action::none);

  // The app switches to 1080p: START again
  CHECK(policy.update(true, full, true, t0 + 2s) == action::start);
  CHECK(policy.start().kbps == 5000);

  // The app closes the camera: nothing for stop_delay, then STOP, once
  auto closed = t0 + 10s;
  CHECK(policy.update(false, full, true, closed) == action::none);
  CHECK(policy.deadline() && *policy.deadline() == closed + stop_delay);
  CHECK(policy.update(false, full, true, closed + stop_delay - 1ms) == action::none);
  CHECK(policy.update(false, full, true, closed + stop_delay) == action::stop);
  CHECK(!policy.started());
  CHECK(!policy.deadline());
  CHECK(policy.update(false, full, true, closed + 20s) == action::none);

  // Reopened within the delay at the same size: the camera stays on, with nothing sent
  CHECK(policy.update(true, full, true, t0 + 30s) == action::start);
  CHECK(policy.update(false, full, true, t0 + 31s) == action::none);
  CHECK(policy.update(true, full, true, t0 + 33s) == action::none);
  CHECK(!policy.deadline());
  // or at another size: START, and no stop
  CHECK(policy.update(false, full, true, t0 + 40s) == action::none);
  CHECK(policy.update(true, hd, true, t0 + 41s) == action::start);
  CHECK(policy.update(true, hd, true, t0 + 60s) == action::none);

  // The Mac goes away and comes back: the new connection starts off, so START again
  CHECK(policy.update(true, hd, false, t0 + 61s) == action::none);
  CHECK(!policy.started());
  CHECK(policy.update(true, hd, true, t0 + 62s) == action::start);

  // A Mac that connects while nothing streams hears nothing
  start_stop_t idle;
  CHECK(idle.update(false, hd, true, t0) == action::none);
  CHECK(idle.update(false, hd, true, t0 + 60s) == action::none);
  CHECK(!idle.deadline());
}

static void test_frame_gate() {
  using verdict = frame_gate_t::verdict_e;
  auto t0 = clock::now();
  frame_size_t hd {1280, 720};
  frame_size_t full {1920, 1080};
  frame_gate_t gate;

  // Fresh: a keyframe first, asked for at once
  gate.reset(t0, true);
  CHECK(gate.frame(false, hd, t0) == verdict::drop_and_ask);
  CHECK(gate.frame(false, hd, t0 + 100ms) == verdict::drop);
  CHECK(gate.frame(false, hd, t0 + keyframe_retry) == verdict::drop_and_ask);
  CHECK(gate.frame(true, hd, t0 + 1200ms) == verdict::decode);
  CHECK(!gate.waiting());
  CHECK(gate.frame(false, hd, t0 + 1233ms) == verdict::decode);

  // Without asking at once: the client sends a keyframe first anyway after START
  gate.reset(t0, false);
  CHECK(gate.frame(false, hd, t0 + 10ms) == verdict::drop);
  CHECK(gate.frame(false, hd, t0 + keyframe_retry) == verdict::drop_and_ask);

  // A new size needs its own keyframe
  gate.reset(t0, false);
  CHECK(gate.frame(true, hd, t0) == verdict::decode);
  CHECK(gate.frame(false, full, t0 + 2s) == verdict::drop_and_ask);
  CHECK(gate.frame(true, full, t0 + 2100ms) == verdict::decode);
  // and a keyframe at a new size starts it straight away
  CHECK(gate.frame(true, hd, t0 + 3s) == verdict::decode);
}

static void test_placeholder() {
  CHECK(placeholder_text(placeholder_e::no_stream, "") == "Start a LuzLunar stream to use your Mac's camera");
  CHECK(placeholder_text(placeholder_e::connecting, "Ana's MacBook") == "Connecting to the camera of Ana's MacBook\xE2\x80\xA6");
  CHECK(placeholder_text(placeholder_e::starting, "") == "Starting the camera of the Mac\xE2\x80\xA6");
  CHECK(placeholder_text(placeholder_e::denied, "Mac").find("Privacy & Security") != std::string::npos);
  CHECK(placeholder_text(placeholder_e::unavailable, "Mac", "in use by FaceTime") == "The camera of Mac isn't available: in use by FaceTime");
  CHECK(placeholder_text(placeholder_e::unavailable, "Mac") == "The camera of Mac isn't available");
  CHECK(placeholder_text(placeholder_e::off, "Mac") == "The camera of Mac is off in LuzLunar's settings");

  CHECK(!placeholder_for(luzcamera::status_e::live));
  CHECK(placeholder_for(luzcamera::status_e::denied) == placeholder_e::denied);
  CHECK(placeholder_for(luzcamera::status_e::unavailable) == placeholder_e::unavailable);
  CHECK(placeholder_for(luzcamera::status_e::off) == placeholder_e::off);
  CHECK(placeholder_for(luzcamera::status_e::stopped) == placeholder_e::starting);
  CHECK(placeholder_for((luzcamera::status_e) 77) == placeholder_e::unavailable);
}

static std::vector<std::uint8_t> frame_of(std::uint32_t width, std::uint32_t height, std::uint8_t fill) {
  std::vector<std::uint8_t> nv12(nv12_bytes(width, height), fill);
  return nv12;
}

static void test_shared_frames() {
  using namespace shared;
  CHECK(k_data_offset == 4096);
  CHECK(k_slot_stride % 4096 == 0 && k_slot_stride >= sizeof(slot_header_t) + nv12_bytes(1920, 1080));
  CHECK(k_frames_bytes == k_data_offset + 3 * k_slot_stride);
  // Each slot's header and pixels are 64-byte aligned, for the atomics and fast copies
  CHECK(sizeof(slot_header_t) == 64);
  CHECK(sizeof(frames_header_t) == 64);

  std::vector<std::uint8_t> section(k_frames_bytes + 64);
  // Page-aligned like a mapped view would be (64 is enough for the test)
  std::uint8_t *base = section.data() + (64 - reinterpret_cast<std::uintptr_t>(section.data()) % 64) % 64;
  std::memset(base, 0, k_frames_bytes);
  CHECK(!frames_valid(base));
  init_frames(base);
  CHECK(frames_valid(base));
  CHECK(published(base) == 0);

  // Nothing to read yet
  int calls = 0;
  CHECK(!read_latest(base, [&](const frame_view_t &) {
    calls++;
  }));
  CHECK(calls == 0);

  // A frame goes in and comes out whole
  std::vector<std::uint8_t> a = frame_of(1280, 720, 0x40);
  CHECK(publish(base, 1280, 720, kind_e::camera, 1000, a.data()));
  CHECK(published(base) == 1);
  std::vector<std::uint8_t> got;
  frame_view_t seen;
  CHECK(read_latest(base, [&](const frame_view_t &view) {
    seen = view;
    got.assign(view.nv12, view.nv12 + nv12_bytes(view.width, view.height));
  }));
  CHECK(seen.width == 1280 && seen.height == 720 && seen.kind == kind_e::camera && seen.number == 1);
  CHECK(got == a);

  // The slots rotate, and the newest is always read
  for (int i = 0; i < 7; i++) {
    std::vector<std::uint8_t> f = frame_of(1920, 1080, (std::uint8_t) i);
    CHECK(publish(base, 1920, 1080, i % 2 ? kind_e::placeholder : kind_e::camera, 2000 + i, f.data()));
    CHECK(read_latest(base, [&](const frame_view_t &view) {
      seen = view;
      got.assign(view.nv12, view.nv12 + nv12_bytes(view.width, view.height));
    }));
    CHECK(seen.number == (std::uint64_t) i + 2);
    CHECK(got == f);
    CHECK(frames_header(base)->latest == (std::uint32_t) (i + 1) % k_slot_count);
  }

  // A slot being written isn't read, and one written over during the copy is reported torn
  std::uint32_t latest = frames_header(base)->latest;
  slot_header_t *h = slot_header(base, latest);
  h->seq |= 1u;
  CHECK(!read_latest(base, [](const frame_view_t &) {}));
  h->seq += 1u;
  CHECK(read_latest(base, [](const frame_view_t &) {}));
  CHECK(!read_latest(base, [&](const frame_view_t &) {
    h->seq += 2;  // Apollo wrote it again meanwhile
  }));

  // Apollo's heartbeat says whether its frames are current
  CHECK(!frames_alive(base, 5000));
  beat_frames(base, 5000);
  CHECK(frames_alive(base, 5000));
  CHECK(frames_alive(base, 5000 + heartbeat_timeout_ms));
  CHECK(!frames_alive(base, 5001 + heartbeat_timeout_ms));
  CHECK(!frames_alive(base, 1000));

  // Sizes the slots can't hold are refused
  std::vector<std::uint8_t> big = frame_of(1920, 1080, 1);
  CHECK(!publish(base, 2560, 1440, kind_e::camera, 0, big.data()));
  CHECK(!publish(base, 1281, 720, kind_e::camera, 0, big.data()));
  CHECK(!publish(base, 0, 720, kind_e::camera, 0, big.data()));
}

static void test_shared_control() {
  using namespace shared;
  std::vector<std::uint8_t> block(k_control_bytes + 64);
  std::uint8_t *base = block.data() + (64 - reinterpret_cast<std::uintptr_t>(block.data()) % 64) % 64;
  std::memset(base, 0, k_control_bytes);

  // Before Apollo set it up, nothing streams
  apps_t apps = read_control(base, 1000);
  CHECK(!apps.streaming);
  init_control(base);
  CHECK(!read_control(base, 1000).streaming);

  // The source starts at 1080p
  apps_t wrote;
  wrote.streaming = true;
  wrote.size = {1920, 1080};
  wrote.fps = 30;
  wrote.pid = 4242;
  write_control(base, wrote, 10000);
  apps = read_control(base, 10500);
  CHECK(apps.streaming);
  CHECK((apps.size == frame_size_t {1920, 1080}));
  CHECK(apps.fps == 30 && apps.pid == 4242);
  std::uint32_t seq = reinterpret_cast<control_t *>(base)->seq;
  CHECK(seq == 1);

  // Its heartbeat keeps it streaming; a stale one means it's gone
  CHECK(read_control(base, 10000 + heartbeat_timeout_ms).streaming);
  CHECK(!read_control(base, 10001 + heartbeat_timeout_ms).streaming);
  beat(base, 20000);
  CHECK(read_control(base, 21000).streaming);
  // A clock far behind the beat (the source's, after a reboot of it) doesn't count either
  CHECK(!read_control(base, 1000).streaming);

  // An odd size from the source comes back as the nearest offered one
  wrote.size = {640, 480};
  write_control(base, wrote, 30000);
  CHECK((read_control(base, 30000).size == frame_size_t {1280, 720}));
  CHECK(reinterpret_cast<control_t *>(base)->seq == seq + 1);

  // Stopped
  wrote.streaming = false;
  write_control(base, wrote, 40000);
  CHECK(!read_control(base, 40000).streaming);
}

static void test_nv12() {
  using namespace nv12;
  // Video-range BT.709
  yuv_t black = from_rgb(0, 0, 0);
  yuv_t white = from_rgb(255, 255, 255);
  CHECK(black.y == 16 && black.u == 128 && black.v == 128);
  CHECK(white.y == 235 && white.u == 128 && white.v == 128);
  yuv_t red = from_rgb(255, 0, 0);
  CHECK(red.y >= 61 && red.y <= 64);
  CHECK(red.v > 230);
  yuv_t blue = from_rgb(0, 0, 255);
  CHECK(blue.u > 230);

  // Fill with a pitch: the padding stays untouched
  std::uint32_t w = 8;
  std::uint32_t h = 4;
  std::size_t pitch = 16;
  std::vector<std::uint8_t> frame(pitch * h * 3 / 2, 0xEE);
  fill(frame.data(), w, h, pitch, {50, 60, 70});
  CHECK(frame[0] == 50 && frame[7] == 50 && frame[8] == 0xEE);
  CHECK(frame[pitch * 3 + 7] == 50);
  CHECK(frame[pitch * h] == 60 && frame[pitch * h + 1] == 70 && frame[pitch * h + 7] == 70 && frame[pitch * h + 8] == 0xEE);

  // A copy at the same size is exact; across sizes it's nearest-pixel
  std::vector<std::uint8_t> src(nv12_bytes(4, 2));
  for (std::size_t i = 0; i < src.size(); i++) {
    src[i] = (std::uint8_t) (i + 1);
  }
  std::vector<std::uint8_t> same(16 * 2 * 3 / 2, 0);
  copy_scaled(src.data(), 4, 2, same.data(), 4, 2, 16);
  CHECK(same[0] == 1 && same[3] == 4 && same[16] == 5 && same[19] == 8);
  CHECK(same[32] == 9 && same[35] == 12);
  std::vector<std::uint8_t> doubled(8 * 4 * 3 / 2, 0);
  copy_scaled(src.data(), 4, 2, doubled.data(), 8, 4, 8);
  CHECK(doubled[0] == 1 && doubled[1] == 1 && doubled[2] == 2 && doubled[7] == 4);
  CHECK(doubled[8] == 1 && doubled[16] == 5 && doubled[31] == 8);
  // Chroma pairs stay pairs
  CHECK(doubled[32] == 9 && doubled[33] == 10 && doubled[34] == 9 && doubled[35] == 10 && doubled[36] == 11 && doubled[37] == 12);

  // Crop the decoder's taller frame back to the visible rows
  std::uint32_t cw = 4;
  std::uint32_t ch = 2;
  std::size_t src_pitch = 8;
  std::size_t uv_offset = src_pitch * 4;  // 4 rows decoded for 2 shown
  std::vector<std::uint8_t> decoded(uv_offset + src_pitch * 2, 0);
  for (std::uint32_t r = 0; r < 2; r++) {
    for (std::uint32_t x = 0; x < cw; x++) {
      decoded[r * src_pitch + x] = (std::uint8_t) (10 * r + x);
    }
  }
  for (std::uint32_t x = 0; x < cw; x++) {
    decoded[uv_offset + x] = (std::uint8_t) (100 + x);
  }
  std::vector<std::uint8_t> cropped(nv12_bytes(cw, ch));
  crop(decoded.data(), src_pitch, uv_offset, cw, ch, cropped.data());
  CHECK(cropped == std::vector<std::uint8_t>({0, 1, 2, 3, 10, 11, 12, 13, 100, 101, 102, 103}));

  // BGRA to NV12: a white frame is white, a red one's chroma is red's
  std::vector<std::uint8_t> bgra(4 * 4 * 2, 255);
  std::vector<std::uint8_t> out(nv12_bytes(4, 2));
  from_bgra(bgra.data(), 16, 4, 2, out.data());
  CHECK(out[0] == 235 && out[7] == 235 && out[8] == 128 && out[9] == 128);
  for (std::size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = 0;
    bgra[i + 1] = 0;
    bgra[i + 2] = 255;
  }
  from_bgra(bgra.data(), 16, 4, 2, out.data());
  CHECK(out[0] == red.y && out[8] == red.u && out[9] == red.v);

  // The placeholder's icon: drawn in the middle, a little high, with background left around it
  std::uint32_t pw = 1280;
  std::uint32_t ph = 720;
  std::vector<std::uint8_t> placeholder(pw * ph * 3 / 2);
  fill(placeholder.data(), pw, ph, pw, k_background);
  draw_camera_icon(placeholder.data(), pw, ph, pw);
  auto luma = [&](std::uint32_t x, std::uint32_t y) {
    return placeholder[(std::size_t) y * pw + x];
  };
  std::uint32_t cy = (std::uint32_t) (ph * 0.40);
  CHECK(luma(pw / 2, cy) == k_icon.y);  // the lens
  CHECK(luma(pw / 2 + (std::uint32_t) (ph * 0.22 * 0.28), cy) == k_background.y);  // the ring around it
  CHECK(luma(pw / 2 + (std::uint32_t) (ph * 0.22 * 0.6), cy) == k_icon.y);  // the body
  CHECK(luma(10, 10) == k_background.y);
  CHECK(luma(pw / 2, ph - 20) == k_background.y);  // room for the text below
  std::size_t lit = 0;
  for (std::size_t i = 0; i < (std::size_t) pw * ph; i++) {
    lit += placeholder[i] == k_icon.y;
  }
  CHECK(lit > (std::size_t) pw * ph / 50 && lit < (std::size_t) pw * ph / 5);
  // The chroma of the lens
  CHECK(placeholder[(std::size_t) pw * ph + (cy / 2) * pw + (pw / 2 & ~1u)] == k_icon.u);
}

int main() {
  test_sizes();
  test_start_stop();
  test_frame_gate();
  test_placeholder();
  test_shared_frames();
  test_shared_control();
  test_nv12();
  if (s_Failures == 0) {
    std::printf("camera_policy_test: all passed\n");
    return 0;
  }
  std::printf("camera_policy_test: %d failed\n", s_Failures);
  return 1;
}
