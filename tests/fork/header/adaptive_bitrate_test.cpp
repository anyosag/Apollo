// Tests for src/fork/adaptive_bitrate.h: LuzLunar's bitrate changes while a stream runs. Requests
// convert to the encoder's bitrate exactly as rtsp.cpp converts the launch bitrate, stay between the
// floor and the launch bitrate, and pass from the HTTPS thread to the encode thread. A standalone
// program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/adaptive_bitrate_test.cpp

#include "fork/adaptive_bitrate.h"

#include <cstdio>

using namespace fork_tweaks::adaptive_bitrate;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

// rtsp.cpp's ANNOUNCE, as it is there: the warp multiplication, then the adjustment block
static std::int64_t rtsp_reference(std::int64_t configuredBitrateKbps, std::size_t warp_factor, int fec_percentage, int audioBitrateAdjustment) {
  if (warp_factor >= 2) {
    configuredBitrateKbps *= warp_factor;
  }
  if (configuredBitrateKbps) {
    if (fec_percentage <= 80) {
      configuredBitrateKbps /= 100.f / (100 - fec_percentage);
    }
    configuredBitrateKbps -= std::min((std::int64_t) audioBitrateAdjustment, configuredBitrateKbps / 5);
    configuredBitrateKbps -= std::min((std::int64_t) 500, configuredBitrateKbps / 10);
  }
  return configuredBitrateKbps;
}

static launch_t launch(std::int64_t client_kbps, std::int64_t warp = 1, int fec = 20, std::int64_t audio = 512) {
  launch_t l;
  l.client_kbps = client_kbps;
  l.warp_factor = warp;
  l.fec_percentage = fec;
  l.audio_kbps = audio;
  return l;
}

static void test_conversion() {
  CHECK(audio_kbps(true, 2) == 512);
  CHECK(audio_kbps(false, 2) == 192);
  CHECK(audio_kbps(true, 6) == 1536);
  CHECK(audio_kbps(false, 8) == 768);

  // 50 Mb/s: a fifth to FEC, 512 kbps to audio, 500 to overhead
  CHECK(encoder_kbps(launch(50000), 50000) == 38988);
  // Low bitrates: audio and overhead are capped at a fifth and a tenth
  CHECK(encoder_kbps(launch(50000), 2000) == 1152);
  // FEC above 80% isn't taken off
  CHECK(encoder_kbps(launch(10000, 1, 90), 10000) == 8988);
  // Warp mode multiplies first
  CHECK(encoder_kbps(launch(20000, 4), 20000) == 62988);

  // Exactly what rtsp.cpp computes, float rounding included
  for (int fec : {0, 10, 20, 33, 50, 80, 81, 95}) {
    for (int audio : {192, 512, 1536}) {
      for (std::size_t warp : {1u, 2u, 4u}) {
        for (std::int64_t kbps = 1; kbps <= 300000; kbps += (kbps < 2000 ? 7 : 997)) {
          if (encoder_kbps(launch(kbps, (std::int64_t) warp, fec, audio), kbps) != rtsp_reference(kbps, warp, fec, audio)) {
            std::printf("FAIL: %lld kbps, warp %zu, FEC %d%%, audio %d\n", (long long) kbps, warp, fec, audio);
            s_Failures++;
            return;
          }
        }
      }
    }
  }
}

static void test_limits() {
  CHECK(floor_kbps(launch(20000)) == 1000);
  CHECK(floor_kbps(launch(800)) == 800);
  CHECK(clamp(launch(20000), 500) == 1000);
  CHECK(clamp(launch(20000), 30000) == 20000);
  CHECK(clamp(launch(20000), 12345) == 12345);

  // NVENC's VBV: one frame of the bitrate, plus the increase
  CHECK(vbv_bits(20000, 60, 0) == 333333);
  CHECK(vbv_bits(20000, 60, 10) == 366666);
  CHECK(vbv_bits(20000, 0, 10) == 0);
  CHECK(vbv_bits(0, 60, 0) == 0);
}

