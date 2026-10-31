// Tests for draining a safe::event_t (src/thread_safe.h) as the capture thread does before a
// re-init (src/video.cpp): `while (images->pop(0ms)) {}` takes what's there and never waits, even
// when the encoder takes the frame at the same moment (Sunshine #5249). A standalone program, so CI
// builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/event_drain_test.cpp

#include "thread_safe.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

using event_t = safe::event_t<std::shared_ptr<int>>;

// The capture thread's drain, as in video.cpp
static int drain(event_t &images) {
  int drained = 0;
  while (images.pop(0ms)) {
    drained++;
  }
  return drained;
}

// Whether the drain returns within a second. A stuck one is let go with stop(), so the test fails
// rather than hangs (the future waits for its thread when it goes)
static bool drains_in_time(event_t &images) {
  auto done = std::async(std::launch::async, [&images] {
    return drain(images);
  });
  if (done.wait_for(1s) == std::future_status::ready) {
    return true;
  }
  images.stop();
  return false;
}

static void test_empty() {
  event_t images;
  CHECK(drains_in_time(images));
  CHECK(drain(images) == 0);
}

static void test_full() {
  event_t images;
  images.raise(std::make_shared<int>(1));
  CHECK(drain(images) == 1);
  CHECK(!images.peek());
  CHECK(drain(images) == 0);
}

static void test_stopped() {
  event_t images;
  images.raise(std::make_shared<int>(1));
  images.stop();
  CHECK(drains_in_time(images));
  CHECK(drain(images) == 0);
}

// The encoder takes the frame while the capture thread drains: the old `while (peek()) pop();` could
// see it in peek() and then wait in an untimed pop() forever, as nothing else raises a frame
static void test_encoder_takes_it_meanwhile() {
  for (int i = 0; i < 2000; i++) {
    event_t images;
    images.raise(std::make_shared<int>(i));
    std::thread encoder([&images] {
      images.pop(0ms);
    });
    bool returned = drains_in_time(images);
    encoder.join();
    CHECK(returned);
    if (!returned) {
      break;
    }
    CHECK(!images.peek());
  }
}

int main() {
  test_empty();
  test_full();
  test_stopped();
  test_encoder_takes_it_meanwhile();

  if (s_Failures) {
    std::printf("%d failure(s)\n", s_Failures);
    return 1;
  }
  std::printf("event_drain_test: all passed\n");
  return 0;
}
