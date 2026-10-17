// Tests for src/fork/frame_loss.h: the client's loss reports, counted per stream. A standalone
// program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/frame_loss_test.cpp

#include "fork/frame_loss.h"

#include <cstdio>
#include <string>

using namespace frame_loss;
using namespace std::chrono_literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

// A message as moonlight-common-c sends it (big-endian, packed)
static std::string message(uint32_t frame, uint16_t total_data, uint16_t total_parity, uint16_t received_data, uint16_t received_parity, uint8_t fec = 20, uint8_t block = 0, uint8_t blocks = 1) {
  std::string m;
  auto put16 = [&](uint16_t v) {
    m += (char) (v >> 8);
    m += (char) (v & 0xFF);
  };
  m += (char) (frame >> 24);
  m += (char) (frame >> 16);
  m += (char) (frame >> 8);
  m += (char) frame;
  put16(0x1234);  // highest received sequence number
  put16(0x1230);  // next contiguous sequence number
  put16(3);  // missing before the highest received
  put16(total_data);
  put16(total_parity);
  put16(received_data);
  put16(received_parity);
  m += (char) fec;
  m += (char) block;
  m += (char) blocks;
  return m;
}

static fec_status_t status(uint32_t frame, uint16_t total_data, uint16_t total_parity, uint16_t received_data, uint16_t received_parity, uint8_t block = 0, uint8_t blocks = 1) {
  return *parse_fec_status(message(frame, total_data, total_parity, received_data, received_parity, 20, block, blocks));
}

static void test_parse() {
  auto s = parse_fec_status(message(0x01020304, 40, 8, 37, 5, 20, 1, 2));
  CHECK(s.has_value());
  CHECK(s->frame_index == 0x01020304);
  CHECK(s->highest_received_sequence == 0x1234);
  CHECK(s->next_contiguous_sequence == 0x1230);
  CHECK(s->missing_before_highest == 3);
  CHECK(s->total_data == 40);
  CHECK(s->total_parity == 8);
  CHECK(s->received_data == 37);
  CHECK(s->received_parity == 5);
  CHECK(s->fec_percentage == 20);
  CHECK(s->block_index == 1);
  CHECK(s->block_count == 2);

  // Bytes above 0x7F (char is signed on most compilers)
  auto high = parse_fec_status(message(0xFFFFFFF0, 0xFF00, 0x80, 0xFEFF, 0x81, 200));
  CHECK(high.has_value());
  CHECK(high->frame_index == 0xFFFFFFF0);
  CHECK(high->total_data == 0xFF00);
  CHECK(high->received_data == 0xFEFF);
  CHECK(high->fec_percentage == 200);

  // Too short; longer is fine
  CHECK(!parse_fec_status(message(1, 2, 3, 4, 5).substr(0, fec_status_size - 1)).has_value());
  CHECK(!parse_fec_status("").has_value());
  CHECK(parse_fec_status(message(1, 2, 3, 4, 5) + "extra").has_value());
}

static void test_repairable() {
  // 40 data packets, 3 missing, 5 parity received: rebuilt
  CHECK(repairable(status(1, 40, 8, 37, 5)));
  // Exactly enough
  CHECK(repairable(status(1, 40, 8, 37, 3)));
  // One short: dropped
  CHECK(!repairable(status(1, 40, 8, 37, 2)));
  CHECK(!repairable(status(1, 40, 8, 0, 8)));
}

static void test_summary() {
  tracker_t t;
  auto t0 = tracker_t::clock::time_point {} + 100s;

  // Nothing reported: no line, however long
  CHECK(!t.take_summary(t0, 10s).has_value());
  CHECK(!t.take_summary(t0 + 30s, 10s).has_value());

  auto w = t0 + 30s;
  t.add_fec_status(status(100, 40, 8, 38, 4), w + 1s);  // repaired, 2 missing
  t.add_fec_status(status(101, 40, 8, 30, 8), w + 2s);  // lost, 10 missing
  t.add_fec_status(status(102, 20, 4, 19, 4), w + 3s);  // repaired, 1 missing
  t.add_invalidation(w + 2s);
  t.add_keyframe_request(w + 4s);

  // Not yet 10 s since the last line
  CHECK(!t.take_summary(w + 5s, 10s).has_value());

  auto line = t.take_summary(w + 10s, 10s);
  CHECK(line.has_value());
  CHECK(*line == "in the last 10.0 s, 2 frames repaired with FEC, 1 frame lost (13 of 100 data packets missing in those frames), "
                 "1 reference invalidation, 1 keyframe request; FEC 20%");

  // The counts start again
  CHECK(!t.take_summary(w + 20s, 10s).has_value());

  auto total = t.take_total(w + 30s);
  CHECK(total == "over 60.0 s, 2 frames repaired with FEC, 1 frame lost (13 of 100 data packets missing in those frames), "
                 "1 reference invalidation, 1 keyframe request; FEC 20%");
}

static void test_blocks_of_one_frame() {
  tracker_t t;
  auto t0 = tracker_t::clock::time_point {} + 100s;

  // Frame 7 in two blocks, the second lost: one frame, lost
  t.add_fec_status(status(7, 40, 8, 39, 8, 0, 2), t0);
  t.add_fec_status(status(7, 40, 8, 20, 2, 1, 2), t0 + 1ms);
  // Frame 8 in two blocks, both repaired: one frame, repaired
  t.add_fec_status(status(8, 40, 8, 39, 1, 0, 2), t0 + 8ms);
  t.add_fec_status(status(8, 40, 8, 38, 2, 1, 2), t0 + 9ms);

  t.take_total(t0 + 1s);
  CHECK(t.total().frames_lost == 1);
  CHECK(t.total().frames_repaired == 1);
  CHECK(t.total().data_packets == 160);
  CHECK(t.total().data_packets_missing == 1 + 20 + 1 + 2);
}

static void test_clean_total() {
  tracker_t t;
  auto t0 = tracker_t::clock::time_point {} + 100s;

  CHECK(!t.take_summary(t0, 10s).has_value());
  CHECK(t.take_total(t0 + 30min) == "over 30.0 min, 0 frames repaired with FEC, 0 frames lost, 0 reference invalidations, 0 keyframe requests");
}

int main() {
  test_parse();
  test_repairable();
  test_summary();
  test_blocks_of_one_frame();
  test_clean_total();

  if (s_Failures) {
    std::printf("%d failure(s)\n", s_Failures);
    return 1;
  }
  std::printf("frame_loss_test: all passed\n");
  return 0;
}