static void test_requests() {
  live_t live;
  live.launch = launch(20000);
  const auto launch_encoder = encoder_kbps(live.launch, 20000);

  // Nothing asked yet: the launch bitrate, nothing for the encode thread
  auto now = status(live);
  CHECK(now.target == 20000);
  CHECK(now.encoder == launch_encoder);
  CHECK(now.min == 1000 && now.max == 20000);
  CHECK(now.live && !now.pending);
  CHECK(!take(live));
  CHECK(request(live, 20000) == request_e::unchanged);
  CHECK(request(live, 90000) == request_e::unchanged);  // clamped to the launch bitrate

  // Down: queued for the encode thread, which takes it once
  CHECK(request(live, 10000) == request_e::queued);
  now = status(live);
  CHECK(now.target == 10000 && now.pending && now.encoder == launch_encoder);
  auto kbps = take(live);
  CHECK(kbps && *kbps == encoder_kbps(live.launch, 10000));
  CHECK(!take(live));
  done(live, *kbps, change_e::changed);
  now = status(live);
  CHECK(now.encoder == *kbps && !now.pending && now.live);
  CHECK(request(live, 10000) == request_e::unchanged);
  CHECK(restart_kbps(live, launch_encoder) == *kbps);

  // Two requests before a frame: only the last one counts
  CHECK(request(live, 5000) == request_e::queued);
  CHECK(request(live, 7000) == request_e::queued);
  kbps = take(live);
  CHECK(kbps && *kbps == encoder_kbps(live.launch, 7000));
  done(live, *kbps, change_e::changed);

  // Asking for what runs while a change waits cancels the change
  CHECK(request(live, 3000) == request_e::queued);
  CHECK(request(live, 7000) == request_e::unchanged);
  CHECK(!take(live));
  CHECK(!status(live).pending);

  // Back up to the launch bitrate
  CHECK(request(live, 20000) == request_e::queued);
  kbps = take(live);
  CHECK(kbps && *kbps == launch_encoder);
  done(live, *kbps, change_e::changed);
  CHECK(status(live).encoder == launch_encoder);

  // A request while the encode thread changes to another: compared with the one it changes to,
  // so asking for the bitrate from before isn't lost as "unchanged"
  CHECK(request(live, 9000) == request_e::queued);
  kbps = take(live);
  CHECK(request(live, 20000) == request_e::queued);
  done(live, *kbps, change_e::changed);
  auto again = take(live);
  CHECK(again && *again == launch_encoder);
  done(live, *again, change_e::changed);
  CHECK(status(live).encoder == launch_encoder);

  // One that fails once keeps its bitrate, and the next request is tried
  CHECK(request(live, 9000) == request_e::queued);
  kbps = take(live);
  done(live, *kbps, change_e::failed);
  now = status(live);
  CHECK(now.live && !now.pending && now.encoder == launch_encoder);
  CHECK(request(live, 9000) == request_e::queued);
  kbps = take(live);
  done(live, *kbps, change_e::changed);
  CHECK(status(live).encoder == *kbps);
  CHECK(request(live, 20000) == request_e::queued);
  kbps = take(live);
  done(live, *kbps, change_e::changed);

  // An encoder that can't: refused from then on
  CHECK(request(live, 8000) == request_e::queued);
  kbps = take(live);
  CHECK(kbps.has_value());
  done(live, *kbps, change_e::unsupported);
  now = status(live);
  CHECK(!now.live && !now.pending);
  CHECK(now.encoder == launch_encoder);
  CHECK(request(live, 8000) == request_e::not_live);
  CHECK(!take(live));

  // A stream with no bitrate to change
  live_t none;
  CHECK(request(none, 8000) == request_e::not_live);
  CHECK(!status(none).live);
  CHECK(restart_kbps(none, 12345) == 12345);
}

