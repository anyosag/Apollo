// Tests for src/fork/mic_policy.h: the jitter buffer between LuzLunar's microphone frames and the
// Steam microphone's clock (start, loss, late and repeated frames, the delay following the network
// and the clocks' drift, new timelines), when the Mac records, and which default recording device
// goes back. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/mic_policy_test.cpp

#include "fork/mic_policy.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mic_policy;
using play_e = slot_t::play_e;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

// A frame whose Opus bytes say which one it is
static bool push(jitter_buffer_t &buffer, std::uint16_t sequence, std::uint32_t time_ms, bool start = false) {
  std::vector<std::uint8_t> opus {static_cast<std::uint8_t>(sequence), static_cast<std::uint8_t>(sequence >> 8), 0xAA};
  luzmic::frame_t frame;
  frame.flags = start ? luzmic::flag_start : 0;
  frame.sequence = sequence;
  frame.time_ms = time_ms;
  frame.opus = opus.data();
  frame.opus_size = opus.size();
  return buffer.push(frame);
}

static bool push_seq(jitter_buffer_t &buffer, std::uint16_t sequence, bool start = false) {
  return push(buffer, sequence, 1000 + sequence * 20u, start);
}

static int which(const std::vector<std::uint8_t> &opus) {
  return opus.size() == 3 ? opus[0] | (opus[1] << 8) : -1;
}

static void testStart() {
  jitter_buffer_t buffer;
  CHECK(buffer.pull().play == play_e::silence);
  CHECK(push_seq(buffer, 10, true));
  CHECK(buffer.pull().play == play_e::silence);
  CHECK(push_seq(buffer, 11));
  CHECK(buffer.pull().play == play_e::silence);
  CHECK(!buffer.playing());
  CHECK(push_seq(buffer, 12));

  // 60 ms buffered: the first slot resets the decoder and plays the first frame
  auto slot = buffer.pull();
  CHECK(slot.play == play_e::frame);
  CHECK(slot.reset);
  CHECK(which(slot.opus) == 10);
  CHECK(buffer.playing());
  CHECK(buffer.waiting() == 2);

  slot = buffer.pull();
  CHECK(slot.play == play_e::frame && !slot.reset && which(slot.opus) == 11);

  auto stats = buffer.take_stats();
  CHECK(stats.frames == 2);
  CHECK(stats.restarts == 1);
}

static void testSteady() {
  jitter_buffer_t buffer;
  push_seq(buffer, 0);
  push_seq(buffer, 1);
  for (int i = 2; i < 500; ++i) {
    push_seq(buffer, i);
    auto slot = buffer.pull();
    CHECK(slot.play == play_e::frame);
    CHECK(which(slot.opus) == i - 2);
    CHECK(slot.skipped.empty());
  }
  auto stats = buffer.take_stats();
  CHECK(stats.frames == 498);
  CHECK(stats.dropped == 0 && stats.waits == 0 && stats.concealed == 0 && stats.fec == 0);
}

static void testLoss() {
  jitter_buffer_t buffer;
  // 3 is lost: its slot is rebuilt from 4's redundancy, then 4 plays
  for (int i : {0, 1, 2}) {
    push_seq(buffer, i);
  }
  CHECK(which(buffer.pull().opus) == 0);
  for (int i : {4, 5, 7, 8, 9}) {
    push_seq(buffer, i);
  }
  CHECK(which(buffer.pull().opus) == 1);
  CHECK(which(buffer.pull().opus) == 2);
  auto slot = buffer.pull();
  CHECK(slot.play == play_e::fec);
  CHECK(which(slot.opus) == 4);
  slot = buffer.pull();
  CHECK(slot.play == play_e::frame && which(slot.opus) == 4);
  CHECK(which(buffer.pull().opus) == 5);
  // 6 is lost too
  slot = buffer.pull();
  CHECK(slot.play == play_e::fec && which(slot.opus) == 7);
  CHECK(which(buffer.pull().opus) == 7);

  // Two in a row: the first is concealed, the second rebuilt
  jitter_buffer_t two;
  for (int i : {0, 1, 2}) {
    push_seq(two, i);
  }
  two.pull();
  push_seq(two, 5);
  push_seq(two, 6);
  two.pull();
  two.pull();
  CHECK(two.pull().play == play_e::conceal);
  slot = two.pull();
  CHECK(slot.play == play_e::fec && which(slot.opus) == 5);
  slot = two.pull();
  CHECK(slot.play == play_e::frame && which(slot.opus) == 5);
  auto stats = two.take_stats();
  CHECK(stats.concealed == 1 && stats.fec == 1);
}

