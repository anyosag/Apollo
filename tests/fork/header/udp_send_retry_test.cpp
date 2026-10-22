// Tests for src/fork/udp_send_retry.h: how long a send is retried while Windows is out of send
// buffers, which errors send a batch packet by packet, and how often failures are logged. A
// standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/udp_send_retry_test.cpp

#include "fork/udp_send_retry.h"

#include <cstdio>
#include <vector>

using namespace udp_send_retry;
using namespace std::literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

// A clock that only moves when the fake wait (or a send) moves it
struct fake_t {
  clock::time_point time = clock::time_point {} + 1h;
  std::vector<int> answers;  ///< What each send returns; the last one repeats
  std::size_t sends = 0;
  std::size_t waits = 0;
  clock::duration wait_step = 500us;
  int wait_error = 0;

  result_t run() {
    return send(
      [&]() {
        auto answer = answers[std::min(sends, answers.size() - 1)];
        sends++;
        return answer;
      },
      [&](int, clock::duration remaining) {
        waits++;
        CHECK(remaining > clock::duration::zero());
        CHECK(remaining <= budget);
        time += wait_step;
        return wait_error;
      },
      [&]() {
        return time;
      }
    );
  }
};

static void test_send() {
  // Sent the first time
  fake_t first {.answers = {0}};
  auto result = first.run();
  CHECK(result.error == 0 && !result.timed_out && result.retries == 0);
  CHECK(first.sends == 1 && first.waits == 0);

  // Out of buffers twice, then sent
  fake_t pressure_then_sent {.answers = {wsa::no_buffers, wsa::would_block, 0}};
  result = pressure_then_sent.run();
  CHECK(result.error == 0 && result.retries == 2);
  CHECK(pressure_then_sent.sends == 3 && pressure_then_sent.waits == 2);

  // Out of buffers for good: gives up when 2 ms are spent, with no send after the deadline
  fake_t stuck {.answers = {wsa::no_buffers}};
  result = stuck.run();
  CHECK(result.error == wsa::no_buffers && result.timed_out);
  CHECK(stuck.sends == 4);  // at 0, 0.5, 1 and 1.5 ms
  CHECK(stuck.time - (clock::time_point {} + 1h) == budget);

  // A wait that takes the whole budget isn't followed by another send
  fake_t slow {.answers = {wsa::no_buffers}, .wait_step = 5ms};
  result = slow.run();
  CHECK(result.timed_out && slow.sends == 1 && slow.waits == 1);

  // Any other error ends it at once
  fake_t refused {.answers = {wsa::invalid}};
  result = refused.run();
  CHECK(result.error == wsa::invalid && !result.timed_out && refused.waits == 0);

  // So does a failing wait
  fake_t wait_fails {.answers = {wsa::would_block}, .wait_error = wsa::interrupted};
  result = wait_fails.run();
  CHECK(result.error == wsa::interrupted && wait_fails.sends == 1);
}

static void test_batch_outcome() {
  CHECK(batch_outcome(0, true, 1400, false) == batch_e::sent);
  // No segmentation offload: one by one, as before
  CHECK(batch_outcome(wsa::invalid, true, 1400, false) == batch_e::one_by_one);
  CHECK(batch_outcome(wsa::not_supported, true, 1400, true) == batch_e::one_by_one);
  CHECK(batch_outcome(wsa::no_protocol_option, true, 1400, false) == batch_e::one_by_one);
  CHECK(batch_outcome(wsa::message_size, true, 1400, false) == batch_e::one_by_one);
  CHECK(batch_outcome(wsa::message_size, true, 65508, false) == batch_e::dropped);
  CHECK(batch_outcome(wsa::message_size, true, 65527, true) == batch_e::one_by_one);
  CHECK(batch_outcome(wsa::message_size, true, 65528, true) == batch_e::dropped);
  // A single packet isn't segmented, so there's nothing to send one by one
  CHECK(batch_outcome(wsa::invalid, false, 1400, false) == batch_e::dropped);
  // Still out of buffers after the retries: sending each packet again would only meet them again
  CHECK(batch_outcome(wsa::no_buffers, true, 1400, false) == batch_e::dropped);
  CHECK(batch_outcome(wsa::would_block, true, 1400, false) == batch_e::dropped);

  CHECK(pressure(wsa::no_buffers) && pressure(wsa::would_block));
  CHECK(!pressure(0) && !pressure(wsa::invalid) && !pressure(wsa::interrupted));
}

static void test_log_limiter() {
  auto start = clock::time_point {} + 1h;
  log_limiter_t limiter;
  CHECK(limiter.due(start) == 1);  // the first failure is reported
  CHECK(limiter.due(start + 1s) == 0);
  CHECK(limiter.due(start + 4s) == 0);
  CHECK(limiter.due(start + 5s) == 3);  // covers the two held back
  CHECK(limiter.due(start + 6s) == 0);
  CHECK(limiter.due(start + 60s) == 2);
}

int main() {
  test_send();
  test_batch_outcome();
  test_log_limiter();

  if (s_Failures == 0) {
    std::printf("udp_send_retry: all tests passed\n");
    return 0;
  }
  std::printf("udp_send_retry: %d failure(s)\n", s_Failures);
  return 1;
}
