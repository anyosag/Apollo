// Tests for src/fork/host_sleep_policy.h: when LuzLunar may put this PC to sleep, the single-use
// tokens a request needs, and the JSON it gets. A standalone program, so CI builds it without the
// rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/host_sleep_policy_test.cpp

#include "fork/host_sleep_policy.h"

#include <cstdio>
#include <string>

using namespace host_sleep_policy;
using namespace std::literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void test_blocker() {
  facts_t idle;
  CHECK(blocker(idle, false) == reason_e::none);
  CHECK(available(idle));

  // A stream to anyone always wins, even over a running app and a request to quit it
  facts_t streaming;
  streaming.streaming = true;
  streaming.app_running = true;
  streaming.app = "Desktop";
  CHECK(blocker(streaming, true) == reason_e::streaming);
  CHECK(!available(streaming));

  facts_t updating;
  updating.installing = true;
  CHECK(blocker(updating, true) == reason_e::updating);
  CHECK(!available(updating));

  facts_t sleeping;
  sleeping.sleeping = true;
  CHECK(blocker(sleeping, false) == reason_e::sleeping);

  // An app kept for a resume stops the PC sleeping only until the request says to quit it
  facts_t app;
  app.app_running = true;
  app.app = "Cyberpunk 2077";
  CHECK(blocker(app, false) == reason_e::app_running);
  CHECK(blocker(app, true) == reason_e::none);
  CHECK(available(app));

  CHECK(key(reason_e::none) == "none");
  CHECK(key(reason_e::streaming) == "streaming");
  CHECK(key(reason_e::updating) == "updating");
  CHECK(key(reason_e::sleeping) == "sleeping");
  CHECK(key(reason_e::app_running) == "app_running");
}

static void test_status_json() {
  facts_t idle;
  auto json = nlohmann::json::parse(status_json(idle, "abc123"));
  CHECK(json["available"] == true);
  CHECK(json["reason"] == "none");
  CHECK(json["app"] == "");
  CHECK(json["token"] == "abc123");

  facts_t app;
  app.app_running = true;
  app.app = "Cyberpunk 2077";
  json = nlohmann::json::parse(status_json(app, "abc123"));
  CHECK(json["available"] == true);
  CHECK(json["reason"] == "app_running");
  CHECK(json["app"] == "Cyberpunk 2077");
  CHECK(json["token"] == "abc123");

  // No token while it can't sleep
  facts_t streaming;
  streaming.streaming = true;
  json = nlohmann::json::parse(status_json(streaming, "abc123"));
  CHECK(json["available"] == false);
  CHECK(json["reason"] == "streaming");
  CHECK(!json.contains("token"));

  // An app's name that isn't valid UTF-8 doesn't break the answer
  app.app = "Bad \xff name";
  json = nlohmann::json::parse(status_json(app, "t"), nullptr, false);
  CHECK(!json.is_discarded());

  json = nlohmann::json::parse(accepted_json(true));
  CHECK(json["sleeping"] == true);
  CHECK(json["quitApp"] == true);

  json = nlohmann::json::parse(refusal_json("app_running", "Cyberpunk 2077"));
  CHECK(json["error"] == "app_running");
  CHECK(json["app"] == "Cyberpunk 2077");
  json = nlohmann::json::parse(refusal_json("token"));
  CHECK(json["error"] == "token");
  CHECK(!json.contains("app"));
}

static void test_parse_request() {
  std::string error;
  auto request = parse_request(R"({"token":"abc","quitApp":true})", error);
  CHECK(request && request->token == "abc" && request->quit_app);
  request = parse_request(R"({"token":"abc"})", error);
  CHECK(request && !request->quit_app);

  auto rejected = [](std::string_view body) {
    std::string problem;
    return !parse_request(body, problem) && !problem.empty();
  };
  CHECK(rejected(""));
  CHECK(rejected("[]"));
  CHECK(rejected("not json"));
  CHECK(rejected("{}"));
  CHECK(rejected(R"({"token":""})"));
  CHECK(rejected(R"({"token":5})"));
  CHECK(rejected(R"({"token":")" + std::string(max_token + 1, 'a') + R"("})"));
  CHECK(rejected(R"({"token":"abc","quitApp":"yes"})"));
}

static void test_tokens() {
  auto start = clock::time_point {} + std::chrono::hours(1);
  tokens_t tokens;

  // Good once, within 30 s, for its own client
  tokens.issue("mac", "t1", start);
  CHECK(!tokens.redeem("iphone", "t1", start));
  CHECK(tokens.redeem("mac", "t1", start + 29s));
  CHECK(!tokens.redeem("mac", "t1", start + 29s));

  // Too late
  tokens.issue("mac", "t2", start);
  CHECK(!tokens.redeem("mac", "t2", start + 30s));

  // A wrong guess uses it up
  tokens.issue("mac", "t3", start);
  CHECK(!tokens.redeem("mac", "nope", start));
  CHECK(!tokens.redeem("mac", "t3", start));

  // A new GET replaces the client's previous token
  tokens.issue("mac", "t4", start);
  tokens.issue("mac", "t5", start + 1s);
  CHECK(!tokens.redeem("mac", "t4", start + 2s));
  tokens.issue("mac", "t5", start + 1s);
  CHECK(tokens.redeem("mac", "t5", start + 2s));
  CHECK(!tokens.redeem("mac", "", start + 2s));

  // Expired tokens go when another is issued, and no more than max_tokens are kept
  tokens_t many;
  for (std::size_t i = 0; i < max_tokens + 5; i++) {
    many.issue("client" + std::to_string(i), "t", start + std::chrono::milliseconds(i));
  }
  CHECK(many.size() == max_tokens);
  CHECK(!many.redeem("client0", "t", start + 1s));  // the oldest went first
  CHECK(many.redeem("client" + std::to_string(max_tokens + 4), "t", start + 1s));
  many.issue("late", "t", start + 40s);
  CHECK(many.size() == 1);
}

int main() {
  test_blocker();
  test_status_json();
  test_parse_request();
  test_tokens();

  if (s_Failures == 0) {
    std::printf("host_sleep_policy: all tests passed\n");
    return 0;
  }
  std::printf("host_sleep_policy: %d failure(s)\n", s_Failures);
  return 1;
}
