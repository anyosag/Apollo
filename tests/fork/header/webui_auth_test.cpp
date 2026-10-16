// Tests for the Web UI's password hashing (src/password_hash.h), login throttling
// (src/login_throttle.h) and atomic credentials writes (src/atomic_file.h). A standalone program,
// so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -Isrc -Ithird-party tests/fork/header/webui_auth_test.cpp -lcrypto

#include "atomic_file.h"
#include "login_throttle.h"
#include "password_hash.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

namespace ph = http::password_hash;

static void test_password_hash() {
  const std::string salt = "AbCdEfGhIjKlMnOp";

  // What earlier versions stored: util::hex(crypto::hash(password + salt)), last byte first
  // (computed independently with Python's hashlib)
  const std::string legacy = "552A1A1268C440C127058ACB618A017FA1929C3A56E82F020E89AC2707207DA0";
  CHECK(ph::is_legacy(legacy));
  CHECK(ph::verify(legacy, "hunter2", salt));
  CHECK(!ph::verify(legacy, "hunter3", salt));
  CHECK(!ph::verify(legacy, "hunter2", "other salt"));
  CHECK(!ph::verify("", "", ""));

  // PBKDF2-HMAC-SHA256 (reference from Python's hashlib.pbkdf2_hmac)
  const std::string stored = ph::hash("hunter2", salt, 1000);
  CHECK(stored == "pbkdf2-sha256$1000$BE4B91F99AE91C7C2848168015DD32B8945E947DD52E4BAB9B9521AB6F6D5C9B");
  CHECK(!ph::is_legacy(stored));
  CHECK(ph::verify(stored, "hunter2", salt));
  CHECK(!ph::verify(stored, "hunter2 ", salt));
  CHECK(!ph::verify(stored, "hunter2", "AbCdEfGhIjKlMnOq"));

  // The default cost, round trip
  const std::string strong = ph::hash("correct horse", salt);
  CHECK(strong.starts_with("pbkdf2-sha256$600000$"));
  CHECK(ph::verify(strong, "correct horse", salt));

  // Malformed stored values never verify
  CHECK(!ph::verify("pbkdf2-sha256$", "hunter2", salt));
  CHECK(!ph::verify("pbkdf2-sha256$abc$00", "hunter2", salt));
  CHECK(!ph::verify("pbkdf2-sha256$0$00", "hunter2", salt));
  CHECK(!ph::verify("pbkdf2-sha256$-5$00", "hunter2", salt));
  CHECK(!ph::verify("pbkdf2-sha256$1000", "hunter2", salt));
  CHECK(!ph::verify("pbkdf2-sha256$999999999999$00", "hunter2", salt));
}

static void test_login_throttle() {
  using clock = confighttp::login_throttle_t::clock;
  using namespace std::chrono_literals;
  confighttp::login_throttle_t throttle;
  auto now = clock::now();

  // A few mistakes are free
  for (int i = 0; i < confighttp::login_throttle_t::free_failures; i++) {
    throttle.failed("10.0.0.5", now);
  }
  CHECK(throttle.wait_for("10.0.0.5", now) == clock::duration::zero());

  // Then each failure doubles the wait: 2 s, 4 s, 8 s...
  throttle.failed("10.0.0.5", now);
  CHECK(throttle.wait_for("10.0.0.5", now) == 2s);
  CHECK(throttle.wait_for("10.0.0.5", now + 2s) == clock::duration::zero());
  throttle.failed("10.0.0.5", now);
  CHECK(throttle.wait_for("10.0.0.5", now) == 4s);
  throttle.failed("10.0.0.5", now);
  CHECK(throttle.wait_for("10.0.0.5", now) == 8s);

  // ...up to 5 minutes
  for (int i = 0; i < 40; i++) {
    throttle.failed("10.0.0.5", now);
  }
  CHECK(throttle.wait_for("10.0.0.5", now) == 300s);

  // Other addresses are unaffected, and a success clears the address
  CHECK(throttle.wait_for("10.0.0.6", now) == clock::duration::zero());
  throttle.succeeded("10.0.0.5");
  CHECK(throttle.wait_for("10.0.0.5", now) == clock::duration::zero());

  // Memory is bounded under a flood of addresses
  for (int i = 0; i < 10000; i++) {
    throttle.failed("192.0.2." + std::to_string(i), now);
  }
  CHECK(throttle.wait_for("192.0.2.9999", now) == clock::duration::zero());  // only one failure
}

static std::string read_all(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void test_atomic_file() {
  auto dir = std::filesystem::temp_directory_path() / "apollo_atomic_file_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  auto path = dir / "sunshine_state.json";

  CHECK(file_handler::write_file_atomic(path, "{\"first\": 1}", true));
  CHECK(read_all(path) == "{\"first\": 1}");

  // Replacing an existing file (on Windows this is the MoveFileEx path)
  CHECK(file_handler::write_file_atomic(path, "{\"second\": 2}", true));
  CHECK(read_all(path) == "{\"second\": 2}");
  CHECK(!std::filesystem::exists(dir / "sunshine_state.json.tmp"));

#ifndef _WIN32
  auto perms = std::filesystem::status(path).permissions();
  CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif

  // A directory that doesn't exist fails without leaving anything behind
  CHECK(!file_handler::write_file_atomic(dir / "missing" / "x.json", "x"));

  std::filesystem::remove_all(dir);
}

int main() {
  test_password_hash();
  test_login_throttle();
  test_atomic_file();

  if (s_Failures > 0) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("All Web UI auth tests passed\n");
  return 0;
}
