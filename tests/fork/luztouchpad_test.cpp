// Tests for luztouchpad.h (trackpad passthrough frames shared by anyosag/Apollo and anyosag/LuzLunar).
// Identical in both repos. A standalone program, so CI builds it without either app:
//   g++ -std=c++17 -I<dir with luztouchpad.h> luztouchpad_test.cpp

#include "luztouchpad.h"

#include <cstdio>
#include <vector>

using namespace luztouchpad;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static frame sample() {
  frame f;
  f.count = 3;
  f.buttons = button_pressed;
  f.flags = 0;
  f.width = 15000;  // 150 mm
  f.height = 9500;
  f.time_ms = 0xFFFFFFF0u;
  f.contacts[0] = {0, phase_down, 0, 0};
  f.contacts[1] = {2, phase_move, 32768, 65535};
  f.contacts[2] = {4, phase_up, 65535, 12345};
  return f;
}

static bool decodes(const std::vector<std::uint8_t> &bytes) {
  frame out;
  return decode(bytes.data(), bytes.size(), out);
}

static std::vector<std::uint8_t> bytes_of(const frame &f) {
  packet p = encode(f);
  return std::vector<std::uint8_t>(p.begin(), p.end());
}

static void test_layout() {
  packet p = encode(sample());
  CHECK(p.size() == 50);
  // Big-endian size of the bytes after the size field
  CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 46);
  // Little-endian magic 0x55004C01
  CHECK(p[4] == 0x01 && p[5] == 0x4C && p[6] == 0x00 && p[7] == 0x55);
  CHECK(p[8] == 1);  // version
  CHECK(p[9] == 3);  // count
  CHECK(p[10] == button_pressed);
  CHECK(p[11] == 0);
  CHECK(p[12] == (15000 & 0xFF) && p[13] == (15000 >> 8));
  CHECK(p[16] == 0xF0 && p[17] == 0xFF && p[18] == 0xFF && p[19] == 0xFF);
  // The second contact
  CHECK(p[26] == 2 && p[27] == phase_move);
  CHECK(p[28] == 0x00 && p[29] == 0x80);
  CHECK(p[30] == 0xFF && p[31] == 0xFF);
  // Contacts past the count are zero
  for (std::size_t i = k_header_size + 3 * k_contact_size; i < k_packet_size; i++) {
    CHECK(p[i] == 0);
  }

  // The two feature flags are separate bits, clear of Moonlight's own (LI_FF_* use the low bits)
  CHECK(k_host_feature_flag == 0x40000000);
  CHECK(k_host_mac_trackpad_flag == 0x20000000);
  CHECK((k_host_feature_flag & k_host_mac_trackpad_flag) == 0);
}

static void test_round_trip() {
  frame in = sample();
  packet p = encode(in);
  frame out;
  CHECK(decode(p.data(), p.size(), out));
  CHECK(out.count == in.count);
  CHECK(out.buttons == in.buttons);
  CHECK(out.flags == in.flags);
  CHECK(out.width == in.width && out.height == in.height);
  CHECK(out.time_ms == in.time_ms);
  for (std::size_t i = 0; i < in.count; i++) {
    CHECK(out.contacts[i].slot == in.contacts[i].slot);
    CHECK(out.contacts[i].phase == in.contacts[i].phase);
    CHECK(out.contacts[i].x == in.contacts[i].x);
    CHECK(out.contacts[i].y == in.contacts[i].y);
  }

  // A cancel-all frame with no contacts
  frame cancel;
  cancel.flags = flag_cancel_all;
  cancel.width = 15000;
  cancel.height = 9500;
  p = encode(cancel);
  CHECK(decode(p.data(), p.size(), out));
  CHECK(out.count == 0 && out.flags == flag_cancel_all);

  // Trailing bytes after the packet are ignored
  std::vector<std::uint8_t> longer = bytes_of(in);
  longer.push_back(0xAA);
  CHECK(decodes(longer));

  // More than 5 contacts are cut to 5 when encoding
  frame many = sample();
  many.count = 9;
  many.contacts[3] = {1, phase_move, 1, 1};
  many.contacts[4] = {3, phase_move, 2, 2};
  p = encode(many);
  CHECK(p[9] == 5);
  CHECK(decode(p.data(), p.size(), out) && out.count == 5);
}

static void test_rejects() {
  std::vector<std::uint8_t> good = bytes_of(sample());
  CHECK(decodes(good));

  frame out;
  CHECK(!decode(nullptr, 50, out));
  CHECK(!decode(good.data(), good.size() - 1, out));

  auto broken = [&](std::size_t index, std::uint8_t value) {
    std::vector<std::uint8_t> b = good;
    b[index] = value;
    return decodes(b);
  };
  CHECK(!broken(3, 45));  // declared size
  CHECK(!broken(0, 1));
  CHECK(!broken(4, 0x02));  // magic
  CHECK(!broken(8, 2));  // version
  CHECK(!broken(9, 6));  // count
  CHECK(!broken(20, 5));  // slot past 4
  CHECK(!broken(26, 0));  // the second contact reusing slot 0
  CHECK(!broken(21, 0));  // phase 0
  CHECK(!broken(21, 5));  // phase past cancel

  // Pad sizes outside 10-400 mm
  frame small = sample();
  small.width = 999;
  CHECK(!decodes(bytes_of(small)));
  small.width = 1000;
  CHECK(decodes(bytes_of(small)));
  frame large = sample();
  large.height = 40001;
  CHECK(!decodes(bytes_of(large)));
  large.height = 40000;
  CHECK(decodes(bytes_of(large)));

  // A malformed contact past the count doesn't matter
  std::vector<std::uint8_t> b = good;
  b[k_header_size + 4 * k_contact_size] = 9;
  CHECK(decodes(b));

  // decode leaves out alone on failure
  frame kept;
  kept.count = 2;
  CHECK(!broken(8, 7));
  std::vector<std::uint8_t> bad = good;
  bad[8] = 7;
  CHECK(!decode(bad.data(), bad.size(), kept));
  CHECK(kept.count == 2);
}

static void test_reliability() {
  frame moves;
  moves.count = 2;
  moves.width = 15000;
  moves.height = 9500;
  moves.contacts[0] = {0, phase_move, 10, 10};
  moves.contacts[1] = {1, phase_move, 20, 20};
  CHECK(!is_reliable(moves, 0));
  CHECK(is_reliable(moves, button_pressed));  // the button was let go

  moves.buttons = button_pressed;
  CHECK(!is_reliable(moves, button_pressed));
  CHECK(is_reliable(moves, 0));  // the button went down
  moves.buttons = 0;

  for (std::uint8_t ph : {phase_down, phase_up, phase_cancel}) {
    frame f = moves;
    f.contacts[1].phase = ph;
    CHECK(is_reliable(f, 0));
  }

  frame cancel = moves;
  cancel.flags = flag_cancel_all;
  CHECK(is_reliable(cancel, 0));

  frame empty;
  CHECK(is_reliable(empty, 0));
}

int main() {
  test_layout();
  test_round_trip();
  test_rejects();
  test_reliability();

  if (s_Failures == 0) {
    std::printf("luztouchpad: all tests passed\n");
    return 0;
  }
  std::printf("luztouchpad: %d failure(s)\n", s_Failures);
  return 1;
}