static void testLateDuplicateReordered() {
  jitter_buffer_t buffer;
  push_seq(buffer, 0);
  push_seq(buffer, 2);
  push_seq(buffer, 1);  // out of order, still in time
  CHECK(!push_seq(buffer, 2));  // again
  CHECK(which(buffer.pull().opus) == 0);
  CHECK(which(buffer.pull().opus) == 1);
  CHECK(!push_seq(buffer, 0));  // its slot played
  push_seq(buffer, 4);
  CHECK(which(buffer.pull().opus) == 2);
  CHECK(buffer.pull().play == play_e::fec);  // 3 is lost, and 4 rebuilds it
  auto stats = buffer.take_stats();
  CHECK(stats.duplicates == 1);
  CHECK(stats.late == 1);
}

static void testWaitsThenSilence() {
  jitter_buffer_t buffer;
  for (int i = 0; i < 3; ++i) {
    push_seq(buffer, i);
  }
  CHECK(buffer.pull().play == play_e::frame);
  push_seq(buffer, 3);
  for (int i = 0; i < 3; ++i) {
    CHECK(buffer.pull().play == play_e::frame);
  }
  // Nothing more comes: conceal while waiting, then silence
  for (int i = 0; i < k_max_waits; ++i) {
    CHECK(buffer.pull().play == play_e::conceal);
  }
  CHECK(buffer.pull().play == play_e::silence);
  CHECK(!buffer.playing());
  CHECK(buffer.pull().play == play_e::silence);

  // A late frame during the waits plays in its own slot, as nothing was skipped
  jitter_buffer_t late;
  for (int i = 0; i < 3; ++i) {
    push_seq(late, i);
  }
  for (int i = 0; i < 3; ++i) {
    late.pull();
  }
  CHECK(late.pull().play == play_e::conceal);
  CHECK(push_seq(late, 3));
  auto slot = late.pull();
  CHECK(slot.play == play_e::frame && which(slot.opus) == 3);

  // Frames come back: 60 ms again, and the decoder starts over
  push_seq(buffer, 10);
  push_seq(buffer, 11);
  CHECK(buffer.pull().play == play_e::silence);
  push_seq(buffer, 12);
  slot = buffer.pull();
  CHECK(slot.play == play_e::frame && slot.reset && which(slot.opus) == 10);
  auto stats = buffer.take_stats();
  CHECK(stats.underruns == 1);
  CHECK(stats.waits == static_cast<std::uint64_t>(k_max_waits));
}

// One clock against the other: frames_per_100 frames come for every 100 slots, for minutes
static stats_t drift(int frames_per_100, int &max_waiting, int &min_waiting_late) {
  jitter_buffer_t buffer;
  std::uint16_t sequence = 0;
  for (int i = 0; i < 3; ++i) {
    push_seq(buffer, sequence++);
  }
  int owed = 0;
  max_waiting = 0;
  min_waiting_late = 1000;
  for (int slot = 0; slot < 30000; ++slot) {
    owed += frames_per_100;
    while (owed >= 100) {
      owed -= 100;
      push(buffer, sequence, 1000u + sequence * 20u);
      ++sequence;
    }
    buffer.pull();
    max_waiting = std::max(max_waiting, buffer.waiting());
    if (slot > 25000) {
      min_waiting_late = std::min(min_waiting_late, buffer.waiting());
    }
  }
  return buffer.take_stats();
}

