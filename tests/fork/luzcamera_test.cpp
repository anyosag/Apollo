// Tests for luzcamera.h (the Mac's camera on the PC, shared by anyosag/Apollo and anyosag/LuzLunar).
// Identical in both repos. A standalone program, so CI builds it without either app:
//   g++ -std=c++17 -I<dir with luzcamera.h> luzcamera_test.cpp

#include "luzcamera.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace luzcamera;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

// Reads every whole message out of bytes fed in pieces of the given size
static std::vector<std::vector<std::uint8_t>> readAll(const std::vector<std::uint8_t> &bytes, std::size_t piece, std::vector<message_t> *headers = nullptr, bool *failed = nullptr) {
  reader_t reader;
  std::vector<std::vector<std::uint8_t>> payloads;
  std::size_t at = 0;
  bool error = false;
  while (at < bytes.size() && !error) {
    std::size_t n = std::min(piece, bytes.size() - at);
    reader.feed(bytes.data() + at, n);
    at += n;
    message_t m;
    for (;;) {
      reader_t::result_e r = reader.next(m);
      if (r == reader_t::result_e::need_more) {
        break;
      }
      if (r == reader_t::result_e::error) {
        error = true;
        break;
      }
      payloads.emplace_back(m.payload, m.payload + m.size);
      if (headers) {
        headers->push_back(m);
      }
    }
  }
  if (failed) {
    *failed = error;
  }
  return payloads;
}

