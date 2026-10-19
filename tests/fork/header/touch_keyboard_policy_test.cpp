// Tests for src/fork/touch_keyboard_policy.h: when Windows' touch keyboard comes up for stock Moonlight
// for iOS. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/touch_keyboard_policy_test.cpp

#include "fork/touch_keyboard_policy.h"

#include <cstdio>

using namespace touch_keyboard_policy;
using namespace std::chrono_literals;
using action = policy_t::action_e;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static const clock::time_point t0 {};

static void test_taps() {
  tap_tracker_t t;

  // A finger down and up on the spot
  t.down(0.5f, 0.5f, t0);
  t.move(0.505f, 0.495f);
  CHECK(t.up(t0 + 80ms));

  // A drag scrolls
  t.down(0.5f, 0.5f, t0);
  t.move(0.5f, 0.6f);
  t.move(0.5f, 0.5f);
  CHECK(!t.up(t0 + 300ms));

  // Held too long
  t.down(0.5f, 0.5f, t0);
  CHECK(!t.up(t0 + tap_max + 1ms));

  // A long press is cancelled, and its lift (if one came) is no tap
  t.down(0.5f, 0.5f, t0);
  t.cancel();
  CHECK(!t.up(t0 + 700ms));

  // An up without a down
  tap_tracker_t fresh;
  CHECK(!fresh.up(t0));

  // The next finger starts over
  t.down(0.1f, 0.1f, t0 + 1s);
  CHECK(t.up(t0 + 1s + 50ms));
}

// A field as UI Automation describes it
static field_t field(int control_type, bool has_value = false, bool value_read_only = false, std::optional<bool> text_read_only = std::nullopt, std::string_view class_name = "", std::string_view process = "app.exe") {
  field_t f;
  f.control_type = control_type;
  f.has_value = has_value;
  f.value_read_only = value_read_only;
  f.text_read_only = text_read_only;
  f.class_name = class_name;
  f.process = process;
  return f;
}

static bool editable(const field_t &f) {
  return judge(f).editable == true;
}

static bool not_editable(const field_t &f) {
  return judge(f).editable == false;
}

static bool keeps(const field_t &f) {
  return !judge(f).editable;
}

static void test_fields() {
  // Text boxes: Win32, WPF, WinUI, Chrome's inputs, password boxes
  CHECK(editable(field(control_edit, true, false)));
  CHECK(editable(field(control_edit, true, false, std::nullopt, "Edit")));
  CHECK(editable(field(control_edit)));  // No pattern says otherwise
  CHECK(!needs_text(field(control_edit, true, false)));

  // A read-only one
  CHECK(not_editable(field(control_edit, true, true)));
  CHECK(not_editable(field(control_edit, true, true, true)));
  CHECK(not_editable(field(control_edit, false, false, true)));
  CHECK(needs_text(field(control_edit, true, true)));

  // A rich edit says its value is read-only, but its text isn't
  CHECK(editable(field(control_edit, true, true, false, "RichEditBox")));

  // Disabled
  {
    auto f = field(control_edit, true, false);
    f.enabled = false;
    CHECK(not_editable(f));
  }

  // A browser's page: a read-only document whose value is the address, with the Text and TextEdit patterns
  CHECK(not_editable(field(control_document, true, true, true, "Chrome_RenderWidgetHostHWND")));
  CHECK(not_editable(field(control_document, true, true)));  // Its text mixed or unreadable
  CHECK(not_editable(field(control_document)));
  CHECK(needs_text(field(control_document)));

  // A contenteditable (a group in Chrome), Word's document, a rich edit document
  CHECK(editable(field(control_group, false, false, false)));
  CHECK(editable(field(control_document, false, false, false, "_WwG")));
  CHECK(editable(field(control_document, true, false, std::nullopt, "RichEditD2DPT")));
  CHECK(not_editable(field(control_group)));
  CHECK(not_editable(field(control_pane, true, false)));  // A writable value alone doesn't make a pane text
  CHECK(editable(field(control_custom, false, false, false)));

  // Combo boxes and spinners: only with a value that can be written
  CHECK(editable(field(control_combo_box, true, false)));
  CHECK(not_editable(field(control_combo_box)));
  CHECK(not_editable(field(control_combo_box, true, true)));
  CHECK(editable(field(control_spinner, true, false)));

  // Terminals by class, whatever their control type
  CHECK(editable(field(50020, false, false, std::nullopt, "TermControl")));
  CHECK(editable(field(control_pane, false, false, std::nullopt, "ConsoleWindowClass")));
  CHECK(editable(field(control_pane, false, false, std::nullopt, "CASCADIA_HOSTING_WINDOW_CLASS")));

  // Menus and the keyboard itself leave it as it is
  CHECK(keeps(field(control_menu_item)));
  CHECK(keeps(field(control_menu)));
  CHECK(keeps(field(50000, false, false, std::nullopt, "", "textinputhost.exe")));

  // Anything else: a button, a list item with a value
  CHECK(not_editable(field(50000)));
  CHECK(not_editable(field(50007, true, false)));
  CHECK(!needs_text(field(50000)));

  CHECK(judge(field(control_edit, true, false)).why == "a text box");
  CHECK(judge(field(control_document, true, true, true)).why == "not editable");
}