static void testDrift() {
  int most = 0;
  int fewest = 0;
  // The Mac's clock 1% fast (far more than real clocks): frames dropped now and then, the delay held
  auto fast = drift(101, most, fewest);
  CHECK(fast.dropped > 0);
  CHECK(most <= k_max_queued + 2);
  CHECK(fast.underruns == 0);
  CHECK(fast.frames > 29000);

  // 1% slow: a slot waits now and then, nothing stops
  auto slow = drift(99, most, fewest);
  CHECK(slow.waits > 0);
  CHECK(slow.underruns == 0);
  CHECK(slow.dropped == 0);
  CHECK(most <= k_max_queued + 2);

  // The same clocks: nothing to correct
  auto same = drift(100, most, fewest);
  CHECK(same.dropped == 0 && same.waits == 0 && same.underruns == 0);
  CHECK(same.frames == 30000);
}

static void testJitterRaisesTheDelay() {
  // Frames come in pairs every other slot: a margin of none half the time, so a slot waits, and
  // after that every frame is in time
  jitter_buffer_t buffer;
  std::uint16_t sequence = 0;
  for (int i = 0; i < 3; ++i) {
    push_seq(buffer, sequence++);
  }
  stats_t total;
  for (int slot = 0; slot < 2000; ++slot) {
    if (slot % 2 == 0) {
      push_seq(buffer, sequence++);
      push_seq(buffer, sequence++);
    }
    buffer.pull();
    if (slot == 999) {
      total = buffer.take_stats();
    }
  }
  auto later = buffer.take_stats();
  CHECK(later.waits == 0);
  CHECK(later.concealed == 0 && later.fec == 0);
  CHECK(later.frames == 1000);
  (void) total;
}

static void testDropKeepsTheDecoderRunning() {
  // A burst leaves 7 waiting for a whole second (the first had the start's 2): the 5 past those 2
  // dropped at once, the last handed over to decode
  jitter_buffer_t buffer;
  std::uint16_t sequence = 0;
  for (int i = 0; i < 3; ++i) {
    push_seq(buffer, sequence++);
  }
  buffer.pull();
  for (int i = 0; i < 5; ++i) {
    push_seq(buffer, sequence++);
  }
  bool skipped = false;
  for (int slot = 0; slot < 110; ++slot) {
    push_seq(buffer, sequence++);
    auto played = buffer.pull();
    if (!played.skipped.empty()) {
      skipped = true;
      CHECK(which(played.skipped) + 1 == which(played.opus));
      CHECK(played.play == play_e::frame);
    }
  }
  CHECK(skipped);
  CHECK(buffer.take_stats().dropped == 5);
  CHECK(buffer.waiting() == k_start_frames - 1);
}

static void testStartFlagAndJumps() {
  jitter_buffer_t buffer;
  for (int i = 0; i < 3; ++i) {
    push_seq(buffer, i);
  }
  buffer.pull();
  push_seq(buffer, 3);
  push_seq(buffer, 4);
  buffer.pull();

  // The client stopped and started recording: a new timeline, whatever was waiting goes
  CHECK(push_seq(buffer, 40, true));
  CHECK(!buffer.playing());
  CHECK(!push_seq(buffer, 5));  // the old timeline's, late
  push_seq(buffer, 41);
  push_seq(buffer, 42);
  auto slot = buffer.pull();
  CHECK(slot.play == play_e::frame && slot.reset && which(slot.opus) == 40);

  // The numbers go on but the time jumped 2 s: a new timeline too
  CHECK(push(buffer, 43, 1000 + 43 * 20 + 2000));
  CHECK(!buffer.playing());
  auto stats = buffer.take_stats();
  CHECK(stats.restarts == 3);
  CHECK(stats.late == 1);

  // The client's clock a little off what the numbers say: the same timeline
  jitter_buffer_t steady;
  push(steady, 0, 0);
  push(steady, 1, 20);
  CHECK(push(steady, 2, 40 + 300));
  CHECK(push(steady, 3, 60 + 330));
  CHECK(push(steady, 5, 100 + 300));
  CHECK(steady.take_stats().restarts == 1);
}