static void testHeaderLayout() {
  start_t start;
  start.width = 1920;
  start.height = 1080;
  start.fps = 30;
  start.kbps = 5000;
  std::vector<std::uint8_t> bytes = make_start(start);
  CHECK(bytes.size() == k_header_size + k_start_size);
  // u8 type, u8 flags, u16 reserved, u32 size, little-endian
  CHECK(bytes[0] == 0x01);
  CHECK(bytes[1] == 0);
  CHECK(bytes[2] == 0 && bytes[3] == 0);
  CHECK(bytes[4] == 12 && bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0);
  // u16 width, u16 height, u16 fps, u16 reserved, u32 kbps
  CHECK(bytes[8] == 0x80 && bytes[9] == 0x07);
  CHECK(bytes[10] == 0x38 && bytes[11] == 0x04);
  CHECK(bytes[12] == 30 && bytes[13] == 0);
  CHECK(bytes[14] == 0 && bytes[15] == 0);
  CHECK(bytes[16] == 0x88 && bytes[17] == 0x13 && bytes[18] == 0 && bytes[19] == 0);

  CHECK(make_empty(type_e::stop) == std::vector<std::uint8_t>({0x02, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(make_empty(type_e::keyframe) == std::vector<std::uint8_t>({0x03, 0, 0, 0, 0, 0, 0, 0}));
}

static void testRoundTrip() {
  std::vector<std::uint8_t> stream;
  auto add = [&](const std::vector<std::uint8_t> &m) {
    stream.insert(stream.end(), m.begin(), m.end());
  };
  start_t start;
  start.width = 1280;
  start.height = 720;
  start.fps = 30;
  start.kbps = 2500;
  add(make_start(start));
  add(make_bitrate(1800));
  add(make_hello("Galvez\xE2\x80\x99s MacBook Pro"));
  add(make_status(status_e::denied, "Camera access is off"));
  add(make_ping(0x0102030405060708ull));

  std::vector<std::uint8_t> au = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 0, 1, 0x68, 3, 0, 0, 1, 0x65, 9, 9, 9};
  video_t video;
  video.keyframe = true;
  video.time_us = 123456789012ull;
  video.number = 42;
  video.width = 1280;
  video.height = 720;
  add(make_video(video, au.data(), au.size()));
  add(make_empty(type_e::stop));

  // Whole, a byte at a time, and in odd pieces
  for (std::size_t piece : {stream.size(), std::size_t(1), std::size_t(7), std::size_t(13)}) {
    std::vector<message_t> headers;
    bool failed = true;
    std::vector<std::vector<std::uint8_t>> payloads = readAll(stream, piece, &headers, &failed);
    CHECK(!failed);
    CHECK(payloads.size() == 7);
    if (payloads.size() != 7) {
      continue;
    }

    message_t m;
    m.header = headers[0].header;
    m.payload = payloads[0].data();
    m.size = payloads[0].size();
    start_t s;
    CHECK(m.type() == type_e::start);
    CHECK(parse_start(m, s));
    CHECK(s.width == 1280 && s.height == 720 && s.fps == 30 && s.kbps == 2500);

    m.header = headers[1].header;
    m.payload = payloads[1].data();
    m.size = payloads[1].size();
    std::uint32_t kbps = 0;
    CHECK(m.type() == type_e::bitrate);
    CHECK(parse_bitrate(m, kbps) && kbps == 1800);

    m.header = headers[2].header;
    m.payload = payloads[2].data();
    m.size = payloads[2].size();
    hello_t hello;
    CHECK(m.type() == type_e::hello);
    CHECK(parse_hello(m, hello));
    CHECK(hello.version == k_version);
    CHECK(hello.name == "Galvez\xE2\x80\x99s MacBook Pro");

    m.header = headers[3].header;
    m.payload = payloads[3].data();
    m.size = payloads[3].size();
    status_t status;
    CHECK(m.type() == type_e::status);
    CHECK(parse_status(m, status));
    CHECK(status.state == status_e::denied);
    CHECK(status.detail == "Camera access is off");

    m.header = headers[4].header;
    CHECK(m.type() == type_e::ping);
    CHECK(payloads[4].size() == 8 && payloads[4][0] == 0x08 && payloads[4][7] == 0x01);

    m.header = headers[5].header;
    m.payload = payloads[5].data();
    m.size = payloads[5].size();
    video_t v;
    CHECK(m.type() == type_e::video);
    CHECK(parse_video(m, v));
    CHECK(v.keyframe);
    CHECK(v.time_us == 123456789012ull);
    CHECK(v.number == 42);
    CHECK(v.width == 1280 && v.height == 720);
    CHECK(v.size == au.size());
    CHECK(std::vector<std::uint8_t>(v.data, v.data + v.size) == au);

    CHECK(headers[6].type() == type_e::stop);
  }
}

static void testLimits() {
  // A control message above its limit breaks the connection
  std::vector<std::uint8_t> big = {0x83, 0, 0, 0, 0, 0, 0, 0};
  std::uint32_t size = k_max_control_payload + 1;
  big[4] = (std::uint8_t) size;
  big[5] = (std::uint8_t) (size >> 8);
  big.resize(big.size() + size);
  bool failed = false;
  readAll(big, big.size(), nullptr, &failed);
  CHECK(failed);

  // VIDEO may be larger, up to its own limit, which is checked from the header alone
  std::vector<std::uint8_t> header = {0x82, 0, 0, 0, 0, 0, 0, 0};
  std::uint32_t over = k_max_video_payload + 1;
  for (int i = 0; i < 4; i++) {
    header[4 + i] = (std::uint8_t) (over >> (8 * i));
  }
  reader_t reader;
  reader.feed(header.data(), header.size());
  message_t m;
  CHECK(reader.next(m) == reader_t::result_e::error);
  // and stays broken
  CHECK(reader.next(m) == reader_t::result_e::error);

  // An access unit too large to send isn't made
  video_t video;
  video.width = 1920;
  video.height = 1080;
  std::vector<std::uint8_t> huge(k_max_video_payload);
  CHECK(make_video(video, huge.data(), huge.size()).empty());
  std::vector<std::uint8_t> fits(k_max_video_payload - k_video_prefix);
  CHECK(make_video(video, fits.data(), fits.size()).size() == k_header_size + k_max_video_payload);

  // Names and details are cut, but never inside a UTF-8 character
  std::string longName;
  for (int i = 0; i < 40; i++) {
    longName += "\xC3\xA9";  // é, 2 bytes
  }
  std::vector<std::uint8_t> hello = make_hello(longName);
  std::vector<std::vector<std::uint8_t>> payloads = readAll(hello, hello.size());
  CHECK(payloads.size() == 1);
  if (payloads.size() == 1) {
    CHECK(payloads[0].size() == 4 + k_max_name);
  }
  std::string three;
  for (int i = 0; i < 100; i++) {
    three += "\xE2\x80\x99";  // ’, 3 bytes
  }
  std::vector<std::uint8_t> status = make_status(status_e::unavailable, three);
  payloads = readAll(status, status.size());
  CHECK(payloads.size() == 1);
  if (payloads.size() == 1) {
    // 200 isn't a multiple of 3: the cut goes back to 198
    CHECK(payloads[0].size() == 1 + 198);
  }
}

static void testUnknownAndShort() {
  // An unknown type within the control limit is read like any other, for the caller to skip
  std::vector<std::uint8_t> stream = {0x7E, 0, 0, 0, 2, 0, 0, 0, 0xAA, 0xBB};
  std::vector<std::uint8_t> stop = make_empty(type_e::stop);
  stream.insert(stream.end(), stop.begin(), stop.end());
  std::vector<message_t> headers;
  bool failed = true;
  std::vector<std::vector<std::uint8_t>> payloads = readAll(stream, 3, &headers, &failed);
  CHECK(!failed);
  CHECK(payloads.size() == 2);
  if (payloads.size() == 2) {
    CHECK(headers[0].header.type == 0x7E);
    CHECK(payloads[0] == std::vector<std::uint8_t>({0xAA, 0xBB}));
    CHECK(headers[1].type() == type_e::stop);
  }

  // Payloads too short for their fields don't parse
  message_t m;
  std::uint8_t few[3] = {1, 2, 3};
  m.payload = few;
  m.size = sizeof(few);
  start_t start;
  CHECK(!parse_start(m, start));
  video_t video;
  CHECK(!parse_video(m, video));
  std::uint32_t kbps = 0;
  CHECK(!parse_bitrate(m, kbps));
  m.size = 0;
  status_t status;
  CHECK(!parse_status(m, status));
  hello_t hello;
  CHECK(!parse_hello(m, hello));

  // START needs a size and frame rate
  std::uint8_t zero[k_start_size] = {};
  m.payload = zero;
  m.size = sizeof(zero);
  CHECK(!parse_start(m, start));

  // The reader waits for a whole header and a whole payload
  reader_t reader;
  std::vector<std::uint8_t> ping = make_ping(5);
  reader.feed(ping.data(), 5);
  CHECK(reader.next(m) == reader_t::result_e::need_more);
  reader.feed(ping.data() + 5, 6);
  CHECK(reader.next(m) == reader_t::result_e::need_more);
  CHECK(reader.pending() == 11);
  reader.feed(ping.data() + 11, ping.size() - 11);
  CHECK(reader.next(m) == reader_t::result_e::message);
  CHECK(m.type() == type_e::ping);
  CHECK(reader.pending() == 0);
}

static void testNals() {
  // SPS, PPS and an IDR slice, with 4- and 3-byte start codes and a trailing zero byte
  std::vector<std::uint8_t> key = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1F, 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80, 0x00, 0, 0, 1, 0x65, 0x88, 0x84};
  std::vector<int> types;
  std::vector<std::size_t> sizes;
  std::size_t count = for_each_nal(key.data(), key.size(), [&](int type, const std::uint8_t *, std::size_t size) {
    types.push_back(type);
    sizes.push_back(size);
  });
  CHECK(count == 3);
  CHECK(types == std::vector<int>({7, 8, 5}));
  CHECK(sizes == std::vector<std::size_t>({4, 4, 3}));
  CHECK(is_decodable_keyframe(key.data(), key.size()));

  // An IDR without the parameter sets before it isn't one the decoder can start from
  std::vector<std::uint8_t> bare = {0, 0, 0, 1, 0x65, 0x88, 0x84};
  CHECK(!is_decodable_keyframe(bare.data(), bare.size()));
  // Nor is a P frame
  std::vector<std::uint8_t> p = {0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x68, 2, 0, 0, 0, 1, 0x41, 0x9A};
  CHECK(!is_decodable_keyframe(p.data(), p.size()));
  // No start code: nothing
  std::vector<std::uint8_t> junk = {1, 2, 3, 4};
  CHECK(for_each_nal(junk.data(), junk.size(), [](int, const std::uint8_t *, std::size_t) {}) == 0);
  CHECK(for_each_nal(junk.data(), 0, [](int, const std::uint8_t *, std::size_t) {}) == 0);
}

static void testBitrates() {
  CHECK(default_kbps(1920, 1080) == 5000);
  CHECK(default_kbps(1280, 720) == 2500);
  CHECK(default_kbps(640, 480) == 1500);
}

int main() {
  testHeaderLayout();
  testRoundTrip();
  testLimits();
  testUnknownAndShort();
  testNals();
  testBitrates();
  if (s_Failures == 0) {
    std::printf("luzcamera_test: all passed\n");
    return 0;
  }
  std::printf("luzcamera_test: %d failed\n", s_Failures);
  return 1;
}