// Taps a text field and returns what the policy said once the focus settled
static action tap_text(policy_t &p, clock::time_point at, bool visible = false) {
  p.tap(at);
  CHECK(p.next_check() == at + settle);
  return p.focus(true, visible, at + settle);
}

// After a show at `shown`, Windows confirms the keyboard is up
static void confirm(policy_t &p, clock::time_point shown) {
  CHECK(p.focus(true, true, shown + toggle_settle) == action::none);
}

static void test_show_and_hide() {
  // A tap on a text field opens it; the focus isn't looked at before it settles
  {
    policy_t p;
    CHECK(!p.next_check());
    p.tap(t0);
    CHECK(p.focus(true, false, t0 + 100ms) == action::none);
    CHECK(p.focus(true, false, t0 + settle) == action::show);
    CHECK(p.ours(false, t0 + settle + 100ms));  // Windows hasn't caught up yet
  }

  // A tap on something else closes the one it opened
  {
    policy_t p;
    CHECK(tap_text(p, t0) == action::show);
    confirm(p, t0 + settle);
    p.tap(t0 + 3s);
    CHECK(p.focus(false, true, t0 + 3s + settle) == action::hide);
    CHECK(!p.ours(true, t0 + 3s + settle + 100ms));
  }

  // ... even right after opening it, while Windows doesn't report it yet
  {
    policy_t p;
    CHECK(tap_text(p, t0) == action::show);
    p.tap(t0 + settle + 50ms);
    CHECK(p.focus(false, false, t0 + 2 * settle + 50ms) == action::hide);
  }

  // Taps on its own keys keep the focus in the field, so it stays
  {
    policy_t p;
    CHECK(tap_text(p, t0) == action::show);
    confirm(p, t0 + settle);
    for (int i = 1; i <= 5; i++) {
      auto at = t0 + 2s + i * 150ms;
      p.tap(at);
      CHECK(p.focus(true, true, at + settle) == action::none);
    }
  }

  // A second tap while it slides in: Windows still says it's down, but it isn't toggled again
  {
    policy_t p;
    CHECK(tap_text(p, t0) == action::show);
    p.tap(t0 + settle + 50ms);
    CHECK(p.focus(true, false, t0 + 2 * settle + 50ms) == action::none);
  }

  // Already up (Windows or the user opened it): left alone, and a tap elsewhere doesn't close it
  {
    policy_t p;
    CHECK(tap_text(p, t0, true) == action::none);
    CHECK(!p.next_check());
    p.tap(t0 + 1s);
    CHECK(p.focus(false, true, t0 + 1s + settle) == action::none);
  }

  // A tap on something that isn't text with the keyboard down does nothing
  {
    policy_t p;
    p.tap(t0);
    CHECK(p.focus(false, false, t0 + settle) == action::none);
    CHECK(!p.next_check());
  }

  // A focus that can't be read, or a menu, changes nothing
  {
    policy_t p;
    p.tap(t0);
    CHECK(p.focus(std::nullopt, false, t0 + settle) == action::none);
    CHECK(!p.next_check());

    CHECK(tap_text(p, t0 + 1s) == action::show);
    confirm(p, t0 + 1s + settle);
    p.tap(t0 + 3s);
    CHECK(p.focus(std::nullopt, true, t0 + 3s + settle) == action::none);
    CHECK(p.ours(true, t0 + 3s + settle));
  }
}