// A client that launched a quarter above its own bitrate: the encoder is created at its own
static void test_start() {
  live_t live;
  live.launch = launch(25000);
  const auto launch_encoder = encoder_kbps(live.launch, 25000);
  const auto own_encoder = encoder_kbps(live.launch, 20000);

  start(live, 20000);
  auto now = status(live);
  CHECK(now.target == 20000);
  CHECK(now.encoder == own_encoder);
  CHECK(now.max == 25000 && now.live && !now.pending);
  // Nothing for the encode thread: the encoder is created at it
  CHECK(!take(live));
  CHECK(restart_kbps(live, launch_encoder) == own_encoder);
  CHECK(request(live, 20000) == request_e::unchanged);

  // Raised to the launch bitrate later, and no further
  CHECK(request(live, 30000) == request_e::queued);
  auto kbps = take(live);
  CHECK(kbps && *kbps == launch_encoder);
  done(live, *kbps, change_e::changed);
  CHECK(status(live).target == 25000 && status(live).encoder == launch_encoder);
  CHECK(restart_kbps(live, launch_encoder) == launch_encoder);

  // An encoder that can't change its bitrate still starts at the client's own
  live_t fixed;
  fixed.launch = launch(25000);
  start(fixed, 20000);
  CHECK(restart_kbps(fixed, launch_encoder) == own_encoder);
  CHECK(request(fixed, 25000) == request_e::queued);
  kbps = take(fixed);
  done(fixed, *kbps, change_e::unsupported);
  CHECK(!status(fixed).live && status(fixed).encoder == own_encoder && status(fixed).target == 25000);
  CHECK(restart_kbps(fixed, launch_encoder) == own_encoder);

  // Clamped like a request: above the launch bitrate it's the launch bitrate, which needs nothing
  live_t above;
  above.launch = launch(25000);
  start(above, 40000);
  CHECK(status(above).target == 25000 && status(above).encoder == launch_encoder);
  CHECK(restart_kbps(above, launch_encoder) == launch_encoder);
  live_t below;
  below.launch = launch(25000);
  start(below, 200);
  CHECK(status(below).target == 1000);

  // Nothing to start: no launch bitrate, or none asked for
  live_t none;
  start(none, 20000);
  CHECK(status(none).target == 0 && restart_kbps(none, 12345) == 12345);
  live_t unasked;
  unasked.launch = launch(25000);
  start(unasked, 0);
  CHECK(status(unasked).target == 25000 && restart_kbps(unasked, launch_encoder) == launch_encoder);
}

// The encode thread says whether its encoder can change, as soon as it has one
static void test_ready() {
  live_t live;
  live.launch = launch(25000);
  const auto launch_encoder = encoder_kbps(live.launch, 25000);

  // Before an encoder exists, live is a guess
  auto now = status(live);
  CHECK(now.live && !now.known);

  // A request queued before the encoder existed, which turns out not to change: dropped, and
  // refused from then on without any try
  CHECK(request(live, 20000) == request_e::queued);
  encoder_ready(live, false);
  now = status(live);
  CHECK(!now.live && now.known && !now.pending);
  CHECK(!take(live));
  CHECK(request(live, 18000) == request_e::not_live);
  CHECK(now.encoder == launch_encoder);

  // A capture restart's encoder that can: live again
  encoder_ready(live, true);
  now = status(live);
  CHECK(now.live && now.known);
  CHECK(request(live, 18000) == request_e::queued);
  auto kbps = take(live);
  CHECK(kbps.has_value());
  done(live, *kbps, change_e::changed);
  CHECK(status(live).encoder == *kbps);

  // One that can keeps a request queued before it existed
  live_t early;
  early.launch = launch(25000);
  CHECK(request(early, 20000) == request_e::queued);
  encoder_ready(early, true);
  CHECK(status(early).pending && take(early).has_value());

  // A stream with no bitrate to change stays not live
  live_t none;
  encoder_ready(none, true);
  CHECK(!status(none).live && status(none).known);
}

static void test_parse() {
  CHECK(parse_kbps(R"({"kbps":14000})") == 14000);
  CHECK(parse_kbps(R"({"kbps":14000,"other":1})") == 14000);
  CHECK(!parse_kbps(R"({"kbps":0})"));
  CHECK(!parse_kbps(R"({"kbps":-5})"));
  CHECK(!parse_kbps(R"({"kbps":1.5})"));
  CHECK(!parse_kbps(R"({"kbps":"14000"})"));
  CHECK(!parse_kbps(R"({"bitrate":14000})"));
  CHECK(!parse_kbps(R"([14000])"));
  CHECK(!parse_kbps("kbps=14000"));
  CHECK(!parse_kbps(""));

  CHECK(parse_start_kbps("20000") == 20000);
  CHECK(parse_start_kbps("1") == 1);
  CHECK(parse_start_kbps("999999999") == 999999999);
  CHECK(!parse_start_kbps(""));
  CHECK(!parse_start_kbps("0"));
  CHECK(!parse_start_kbps("-20000"));
  CHECK(!parse_start_kbps("+20000"));
  CHECK(!parse_start_kbps("20000kbps"));
  CHECK(!parse_start_kbps(" 20000"));
  CHECK(!parse_start_kbps("20.5"));
  CHECK(!parse_start_kbps("1000000000"));
}

int main() {
  test_conversion();
  test_parse();
  test_limits();
  test_requests();
  test_start();
  test_ready();
  if (s_Failures) {
    std::printf("%d failure(s)\n", s_Failures);
    return 1;
  }
  std::printf("adaptive_bitrate: all tests passed\n");
  return 0;
}
