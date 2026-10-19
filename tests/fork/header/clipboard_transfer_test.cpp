// Tests for src/fork/clipboard_transfer.h: a file copy from LuzLunar arriving chunk by chunk. A
// standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/clipboard_transfer_test.cpp

#include "fork/clipboard_transfer.h"

#include <cstdio>

using namespace fork_tweaks::clipboard_transfer;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static clipbundle::file_entry entry(const char *path, std::uint64_t size, bool directory = false) {
  clipbundle::file_entry e;
  e.path = path;
  e.size = size;
  e.directory = directory;
  return e;
}

static void test_in_order() {
  clipbundle::file_manifest m;
  m.entries = {entry("Photos", 0, true), entry("Photos/a.jpg", 10), entry("empty.txt", 0), entry("big.bin", 20)};
  incoming_t in(m, {"Photos", "Photos/a.jpg", "empty.txt", "big.bin"});

  // An empty file is complete from the start; the others aren't
  CHECK(in.file_complete(2));
  CHECK(!in.complete());

  CHECK(in.check(1, 0, 6) == chunk_e::ok);
  in.received(1, 6);
  CHECK(in.check(1, 0, 4) == chunk_e::out_of_order);  // sent again
  CHECK(in.check(1, 8, 2) == chunk_e::out_of_order);  // a chunk skipped
  CHECK(in.check(1, 6, 5) == chunk_e::too_long);
  CHECK(in.check(1, 6, 4) == chunk_e::ok);
  in.received(1, 4);
  CHECK(in.file_complete(1));
  CHECK(in.check(1, 10, 1) == chunk_e::too_long);
  CHECK(in.check(1, 10, 0) == chunk_e::ok);  // nothing past the end is harmless

  CHECK(in.check(3, 0, 20) == chunk_e::ok);
  in.received(3, 20);
  CHECK(in.complete());
}

static void test_refused() {
  clipbundle::file_manifest m;
  m.entries = {entry("dir", 0, true), entry("kept.txt", 3), entry("clash.txt", 3)};
  incoming_t in(m, {"dir", "kept.txt", ""});

  CHECK(in.check(0, 0, 1) == chunk_e::unknown_entry);  // a folder has no bytes
  CHECK(in.check(3, 0, 1) == chunk_e::unknown_entry);
  CHECK(in.check(2, 0, 3) == chunk_e::left_out);
  CHECK(in.left_out() == 1);

  // A file left out doesn't hold the copy back
  in.received(1, 3);
  CHECK(in.complete());

  // Fewer names than entries: the rest are left out
  incoming_t short_names(m, {"dir"});
  CHECK(short_names.check(1, 0, 3) == chunk_e::left_out);
  CHECK(short_names.complete());
}

static void test_numbers() {
  CHECK(chunk_length(100, 0) == 100);
  CHECK(chunk_length(100, 100) == 0);
  CHECK(!chunk_length(100, 101));
  CHECK(chunk_length(std::uint64_t(3) * clipbundle::k_file_chunk_bytes, 5) == clipbundle::k_file_chunk_bytes);
  CHECK(chunk_length(clipbundle::k_file_chunk_bytes + 10, clipbundle::k_file_chunk_bytes) == 10);

  CHECK(parse_number("0") == 0);
  CHECK(parse_number("8388608") == 8388608);
  CHECK(parse_number("123456789012345678") == 123456789012345678ull);
  CHECK(!parse_number(""));
  CHECK(!parse_number("-1"));
  CHECK(!parse_number("1e3"));
  CHECK(!parse_number(" 1"));
  CHECK(!parse_number("1234567890123456789"));  // more digits than it takes
}

int main() {
  test_in_order();
  test_refused();
  test_numbers();

  if (s_Failures > 0) {
    std::printf("clipboard_transfer: %d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("clipboard_transfer: all tests passed\n");
  return 0;
}