static void test_lost_toggle() {
  // Windows still doesn't report it up a second after the toggle: toggled once more, and only once
  policy_t p;
  auto shown = t0 + settle;
  CHECK(tap_text(p, t0) == action::show);
  CHECK(p.focus(true, false, shown + poll) == action::none);
  CHECK(p.focus(true, false, shown + 2 * poll) == action::none);
  CHECK(p.focus(true, false, shown + 3 * poll) == action::none);
  CHECK(p.next_check() == shown + toggle_settle);
  CHECK(p.focus(true, false, shown + toggle_settle) == action::show);

  // Still not up: the user closed it, or the toggle doesn't work; no more toggles
  auto again = shown + toggle_settle;
  CHECK(p.focus(true, false, again + toggle_settle) == action::none);
  CHECK(!p.ours(false, again + toggle_settle));
  CHECK(!p.next_check());

  // Where only docked keyboards are seen, Windows isn't asked after a toggle: no second toggle
  policy_t blind(false);
  CHECK(tap_text(blind, t0) == action::show);
  CHECK(blind.focus(true, false, shown + toggle_settle) == action::none);
  CHECK(blind.ours(false, shown + 10s));
  blind.tap(t0 + 10s);
  CHECK(blind.focus(true, false, t0 + 10s + settle) == action::none);  // Believed up, not toggled closed
  blind.tap(t0 + 11s);
  CHECK(blind.focus(false, false, t0 + 11s + settle) == action::hide);
}

static void test_focus_leaves() {
  policy_t p;
  auto shown = t0 + settle;
  CHECK(tap_text(p, t0) == action::show);

  // Polls while it's up
  CHECK(p.next_check() == shown + poll);
  CHECK(p.focus(true, true, shown + poll) == action::none);
  CHECK(p.next_check() == shown + 2 * poll);

  // One look without text isn't enough (focus can pass through something else); two are
  CHECK(p.focus(false, true, shown + 2 * poll) == action::none);
  CHECK(p.focus(true, true, shown + 3 * poll) == action::none);
  CHECK(p.focus(true, true, shown + toggle_settle) == action::none);
  CHECK(p.focus(false, true, shown + 4 * poll) == action::none);
  CHECK(p.focus(std::nullopt, true, shown + 5 * poll) == action::none);
  CHECK(p.focus(false, true, shown + 6 * poll) == action::hide);
  CHECK(!p.next_check());
}

static void test_user_closes_it() {
  policy_t p;
  auto shown = t0 + settle;
  CHECK(tap_text(p, t0) == action::show);
  confirm(p, shown);

  // Windows reports it down: the user closed it, so it's no longer polled
  CHECK(p.focus(true, false, t0 + 2s) == action::none);
  CHECK(!p.ours(false, t0 + 2s));
  CHECK(!p.next_check());

  // Another tap in the field opens it again
  CHECK(tap_text(p, t0 + 3s) == action::show);
}

static void test_typing() {
  // A key closes the keyboard that's up, whoever opened it
  {
    policy_t p;
    CHECK(tap_text(p, t0) == action::show);
    confirm(p, t0 + settle);
    CHECK(p.key(true, t0 + 2s) == action::hide);
    CHECK(!p.next_check());
  }
  {
    policy_t p;
    CHECK(p.key(true, t0) == action::hide);
  }

  // With it down, nothing to do
  {
    policy_t p;
    CHECK(p.key(false, t0) == action::none);
  }

  // Taps don't open it until the client has been quiet for 30 s
  {
    policy_t p;
    CHECK(p.key(false, t0) == action::none);
    CHECK(tap_text(p, t0 + 1s) == action::none);
    CHECK(tap_text(p, t0 + key_quiet - settle - 1ms) == action::none);
    CHECK(tap_text(p, t0 + key_quiet) == action::show);
  }

  // A key between a tap and its look: the look is dropped
  {
    policy_t p;
    p.tap(t0);
    CHECK(p.key(false, t0 + 100ms) == action::none);
    CHECK(!p.next_check());
  }

  // Right after a show, a key closes it even though Windows doesn't report it up yet, and the
  // second-toggle check is dropped
  {
    policy_t p;
    CHECK(tap_text(p, t0) == action::show);
    CHECK(p.key(false, t0 + settle + 100ms) == action::hide);
    CHECK(!p.next_check());
  }
}

int main() {
  test_taps();
  test_fields();
  test_show_and_hide();
  test_lost_toggle();
  test_focus_leaves();
  test_user_closes_it();
  test_typing();

  if (s_Failures) {
    std::printf("%d check(s) failed\n", s_Failures);
    return 1;
  }
  std::printf("touch_keyboard_policy: all checks passed\n");
  return 0;
}
