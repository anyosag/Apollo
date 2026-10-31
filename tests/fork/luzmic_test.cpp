// Tests for luzmic.h (the Mac's microphone on the PC, shared by anyosag/Apollo and anyosag/LuzLunar).
// Identical in both repos. A standalone program, so CI builds it without either app:
//   g++ -std=c++17 -I<dir with luzmic.h> luzmic_test.cpp

#include "luzmic.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace luzmic;

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

static void testConstants() {
  // What the other repo, the patches and the input handlers count on
  CHECK(k_host_feature_flag == 0x08000000);
  CHECK(k_magic == 0x55004C02);
  CHECK(k_type_state == 0x4C03);
  CHECK(k_type_state >= 0x4C00 && k_type_state <= 0x4CFF);
  CHECK(k_channel == 0x06);
  CHECK(k_frame_samples == 960);
  CHECK(k_max_opus == 112);
  CHECK(k_input_header_size + k_frame_header_size + k_max_opus == k_max_packet);
}

static void testState() {
  state_t state;
  CHECK(!state.recording());
  CHECK(!state.ready());

  state.flags = flag_ready | flag_recording;
  const std::string payload = encode_state(state);
  CHECK(payload.size() == k_state_size);
  CHECK(static_cast<std::uint8_t>(payload[0]) == k_version);
  CHECK(static_cast<std::uint8_t>(payload[1]) == (flag_ready | flag_recording));
  CHECK(payload[2] == 0 && payload[3] == 0);

  state_t back;
  CHECK(decode_state(bytes(payload), payload.size(), back));
  CHECK(back == state);
  CHECK(back.recording() && back.ready());

  // Ready, nobody recording
  state.flags = flag_ready;
  const std::string ready = encode_state(state);
  CHECK(decode_state(bytes(ready), ready.size(), back));
  CHECK(back.ready() && !back.recording());
  CHECK(back != state_t {});

  // Longer payloads are fine (a later field), shorter ones and other versions aren't
  std::string longer = payload + std::string(4, '\x7f');
  CHECK(decode_state(bytes(longer), longer.size(), back));
  CHECK(!decode_state(bytes(payload), k_state_size - 1, back));
  CHECK(!decode_state(nullptr, 0, back));
  std::string other = payload;
  other[0] = 2;
  CHECK(!decode_state(bytes(other), other.size(), back));
}

static void testFrame() {
  std::vector<std::uint8_t> opus(80);
  for (std::size_t i = 0; i < opus.size(); ++i) {
    opus[i] = static_cast<std::uint8_t>(i * 7 + 3);
  }

  frame_t frame;
  frame.flags = flag_start;
  frame.sequence = 0xFFFE;
  frame.time_ms = 0x89ABCDEF;
  frame.opus = opus.data();
  frame.opus_size = opus.size();

  const std::vector<std::uint8_t> packet = encode_frame(frame);
  CHECK(packet.size() == 16 + opus.size());
  // The input header: the big-endian size of what follows the size field, the magic little-endian
  CHECK(packet[0] == 0 && packet[1] == 0 && packet[2] == 0 && packet[3] == packet.size() - 4);
  CHECK(packet[4] == 0x02 && packet[5] == 0x4C && packet[6] == 0x00 && packet[7] == 0x55);
  CHECK(packet[8] == k_version);
  CHECK(packet[9] == flag_start);
  CHECK(packet[10] == 0xFE && packet[11] == 0xFF);
  CHECK(packet[12] == 0xEF && packet[13] == 0xCD && packet[14] == 0xAB && packet[15] == 0x89);
  CHECK(packet[16] == opus[0] && packet.back() == opus.back());

  CHECK(is_frame(packet.data(), packet.size()));
  frame_t back;
  CHECK(decode_frame(packet.data(), packet.size(), back));
  CHECK(back.start());
  CHECK(back.sequence == 0xFFFE);
  CHECK(back.time_ms == 0x89ABCDEF);
  CHECK(back.opus == packet.data() + 16);
  CHECK(back.opus_size == opus.size());

  // The largest fits LiSendForkInputPacket() exactly; one more byte, or none, makes no packet
  std::vector<std::uint8_t> big(k_max_opus + 1, 0x55);
  frame.opus = big.data();
  frame.opus_size = k_max_opus;
  const auto largest = encode_frame(frame);
  CHECK(largest.size() == k_max_packet);
  CHECK(decode_frame(largest.data(), largest.size(), back));
  frame.opus_size = k_max_opus + 1;
  CHECK(encode_frame(frame).empty());
  frame.opus_size = 0;
  CHECK(encode_frame(frame).empty());
  frame.opus = nullptr;
  frame.opus_size = 10;
  CHECK(encode_frame(frame).empty());
}

static void testBadFrames() {
  std::vector<std::uint8_t> opus(40, 0x11);
  frame_t frame;
  frame.sequence = 7;
  frame.opus = opus.data();
  frame.opus_size = opus.size();
  const std::vector<std::uint8_t> good = encode_frame(frame);
  frame_t back;
  CHECK(decode_frame(good.data(), good.size(), back));
  CHECK(!back.start());

  // Another magic: not a frame at all
  auto other = good;
  other[4] = 0x01;
  CHECK(!is_frame(other.data(), other.size()));
  CHECK(!decode_frame(other.data(), other.size(), back));

  // A declared size that isn't what arrived
  auto wrong_size = good;
  wrong_size[3] = static_cast<std::uint8_t>(wrong_size[3] + 1);
  CHECK(is_frame(wrong_size.data(), wrong_size.size()));
  CHECK(!decode_frame(wrong_size.data(), wrong_size.size(), back));

  // Cut short: the declared size no longer matches
  CHECK(!decode_frame(good.data(), good.size() - 1, back));

  // No Opus bytes at all
  std::vector<std::uint8_t> empty(good.begin(), good.begin() + 16);
  empty[3] = 12;
  CHECK(!decode_frame(empty.data(), empty.size(), back));

  // Another version
  auto version = good;
  version[8] = 2;
  CHECK(!decode_frame(version.data(), version.size(), back));

  // Too short for a header, or nothing
  CHECK(!is_frame(good.data(), 7));
  CHECK(!decode_frame(good.data(), 8, back));
  CHECK(!decode_frame(nullptr, 0, back));

  // Bigger than any input packet
  std::vector<std::uint8_t> huge(k_max_packet + 1, 0);
  huge[3] = static_cast<std::uint8_t>(huge.size() - 4);
  huge[4] = 0x02;
  huge[5] = 0x4C;
  huge[6] = 0x00;
  huge[7] = 0x55;
  huge[8] = k_version;
  CHECK(is_frame(huge.data(), huge.size()));
  CHECK(!decode_frame(huge.data(), huge.size(), back));
}

int main() {
  testConstants();
  testState();
  testFrame();
  testBadFrames();
  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("luzmic: all checks passed\n");
  return 0;
}
