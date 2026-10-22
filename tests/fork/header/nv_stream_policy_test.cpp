// Tests for src/fork/nv_stream_policy.h: the NVIDIA settings a stream gets, what is remembered and
// put back, the journal, and the virtual display's refresh multiple. A standalone program, so CI
// builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/nv_stream_policy_test.cpp

#include "fork/nv_stream_policy.h"

#include <cstdio>
#include <map>

using namespace nv_stream_policy;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

/// A profile in memory: the settings it sets itself, and the driver's defaults for the rest
struct profile_t {
  std::map<uint32_t, uint32_t> own;
  std::map<uint32_t, uint32_t> defaults {{vsync_id, 0x60925292}};
  std::map<uint32_t, bool> broken;

  std::optional<reading_t> read(uint32_t id) const {
    if (broken.count(id)) {
      return std::nullopt;
    }
    if (auto it = own.find(id); it != own.end()) {
      return reading_t {true, it->second};
    }
    auto it = defaults.find(id);
    return reading_t {false, it == defaults.end() ? 0u : it->second};
  }

  void run(const std::vector<action_t> &actions) {
    for (const auto &a : actions) {
      if (a.kind == action_t::kind_e::clear) {
        own.erase(a.id);
      } else {
        own[a.id] = a.value;
      }
    }
  }

  update_t update(const std::vector<entry_t> &journal, const std::vector<setting_t> &want) {
    auto u = nv_stream_policy::update(journal, want, [this](uint32_t id) {
      return read(id);
    });
    run(u.actions);
    return u;
  }
};

static void test_frame_cap() {
  CHECK(frame_cap(120000, false, smooth_motion_cap_e::stream) == 120);
  CHECK(frame_cap(59940, false, smooth_motion_cap_e::stream) == 60);
  CHECK(frame_cap(119880, false, smooth_motion_cap_e::stream) == 120);
  CHECK(frame_cap(120000, true, smooth_motion_cap_e::stream) == 120);
  CHECK(frame_cap(120000, true, smooth_motion_cap_e::half) == 60);
  CHECK(frame_cap(59940, true, smooth_motion_cap_e::half) == 30);
  CHECK(frame_cap(144000, false, smooth_motion_cap_e::half) == 144);

  // The driver's range
  CHECK(frame_cap(100, false, smooth_motion_cap_e::stream) == 1);
  CHECK(frame_cap(2000000, false, smooth_motion_cap_e::stream) == 1023);
}

static void test_for_stream() {
  std::string note;

  auto want = for_stream({}, 120000, 58000, smooth_motion_cap_e::stream, &note);
  CHECK((want == std::vector<setting_t> {{frame_rate_limit_id, 120}, {vsync_id, vsync_force_off}}));
  CHECK(note.empty());
  CHECK(wants_cap(want));

  // Opted out of the cap
  want = for_stream({false, false}, 120000, 58000, smooth_motion_cap_e::stream, &note);
  CHECK(want.empty());
  CHECK(!wants_cap(want));

  // Smooth Motion
  want = for_stream({true, true}, 120000, 57186, smooth_motion_cap_e::half, &note);
  CHECK((want == std::vector<setting_t> {{frame_rate_limit_id, 60}, {vsync_id, vsync_force_off}, {smooth_motion_id, 1}, {smooth_motion_apis_id, 7}}));
  want = for_stream({false, true}, 120000, 57186, smooth_motion_cap_e::half, &note);
  CHECK((want == std::vector<setting_t> {{smooth_motion_id, 1}, {smooth_motion_apis_id, 7}}));

  // Too old a driver, or an unknown one: no Smooth Motion, and the cap at the stream's rate
  want = for_stream({true, true}, 120000, 57185, smooth_motion_cap_e::half, &note);
  CHECK((want == std::vector<setting_t> {{frame_rate_limit_id, 120}, {vsync_id, vsync_force_off}}));
  CHECK(note.find("571.86") != std::string::npos);
  note.clear();
  want = for_stream({false, true}, 120000, 0, smooth_motion_cap_e::stream, &note);
  CHECK(want.empty());
  CHECK(!note.empty());
}

