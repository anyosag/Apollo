// Tests for src/client_args.h: values clients send in /launch, /resume and /pair. A standalone
// program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -Isrc -Ithird-party tests/fork/header/client_args_test.cpp

#include "client_args.h"

#include <cstdio>
#include <string>

using namespace nvhttp::client_args;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void test_rikey() {
  CHECK(valid_rikey("00112233445566778899aabbccddeeff"));
  CHECK(valid_rikey("00112233445566778899AABBCCDDEEFF"));
  CHECK(!valid_rikey(""));
  CHECK(!valid_rikey("00112233445566778899aabbccddee"));  // 15 bytes: AES would read past it
  CHECK(!valid_rikey("00112233445566778899aabbccddeeff00"));  // 17 bytes
  CHECK(!valid_rikey("00112233445566778899aabbccddeegg"));  // not hex
  CHECK(!valid_rikey("00112233 45566778899aabbccddeeff"));
}

static void test_surround_params() {
  CHECK(valid_surround_params(""));
  CHECK(valid_surround_params("62201"));  // 5.1: channels, streams, coupled, mapping
  CHECK(valid_surround_params("853201234567"));
  CHECK(!valid_surround_params("6220\n1"));  // a newline would add lines to the SDP and environment
  CHECK(!valid_surround_params("62201&calc"));
  CHECK(!valid_surround_params(std::string(33, '1')));
}

static void test_device_name() {
  CHECK(sanitize_device_name("Gonzalo's MacBook Pro") == "Gonzalo's MacBook Pro");
  CHECK(sanitize_device_name("Phone (2)") == "Phone [2]");  // as pairing always did
  CHECK(sanitize_device_name("Caf\xC3\xA9 \xF0\x9F\x8C\x99") == "Caf\xC3\xA9 \xF0\x9F\x8C\x99");  // UTF-8 kept
  CHECK(sanitize_device_name("a & b | c > d") == "a   b   c   d");
  CHECK(sanitize_device_name("x %PATH% $HOME `id` ;!") == "x  PATH   HOME  id");
  CHECK(sanitize_device_name("line\r\nbreak\tand\x01") == "line  break and");
  CHECK(sanitize_device_name("   ").empty());
  CHECK(sanitize_device_name("").empty());

  std::string long_name(100, 'a');
  CHECK(sanitize_device_name(long_name).size() == max_device_name_length);
  // A multi-byte character across the cut is dropped whole
  std::string utf8 = std::string(63, 'a') + "\xC3\xA9" + "bb";
  CHECK(sanitize_device_name(utf8) == std::string(63, 'a'));
}

int main() {
  test_rikey();
  test_surround_params();
  test_device_name();

  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All client argument tests passed\n");
  return 0;
}
