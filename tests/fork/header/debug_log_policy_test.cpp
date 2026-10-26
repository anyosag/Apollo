// Tests for src/fork/debug_log_policy.h: Apollo's log for LuzLunar's Mac. Level names, the GET
// query, which lines a tail, a search and a follow-up get, and marks. A standalone program, so CI
// builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/debug_log_policy_test.cpp

#include "fork/debug_log_policy.h"

#include <cstdio>
#include <string>

using namespace debug_log_policy;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static get_t request(std::string_view lines, std::string_view grep = {}, std::string_view from = {}) {
  std::string error;
  auto parsed = parse_get(lines, grep, from, error);
  CHECK(parsed.has_value());
  return parsed.value_or(get_t {});
}

static void test_levels() {
  CHECK(parse_level("verbose") == 0);
  CHECK(parse_level("debug") == 1);
  CHECK(parse_level("info") == 2);
  CHECK(parse_level("none") == 6);
  CHECK(!parse_level("Debug"));
  CHECK(!parse_level("configured"));
  CHECK(level_name(1) == "debug");
  CHECK(level_name(9) == "unknown");
}

static void test_query() {
  std::string error;
  CHECK(request("").lines == default_lines);
  CHECK(request("50").lines == 50);
  CHECK(request("999999").lines == max_lines);
  CHECK(request("", "Clipboard").grep == "clipboard");
  CHECK(request("", "", "1234").from == 1234u);
  CHECK(!parse_get("0", "", "", error) && !error.empty());
  CHECK(!parse_get("-1", "", "", error));
  CHECK(!parse_get("", "", "x", error));
  CHECK(!parse_get("", std::string(201, 'a'), "", error));
}

static void test_tail() {
  std::string log = "first\nsecond\r\nthird\nfour";  // "four" is still being written
  auto all = pick(request("10"), log, 0, log.size());
  CHECK((all.lines == std::vector<std::string> {"first", "second", "third"}));
  CHECK(all.next == log.find("four"));
  CHECK(!all.restarted);

  auto last = pick(request("2"), log, 0, log.size());
  CHECK((last.lines == std::vector<std::string> {"second", "third"}));

  // A window that starts mid-line skips that line
  std::string_view window = std::string_view(log).substr(2);
  auto cut = pick(request("10"), window, 2, log.size());
  CHECK((cut.lines == std::vector<std::string> {"second", "third"}));
  CHECK(cut.next == log.find("four"));

  // Searching ignores case and keeps the last matches
  std::string clip = "[a] clipboard: one\n[b] other\n[c] Clipboard: two\n[d] CLIPBOARD three\n";
  auto found = pick(request("2", "clipboard"), clip, 0, clip.size());
  CHECK((found.lines == std::vector<std::string> {"[c] Clipboard: two", "[d] CLIPBOARD three"}));
  CHECK(found.next == clip.size());

  // Where to read: the end of a big log, more of it when searching
  std::uint64_t big = 100ull << 20;
  CHECK(read_start(request(""), big) == big - tail_window);
  CHECK(read_start(request("", "x"), big) == big - grep_window);
  CHECK(read_start(request(""), 10) == 0);
  CHECK(read_length(request(""), big - tail_window, big) == tail_window);
}

static void test_follow() {
  std::string log = "a\nb\nc\nd\n";
  // From an offset at a line's start, its lines; at most `lines`, the rest next time
  auto first = pick(request("2", "", "2"), std::string_view(log).substr(2), 2, log.size());
  CHECK((first.lines == std::vector<std::string> {"b", "c"}));
  CHECK(first.next == 6);
  auto second = pick(request("2", "", "6"), std::string_view(log).substr(6), 6, log.size());
  CHECK((second.lines == std::vector<std::string> {"d"}));
  CHECK(second.next == log.size());

  // Nothing new
  auto none = pick(request("2", "", "8"), "", 8, log.size());
  CHECK(none.lines.empty() && none.next == 8);

  // A half-written line waits
  std::string growing = "a\nhalf";
  auto partial = pick(request("10", "", "2"), std::string_view(growing).substr(2), 2, growing.size());
  CHECK(partial.lines.empty() && partial.next == 2);

  // Past the end: Apollo restarted, so the new log from its start
  std::string fresh = "new\n";
  get_t past = request("10", "", "5000");
  CHECK(read_start(past, fresh.size()) == 0);
  auto restarted = pick(past, fresh, 0, fresh.size());
  CHECK(restarted.restarted);
  CHECK((restarted.lines == std::vector<std::string> {"new"}));

  // A follow-up that searches skips other lines but still moves past them
  std::string mixed = "x1\nkeep\nx2\n";
  auto searched = pick(request("10", "keep", "0"), mixed, 0, mixed.size());
  CHECK((searched.lines == std::vector<std::string> {"keep"}));
  CHECK(searched.next == mixed.size());

  // Follow-ups read a bounded amount at a time
  CHECK(read_length(request("", "", "0"), 0, 100ull << 20) == follow_window);
}

static void test_marks() {
  CHECK(clean_mark("copy test\n1\t2") == "copy test 1 2");
  std::string long_mark(max_mark - 1, 'a');
  long_mark += "\xC3\xA9";  // é across the limit
  CHECK(clean_mark(long_mark) == std::string(max_mark - 1, 'a'));
  CHECK(clean_mark(std::string(500, 'b')).size() == max_mark);
}

int main() {
  test_levels();
  test_query();
  test_tail();
  test_follow();
  test_marks();
  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All debug_log_policy checks passed\n");
  return 0;
}