static void test_app_choice() {
  CHECK(app_from_json(nlohmann::json::parse(R"({"name":"Desktop"})")).cap);
  CHECK(!app_from_json(nlohmann::json::parse(R"({"name":"Desktop"})")).smooth_motion);
  CHECK(!app_from_json(nlohmann::json::parse(R"({"fork-nv-cap":false})")).cap);
  CHECK(!app_from_json(nlohmann::json::parse(R"({"fork-nv-cap":"disabled"})")).cap);
  CHECK(!app_from_json(nlohmann::json::parse(R"({"fork-nv-cap":0})")).cap);
  CHECK(app_from_json(nlohmann::json::parse(R"({"fork-smooth-motion":true})")).smooth_motion);
  CHECK(app_from_json(nlohmann::json::parse(R"({"fork-smooth-motion":"on"})")).smooth_motion);

  // Nonsense keeps the defaults
  CHECK(app_from_json(nlohmann::json::parse(R"({"fork-nv-cap":"maybe","fork-smooth-motion":7})")).cap);
  CHECK(!app_from_json(nlohmann::json::parse(R"({"fork-nv-cap":"maybe","fork-smooth-motion":7})")).smooth_motion);
  CHECK(app_from_json(nlohmann::json::parse(R"([1,2])")).cap);
}

static void test_remember_and_restore() {
  // The profile's own value comes back; the driver's default is put back by clearing
  CHECK((remember(frame_rate_limit_id, 120, {true, 141}) == entry_t {frame_rate_limit_id, 120, 141}));
  CHECK((remember(frame_rate_limit_id, 120, {false, 0}) == entry_t {frame_rate_limit_id, 120, std::nullopt}));
  CHECK((remember(vsync_id, vsync_force_off, {false, 0x60925292}) == entry_t {vsync_id, vsync_force_off, std::nullopt}));

  // An explicit Off for Smooth Motion counts as the default; an explicit On is kept
  CHECK((remember(smooth_motion_id, 1, {true, 0}) == entry_t {smooth_motion_id, 1, std::nullopt}));
  CHECK((remember(smooth_motion_apis_id, 7, {true, 0}) == entry_t {smooth_motion_apis_id, 7, std::nullopt}));
  CHECK((remember(smooth_motion_apis_id, 7, {true, 3}) == entry_t {smooth_motion_apis_id, 7, 3}));
  // Off is a real value for the cap
  CHECK((remember(frame_rate_limit_id, 120, {true, 0}) == entry_t {frame_rate_limit_id, 120, 0}));

  entry_t ours {frame_rate_limit_id, 120, 141};
  CHECK(restore_action(ours, {true, 120}) == restore_e::set);
  CHECK(restore_action({frame_rate_limit_id, 120, std::nullopt}, {true, 120}) == restore_e::clear);
  // Changed during the stream, or cleared: whoever did that keeps it
  CHECK(restore_action(ours, {true, 90}) == restore_e::leave);
  CHECK(restore_action(ours, {false, 0}) == restore_e::leave);
}

