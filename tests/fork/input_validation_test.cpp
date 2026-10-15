// Tests for src/input_validation.h: input packets are checked against their declared and real sizes
// before any field is read. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -Isrc -Ithird-party tests/fork/input_validation_test.cpp

#include "input_validation.h"

#include <cstdio>
#include <cstring>
#include <vector>

using input::validate_input_packet;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static std::uint32_t to_big(std::uint32_t v) {
  return std::endian::native == std::endian::big ? v : std::byteswap(v);
}

static std::uint32_t to_little(std::uint32_t v) {
  return std::endian::native == std::endian::little ? v : std::byteswap(v);
}

// A packet of total_size bytes whose header declares declared_size and the given type
static std::vector<std::uint8_t> packet(std::uint32_t magic, std::size_t total_size, std::uint32_t declared_size) {
  std::vector<std::uint8_t> bytes(total_size, 0);
  if (total_size >= sizeof(NV_INPUT_HEADER)) {
    NV_INPUT_HEADER header {to_big(declared_size), to_little(magic)};
    std::memcpy(bytes.data(), &header, sizeof(header));
  }
  return bytes;
}

// A well-formed packet of type Packet
template<typename Packet>
static std::vector<std::uint8_t> exact(std::uint32_t magic) {
  return packet(magic, sizeof(Packet), sizeof(Packet) - sizeof(std::uint32_t));
}

template<typename Packet>
static void check_fixed(std::uint32_t magic) {
  constexpr std::uint32_t declared = sizeof(Packet) - sizeof(std::uint32_t);
  CHECK(validate_input_packet(exact<Packet>(magic)));
  // One byte short, with the header claiming the full size
  CHECK(!validate_input_packet(packet(magic, sizeof(Packet) - 1, declared)));
  // Declared one byte short or long
  CHECK(!validate_input_packet(packet(magic, sizeof(Packet), declared - 1)));
  CHECK(!validate_input_packet(packet(magic, sizeof(Packet) + 1, declared + 1)));
  // Just a header
  CHECK(!validate_input_packet(packet(magic, sizeof(NV_INPUT_HEADER), sizeof(std::uint32_t))));
  // Trailing bytes past the declared size are tolerated, as upstream does
  CHECK(validate_input_packet(packet(magic, sizeof(Packet) + 3, declared)));
}

static void test_headers() {
  // Too short for a header, including empty
  for (std::size_t size = 0; size < sizeof(NV_INPUT_HEADER); size++) {
    CHECK(!validate_input_packet(std::vector<std::uint8_t>(size, 0xFF)));
  }

  // The declared size has to cover the type and fit in the packet
  CHECK(!validate_input_packet(packet(0x12345678, 8, 0)));
  CHECK(!validate_input_packet(packet(0x12345678, 8, 3)));
  CHECK(!validate_input_packet(packet(0x12345678, 8, 5)));
  CHECK(!validate_input_packet(packet(0x12345678, 8, 0xFFFFFFFF)));
  CHECK(validate_input_packet(packet(0x12345678, 8, 4)));  // unknown type, just a header

  // The header comes back even for a rejected packet, for the log
  NV_INPUT_HEADER header {};
  CHECK(!validate_input_packet(packet(KEY_DOWN_EVENT_MAGIC, 9, 5), &header));
  CHECK(to_little(header.magic) == KEY_DOWN_EVENT_MAGIC);
}

static void test_fixed_types() {
  check_fixed<NV_REL_MOUSE_MOVE_PACKET>(MOUSE_MOVE_REL_MAGIC_GEN5);
  check_fixed<NV_ABS_MOUSE_MOVE_PACKET>(MOUSE_MOVE_ABS_MAGIC);
  check_fixed<NV_MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
  check_fixed<NV_MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5);
  check_fixed<NV_SCROLL_PACKET>(SCROLL_MAGIC_GEN5);
  check_fixed<SS_HSCROLL_PACKET>(SS_HSCROLL_MAGIC);
  check_fixed<NV_KEYBOARD_PACKET>(KEY_DOWN_EVENT_MAGIC);
  check_fixed<NV_KEYBOARD_PACKET>(KEY_UP_EVENT_MAGIC);
  check_fixed<NV_MULTI_CONTROLLER_PACKET>(MULTI_CONTROLLER_MAGIC_GEN5);
  check_fixed<SS_TOUCH_PACKET>(SS_TOUCH_MAGIC);
  check_fixed<SS_PEN_PACKET>(SS_PEN_MAGIC);
  check_fixed<SS_CONTROLLER_ARRIVAL_PACKET>(SS_CONTROLLER_ARRIVAL_MAGIC);
  check_fixed<SS_CONTROLLER_TOUCH_PACKET>(SS_CONTROLLER_TOUCH_MAGIC);
  check_fixed<SS_CONTROLLER_MOTION_PACKET>(SS_CONTROLLER_MOTION_MAGIC);
  check_fixed<SS_CONTROLLER_BATTERY_PACKET>(SS_CONTROLLER_BATTERY_MAGIC);
}

static void test_unicode() {
  // Text from 0 to the 32-byte maximum, with the size declared and present
  for (std::uint32_t text = 0; text <= UTF8_TEXT_EVENT_MAX_COUNT; text++) {
    CHECK(validate_input_packet(packet(UTF8_TEXT_EVENT_MAGIC, sizeof(NV_INPUT_HEADER) + text, sizeof(std::uint32_t) + text)));
  }
  // Longer than the maximum, even when present
  CHECK(!validate_input_packet(packet(UTF8_TEXT_EVENT_MAGIC, sizeof(NV_INPUT_HEADER) + 33, sizeof(std::uint32_t) + 33)));
  // Declaring more text than the packet holds, the over-read the unicode handler used to do
  CHECK(!validate_input_packet(packet(UTF8_TEXT_EVENT_MAGIC, sizeof(NV_INPUT_HEADER) + 2, sizeof(std::uint32_t) + 20)));
  // Declaring less than the type itself (the handler subtracted and wrapped around)
  CHECK(!validate_input_packet(packet(UTF8_TEXT_EVENT_MAGIC, sizeof(NV_INPUT_HEADER), 2)));
}

int main() {
  test_headers();
  test_fixed_types();
  test_unicode();

  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All input validation tests passed\n");
  return 0;
}
