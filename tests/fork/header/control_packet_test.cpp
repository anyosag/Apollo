// Tests for src/control_packet.h: control stream messages are split and unwrapped without reading
// past their end. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -Isrc -Ithird-party tests/fork/header/control_packet_test.cpp

#include "control_packet.h"

#include <cstdio>
#include <string>

using namespace stream::control_packet;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static std::string le16(std::uint16_t v) {
  return {static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)};
}

static std::string le32(std::uint32_t v) {
  std::string out;
  for (int i = 0; i < 4; i++) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  return out;
}

static std::string be32(std::uint32_t v) {
  std::string out;
  for (int i = 3; i >= 0; i--) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  return out;
}

static void test_control_packet() {
  // Nothing, or only half a type
  CHECK(!parse_control_packet(nullptr, 0));
  CHECK(!parse_control_packet(nullptr, 10));
  std::string one = "\x01";
  CHECK(!parse_control_packet(one.data(), one.size()));

  // Just a type: an empty payload, not a wrapped-around one
  std::string two = le16(0x0302);
  auto message = parse_control_packet(two.data(), two.size());
  CHECK(message && message->second.empty());
  std::uint16_t expected;
  std::memcpy(&expected, two.data(), 2);
  CHECK(message && message->first == expected);

  std::string full = le16(0x0001) + "payload";
  message = parse_control_packet(full.data(), full.size());
  CHECK(message && message->second == "payload");
}

static void test_encrypted() {
  // The payload after the type: length (seq + tagged ciphertext), seq, then the tagged ciphertext
  std::string cipher(24, 'c');
  std::string good = le16(4 + 24) + le32(7) + cipher;
  auto encrypted = parse_encrypted(good);
  CHECK(encrypted && encrypted->seq == 7 && encrypted->tagged_cipher == cipher);

  // Trailing bytes after the declared length are left out
  std::string trailing = good + "xyz";
  encrypted = parse_encrypted(trailing);
  CHECK(encrypted && encrypted->tagged_cipher == cipher);

  // Too short to hold the length and sequence number
  for (std::size_t size = 0; size < 6; size++) {
    CHECK(!parse_encrypted(good.substr(0, size)));
  }
  // A runt: shorter than the tag and the inner header
  CHECK(!parse_encrypted(le16(min_encrypted_length - 1) + le32(1) + std::string(64, 'c')));
  CHECK(parse_encrypted(le16(min_encrypted_length) + le32(1) + std::string(min_encrypted_length - 4, 'c')));
  // Declaring more than the message holds: the heap over-read this guards against
  CHECK(!parse_encrypted(le16(4 + 25) + le32(1) + cipher));
  CHECK(!parse_encrypted(le16(0xFFFF) + le32(1) + cipher));
}

static void test_legacy_input() {
  // The results are views into the message, so it's kept alive while they're checked
  std::string cipher(40, 'c');
  std::string message = be32(40) + cipher;
  auto legacy = parse_legacy_input(message);
  CHECK(legacy && *legacy == cipher);

  for (std::size_t size = 0; size < 4; size++) {
    CHECK(!parse_legacy_input(std::string(size, '\0')));
  }
  CHECK(!parse_legacy_input(be32(41) + cipher));  // longer than the message
  CHECK(!parse_legacy_input(be32(0x80000000u) + cipher));  // negative
  CHECK(!parse_legacy_input(be32(0xFFFFFFFFu) + cipher));
  std::string empty = be32(0);
  legacy = parse_legacy_input(empty);
  CHECK(legacy && legacy->empty());
}

int main() {
  test_control_packet();
  test_encrypted();
  test_legacy_input();

  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All control packet tests passed\n");
  return 0;
}