static void testWraparound() {
  jitter_buffer_t buffer;
  auto push_wrapping = [&](std::uint16_t s) {
    // The time wraps too, between 0xFFFF and 0x0000
    return push(buffer, s, static_cast<std::uint32_t>(0xFFFFFFF0u + static_cast<std::uint16_t>(s + 2) * 20u));
  };
  for (std::uint16_t s : {0xFFFE, 0xFFFF, 0x0000}) {
    push_wrapping(s);
  }
  CHECK(which(buffer.pull().opus) == 0xFFFE);
  push_wrapping(0x0001);
  push_wrapping(0x0002);
  CHECK(which(buffer.pull().opus) == 0xFFFF);
  CHECK(which(buffer.pull().opus) == 0x0000);
  CHECK(which(buffer.pull().opus) == 0x0001);
  CHECK(buffer.take_stats().restarts == 1);
}

static void testBurstIsTrimmed() {
  jitter_buffer_t buffer;
  for (int i = 0; i < 3; ++i) {
    push_seq(buffer, i);
  }
  buffer.pull();
  // A stall, then 40 frames at once: never more than 500 ms waiting
  for (int i = 3; i < 43; ++i) {
    push_seq(buffer, i);
    CHECK(buffer.waiting() <= k_max_waiting);
  }
  auto slot = buffer.pull();
  CHECK(slot.play == play_e::frame);
  // and by the end of the next whole second, back to the start's delay
  std::uint16_t sequence = 43;
  for (int i = 0; i < 110; ++i) {
    push_seq(buffer, sequence++);
    buffer.pull();
  }
  CHECK(buffer.waiting() <= k_start_frames);
  CHECK(buffer.take_stats().dropped > 30);

  // Before playing, only the last 60 ms wait
  jitter_buffer_t idle;
  for (int i = 0; i < 10; ++i) {
    push_seq(idle, i);
  }
  slot = idle.pull();
  CHECK(which(slot.opus) == 7);
}

static void testRecording() {
  using namespace std::chrono;
  recording_t recording;
  const auto t0 = steady_clock::time_point {} + hours(1);
  CHECK(!recording.update(false, t0));
  CHECK(recording.update(true, t0 + milliseconds(500)));  // at once
  CHECK(recording.update(false, t0 + milliseconds(1000)));
  CHECK(recording.update(false, t0 + milliseconds(3000)));
  CHECK(recording.update(true, t0 + milliseconds(3200)));  // reopened in time: never stopped
  CHECK(recording.update(false, t0 + milliseconds(6000)));
  CHECK(!recording.update(false, t0 + milliseconds(6200)));  // 3 s after the last
  CHECK(!recording.recording());
  CHECK(!recording.update(false, t0 + milliseconds(9000)));
}

static void testRestoreDefault() {
  const std::wstring steam = L"{0.0.1.00000000}.{steam-mic}";
  const std::wstring builtin = L"{0.0.1.00000000}.{builtin}";
  const std::wstring headset = L"{0.0.1.00000000}.{headset}";
  CHECK(restore_default(builtin, steam, steam) == builtin);
  CHECK(!restore_default(builtin, headset, steam));  // chosen meanwhile
  CHECK(!restore_default(steam, steam, steam));  // it was the Steam microphone already
  CHECK(!restore_default(L"", steam, steam));  // there was none
}

int main() {
  testStart();
  testSteady();
  testLoss();
  testLateDuplicateReordered();
  testWaitsThenSilence();
  testDrift();
  testJitterRaisesTheDelay();
  testDropKeepsTheDecoderRunning();
  testStartFlagAndJumps();
  testWraparound();
  testBurstIsTrimmed();
  testRecording();
  testRestoreDefault();
  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("mic_policy: all checks passed\n");
  return 0;
}