static void test_update() {
  profile_t p;
  p.own[frame_rate_limit_id] = 141;  // The user's own cap

  // A stream: both written, the user's cap remembered
  auto want = for_stream({}, 120000, 58000, smooth_motion_cap_e::stream);
  auto u = p.update({}, want);
  CHECK(u.actions.size() == 2);
  CHECK((u.entries == std::vector<entry_t> {{frame_rate_limit_id, 120, 141}, {vsync_id, vsync_force_off, std::nullopt}}));
  CHECK(p.own[frame_rate_limit_id] == 120);
  CHECK(p.own[vsync_id] == vsync_force_off);
  CHECK(!u.unreadable);

  // The same again (the session after the launch): nothing to do, nothing forgotten
  auto again = p.update(u.entries, want);
  CHECK(again.actions.empty());
  CHECK(again.entries == u.entries);

  // A resume at 60 fps: the cap changes, and the user's value is still the one to put back
  auto sixty = p.update(u.entries, for_stream({}, 60000, 58000, smooth_motion_cap_e::stream));
  CHECK((sixty.actions == std::vector<action_t> {{action_t::kind_e::write, frame_rate_limit_id, 60}}));
  CHECK((sixty.entries == std::vector<entry_t> {{frame_rate_limit_id, 60, 141}, {vsync_id, vsync_force_off, std::nullopt}}));

  // The end of the stream: the user's cap back, V-Sync to the driver's default
  auto end = p.update(sixty.entries, {});
  CHECK((end.actions == std::vector<action_t> {{action_t::kind_e::set, frame_rate_limit_id, 141}, {action_t::kind_e::clear, vsync_id, 0}}));
  CHECK(end.entries.empty());
  CHECK(p.own[frame_rate_limit_id] == 141);
  CHECK(!p.own.count(vsync_id));
}

static void test_update_changes() {
  profile_t p;

  // A value that's already what the stream wants isn't remembered, so it's never put back
  p.own[vsync_id] = vsync_force_off;
  auto u = p.update({}, for_stream({}, 120000, 58000, smooth_motion_cap_e::stream));
  CHECK((u.entries == std::vector<entry_t> {{frame_rate_limit_id, 120, std::nullopt}}));
  auto end = p.update(u.entries, {});
  CHECK(p.own[vsync_id] == vsync_force_off);
  CHECK(!p.own.count(frame_rate_limit_id));
  CHECK(end.entries.empty());

  // The user changes the cap during the stream: it stays theirs
  u = p.update({}, for_stream({}, 120000, 58000, smooth_motion_cap_e::stream));
  p.own[frame_rate_limit_id] = 90;
  end = p.update(u.entries, {});
  CHECK(end.actions.empty());
  CHECK(p.own[frame_rate_limit_id] == 90);

  // The next app opts out of the cap and asks for Smooth Motion: the cap goes, Smooth Motion comes
  p.own.clear();
  u = p.update({}, for_stream({}, 120000, 58000, smooth_motion_cap_e::stream));
  auto next = p.update(u.entries, for_stream({false, true}, 120000, 58000, smooth_motion_cap_e::stream));
  CHECK((next.entries == std::vector<entry_t> {{smooth_motion_id, 1, std::nullopt}, {smooth_motion_apis_id, 7, std::nullopt}}));
  CHECK(!p.own.count(frame_rate_limit_id));
  CHECK(!p.own.count(vsync_id));
  CHECK(p.own[smooth_motion_id] == 1);
  CHECK(p.own[smooth_motion_apis_id] == 7);
}

static void test_update_unreadable() {
  profile_t p;
  p.broken[vsync_id] = true;

  // V-Sync can't be read, so it isn't touched or remembered
  auto u = p.update({}, for_stream({}, 120000, 58000, smooth_motion_cap_e::stream));
  CHECK(u.unreadable);
  CHECK((u.entries == std::vector<entry_t> {{frame_rate_limit_id, 120, std::nullopt}}));
  CHECK(!p.own.count(vsync_id));

  // A remembered setting that can't be read at the end stays in the journal for the next try
  p.broken.clear();
  p.own.clear();
  u = p.update({}, for_stream({}, 120000, 58000, smooth_motion_cap_e::stream));
  p.broken[vsync_id] = true;
  auto end = p.update(u.entries, {});
  CHECK(end.unreadable);
  CHECK((end.entries == std::vector<entry_t> {{vsync_id, vsync_force_off, std::nullopt}}));
  CHECK(!p.own.count(frame_rate_limit_id));

  // While it's put back, the journal holds both the old and the new entries
  update_t half {{}, {{smooth_motion_id, 1, std::nullopt}}, false};
  auto saving = while_saving(u.entries, half);
  CHECK(saving.size() == 3);
  CHECK(find(saving, frame_rate_limit_id) && find(saving, vsync_id) && find(saving, smooth_motion_id));
}

static void test_failed_save() {
  // Launched at 60 fps, then the first session at 120: if that change isn't saved (or Apollo stops
  // first), the profile still caps at 60, and the journal must still put that back
  profile_t p;
  auto launch = p.update({}, for_stream({}, 60000, 58000, smooth_motion_cap_e::stream));
  CHECK((launch.entries == std::vector<entry_t> {{frame_rate_limit_id, 60, std::nullopt}, {vsync_id, vsync_force_off, std::nullopt}}));

  profile_t unsaved = p;
  auto session = nv_stream_policy::update(launch.entries, for_stream({}, 120000, 58000, smooth_motion_cap_e::stream), [&](uint32_t id) {
    return unsaved.read(id);
  });
  auto saving = while_saving(launch.entries, session);
  CHECK(saving.size() == 3);
  // Nothing was saved: unsaved still caps at 60, and putting back clears it
  auto end = unsaved.update(saving, {});
  CHECK(!unsaved.own.count(frame_rate_limit_id));
  CHECK(!unsaved.own.count(vsync_id));
  CHECK(end.entries.empty());

  // Saved after all: the 120 entry clears it, the 60 one is left
  p.run(session.actions);
  CHECK(p.own[frame_rate_limit_id] == 120);
  end = p.update(saving, {});
  CHECK(!p.own.count(frame_rate_limit_id));
  CHECK(end.entries.empty());

  // A write that failed: the journal keeps what it knew before
  auto kept = write_failed(session.entries, launch.entries, frame_rate_limit_id);
  CHECK((kept == std::vector<entry_t> {{vsync_id, vsync_force_off, std::nullopt}, {frame_rate_limit_id, 60, std::nullopt}}));
  auto none = write_failed({{smooth_motion_id, 1, std::nullopt}}, {}, smooth_motion_id);
  CHECK(none.empty());
}

static void test_journal() {
  journal_t j {profile_e::global, {{frame_rate_limit_id, 120, 141}, {vsync_id, vsync_force_off, std::nullopt}}};
  auto back = from_json(to_json(j));
  CHECK(back && *back == j);

  journal_t base {profile_e::base, {{smooth_motion_apis_id, 7, 3}}};
  back = from_json(to_json(base));
  CHECK(back && *back == base);

  CHECK(!from_json(""));
  CHECK(!from_json("[]"));
  CHECK(!from_json(R"({"version":2,"profile":"global","settings":[]})"));
  CHECK(!from_json(R"({"version":1,"profile":"other","settings":[]})"));
  CHECK(!from_json(R"({"version":1,"profile":"global","settings":[{"id":-1,"ours":1}]})"));
  CHECK(!from_json(R"({"version":1,"profile":"global","settings":[{"id":4294967296,"ours":1}]})"));
  CHECK(!from_json(R"({"version":1,"profile":"global","settings":[{"id":1}]})"));
  CHECK(!from_json(R"({"version":1,"profile":"global","settings":[{"id":1,"ours":2,"before":"x"}]})"));
  auto empty = from_json(R"({"version":1,"profile":"base","settings":[]})");
  CHECK(empty && empty->profile == profile_e::base && empty->entries.empty());
}

static void test_refresh_multiplier() {
  CHECK(refresh_multiplier(4, true) == 4);
  CHECK(refresh_multiplier(4, false) == 2);
  CHECK(refresh_multiplier(2, true) == 2);
  CHECK(refresh_multiplier(2, false) == 2);
  CHECK(refresh_multiplier(1, true) == 1);
  CHECK(refresh_multiplier(0, true) == 1);
  CHECK(refresh_multiplier(8, true) == 4);
}

int main() {
  test_frame_cap();
  test_for_stream();
  test_app_choice();
  test_remember_and_restore();
  test_update();
  test_update_changes();
  test_update_unreadable();
  test_failed_save();
  test_journal();
  test_refresh_multiplier();

  if (s_Failures) {
    std::printf("%d NVIDIA stream settings checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All NVIDIA stream settings tests passed\n");
  return 0;
}
