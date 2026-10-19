/**
 * @file tools/apollo-touch-keyboard.cpp
 * @brief [fork] Opens Windows' touch keyboard when a tap from stock Moonlight for iOS lands in a
 * text field, and closes it again (src/fork/touch_keyboard.cpp starts it).
 *
 * Apollo starts this as the signed-in user, unelevated, while such a client's fingers are touch, and
 * writes one line per event to its standard input:
 *   tap  a finger tapped
 *   key  the client typed: a hardware keyboard on the iPad, or the iOS keyboard
 * It answers with log lines on standard output, each starting with its level ("info: …"). The end
 * of its input means Apollo is done: a keyboard this opened is closed, and it exits.
 *
 * It runs as the user because the keyboard's process (TabTip.exe) only serves callers of its own
 * user and desktop, and in a process of its own so that UI Automation calls into an app that hangs
 * can't hold up Apollo's input. What to do when is touch_keyboard_policy.h's.
 *
 * The keyboard is driven through interfaces Windows doesn't document, as other tools do:
 * ITipInvocation::Toggle (TabTip's UIHostNoLaunch) flips it; whether it's up comes from the
 * immersive shell's IInputHostManagerBroker, which sees floating keyboards too, or from the
 * keyboard's window, or as a last resort from IFrameworkInputPane, which only sees docked ones.
 *
 * --watch tries it by hand, without a stream: each focus change counts as a tap, and lines typed
 * into the console ("tap", "key") count as Apollo's.
 */
#ifndef WINVER
  #define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
  #define NTDDI_VERSION 0x0A000000
#endif

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// platform includes
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uiautomation.h>

// local includes
#include "src/fork/touch_keyboard_policy.h"

using namespace std::literals;
namespace tkp = touch_keyboard_policy;

namespace {
  // Not in every SDK's headers
  constexpr TEXTATTRIBUTEID is_read_only_attribute = 40015;  // UIA_IsReadOnlyAttributeId

  // UI Automation's classes and interfaces, as uiautomationclient.h has them (declared here so
  // nothing depends on libuuid carrying them)
  constexpr GUID clsid_cuiautomation8 = {0xe22ad333, 0xb25f, 0x460c, {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};
  constexpr GUID clsid_cuiautomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
  constexpr GUID iid_iuiautomation = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
  constexpr GUID iid_iuiautomation2 = {0x34723aff, 0x0c9d, 0x49d0, {0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a}};
  constexpr GUID iid_text_pattern = {0x32eba289, 0x3583, 0x42c9, {0x9c, 0x59, 0x3b, 0x6d, 0x9a, 0x1e, 0x9b, 0x6a}};

  // TabTip's UIHostNoLaunch and ITipInvocation, undocumented
  constexpr GUID clsid_ui_host_no_launch = {0x4ce576fa, 0x83dc, 0x4f88, {0x95, 0x1c, 0x9d, 0x07, 0x82, 0xb4, 0xe3, 0x76}};
  constexpr GUID iid_tip_invocation = {0x37c994e7, 0x432b, 0x4834, {0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b}};

  struct tip_invocation_t: IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Toggle(HWND wnd) = 0;
  };

  // The immersive shell's broker for the input host (the keyboard), undocumented
  constexpr GUID clsid_immersive_shell_broker = {0x228826af, 0x02e1, 0x4226, {0xa9, 0xe0, 0x99, 0xa8, 0x55, 0xe4, 0x55, 0xa6}};
  constexpr GUID iid_immersive_shell_broker = {0x9767060c, 0x9476, 0x42e2, {0x8f, 0x7b, 0x2f, 0x10, 0xfd, 0x13, 0x76, 0x5c}};
  constexpr GUID iid_input_host_manager_broker = {0x2166ee67, 0x71df, 0x4476, {0x83, 0x94, 0x0c, 0xed, 0x2e, 0xd0, 0x52, 0x74}};

  struct input_host_manager_broker_t: IUnknown {
    /// @param mode 0 when it's not up, 2 floating, 3 docked
    virtual HRESULT STDMETHODCALLTYPE GetIhmLocation(RECT *rect, int *mode) = 0;
  };

  struct immersive_shell_broker_t: IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Dummy() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInputHostManagerBroker(input_host_manager_broker_t **broker) = 0;
  };

  // IFrameworkInputPane, documented (shobjidl_core.h), declared here like the rest
  constexpr GUID clsid_framework_input_pane = {0xd5120aa3, 0x46ba, 0x44c5, {0x82, 0x2d, 0xca, 0x80, 0x92, 0xc1, 0xfc, 0x72}};
  constexpr GUID iid_framework_input_pane = {0x5752238b, 0x24f0, 0x495a, {0x82, 0xf1, 0x2f, 0xd5, 0x93, 0x05, 0x67, 0x96}};

  struct framework_input_pane_t: IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Advise(IUnknown *window, IUnknown *handler, DWORD *cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE AdviseWithHWND(HWND wnd, IUnknown *handler, DWORD *cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE Location(RECT *rect) = 0;
  };

  /// Releases a COM interface
  template<class T>
  class com_ptr {
  public:
    com_ptr() = default;
    com_ptr(const com_ptr &) = delete;
    com_ptr &operator=(const com_ptr &) = delete;

    ~com_ptr() {
      reset();
    }

    void reset() {
      if (_p) {
        _p->Release();
        _p = nullptr;
      }
    }

    T **put() {
      reset();
      return &_p;
    }

    void **put_void() {
      return reinterpret_cast<void **>(put());
    }

    T *get() const {
      return _p;
    }

    T *operator->() const {
      return _p;
    }

    explicit operator bool() const {
      return _p != nullptr;
    }

  private:
    T *_p = nullptr;
  };

  std::mutex out_mutex;

  /// One log line for Apollo: "<level>: <message>"
  void say(const char *level, std::string message) {
    // Class names come from apps: no control characters, which could start a line of their own
    for (auto &c : message) {
      if ((unsigned char) c < 0x20 || c == 0x7F) {
        c = '?';
      }
    }
    std::lock_guard lg(out_mutex);
    std::printf("%s: %s\n", level, message.c_str());
    std::fflush(stdout);
  }

  std::string hex(HRESULT hr) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", (unsigned long) hr);
    return text;
  }

  std::string to_utf8(const wchar_t *s, int length = -1) {
    if (!s || length == 0) {
      return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, s, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
      return {};
    }
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, length, out.data(), size, nullptr, nullptr);
    if (length < 0 && !out.empty() && out.back() == '\0') {
      out.pop_back();
    }
    return out;
  }

  /// A process's executable file name, in lowercase; empty if it can't be opened
  std::string process_name(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
      return {};
    }
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    auto ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok) {
      return {};
    }

    std::wstring name {path, size};
    auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      name.erase(0, slash + 1);
    }
    for (auto &c : name) {
      c = towlower(c);
    }
    return to_utf8(name.c_str(), (int) name.size());
  }

  /// The touch keyboard: whether it's up, and the toggle
  class keyboard_t {
  public:
    enum class source_e {
      broker,  ///< IInputHostManagerBroker: docked and floating
      window,  ///< The keyboard's window in TextInputHost.exe: docked and floating
      pane,  ///< IFrameworkInputPane: docked only
    };

    /// Picks how to tell it's up: the first way that works on this PC
    keyboard_t() {
      if (broker_up()) {
        _source = source_e::broker;
      } else if (find_window().found) {
        _source = source_e::window;
      } else {
        _source = source_e::pane;
      }
    }

    source_e source() const {
      return _source;
    }

    const char *source_name() const {
      switch (_source) {
        case source_e::broker:
          return "the input host broker";
        case source_e::window:
          return "the keyboard's window";
        default:
          return "IFrameworkInputPane, which doesn't see a floating keyboard";
      }
    }

    /// Whether what this reports covers a floating keyboard
    bool trusted() const {
      return _source != source_e::pane;
    }

    bool up() {
      switch (_source) {
        case source_e::broker:
          if (auto up = broker_up()) {
            return *up;
          }
          // The shell restarted, or the broker went away: the window still tells
          return find_window().up;
        case source_e::window:
          return find_window().up;
        default:
          return pane_up();
      }
    }

    /**
     * @brief Flips the keyboard, starting TabTip.exe first if it isn't running.
     * @param show What the toggle is for, so starting TabTip, which can open the keyboard by
     * itself, doesn't get toggled closed again.
     * @return Whether it toggled, or TabTip's start opened it.
     */
    bool toggle(bool show) {
      // Two toggles close together can cancel out
      auto since = std::chrono::steady_clock::now() - _last_toggle;
      if (since < 300ms) {
        std::this_thread::sleep_for(300ms - since);
      }

      com_ptr<tip_invocation_t> tip;
      auto hr = CoCreateInstance(clsid_ui_host_no_launch, nullptr, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, iid_tip_invocation, tip.put_void());
      if (hr == REGDB_E_CLASSNOTREG) {
        // UIHostNoLaunch is only there while TabTip.exe runs; without it, there's no keyboard to close
        if (!show || !start_tabtip()) {
          return false;
        }
        if (show && wait_up(1s)) {
          say("debug", "TabTip.exe opened the keyboard as it started");
          _last_toggle = std::chrono::steady_clock::now();
          return true;
        }
        for (int i = 0; i < 20; i++) {
          hr = CoCreateInstance(clsid_ui_host_no_launch, nullptr, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, iid_tip_invocation, tip.put_void());
          if (hr != REGDB_E_CLASSNOTREG) {
            break;
          }
          std::this_thread::sleep_for(250ms);
        }
      }
      if (FAILED(hr) || !tip) {
        say("warning", "couldn't reach the touch keyboard (TabTip's UIHostNoLaunch): " + hex(hr));
        return false;
      }

      hr = tip->Toggle(GetDesktopWindow());
      _last_toggle = std::chrono::steady_clock::now();
      if (FAILED(hr)) {
        say("warning", "the touch keyboard's toggle failed: " + hex(hr));
        return false;
      }
      return true;
    }

  private:
    struct window_t {
      bool found = false;  ///< TextInputHost.exe has a window
      bool up = false;  ///< ... and it's shown, not cloaked, and bigger than a sliver
    };

    std::optional<bool> broker_up() {
      com_ptr<immersive_shell_broker_t> shell;
      auto hr = CoCreateInstance(clsid_immersive_shell_broker, nullptr, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, iid_immersive_shell_broker, shell.put_void());
      if (FAILED(hr) || !shell) {
        return std::nullopt;
      }
      com_ptr<input_host_manager_broker_t> broker;
      hr = shell->GetInputHostManagerBroker(broker.put());
      if (FAILED(hr) || !broker) {
        return std::nullopt;
      }
      RECT rect {};
      int mode = 0;
      hr = broker->GetIhmLocation(&rect, &mode);
      if (FAILED(hr)) {
        return std::nullopt;
      }
      return rect.right > rect.left && rect.bottom > rect.top;
    }

    static BOOL CALLBACK find_window_proc(HWND wnd, LPARAM param) {
      auto &result = *reinterpret_cast<window_t *>(param);

      wchar_t cls[64];
      if (!GetClassNameW(wnd, cls, 64) || std::wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0) {
        return TRUE;
      }
      DWORD pid = 0;
      GetWindowThreadProcessId(wnd, &pid);
      if (process_name(pid) != "textinputhost.exe") {
        return TRUE;
      }
      result.found = true;

      int cloaked = 0;
      DwmGetWindowAttribute(wnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
      RECT rect {};
      GetWindowRect(wnd, &rect);
      if (IsWindowVisible(wnd) && !cloaked && rect.right - rect.left > 50 && rect.bottom - rect.top > 50) {
        result.up = true;
        return FALSE;
      }
      return TRUE;
    }

    window_t find_window() {
      window_t result;
      EnumWindows(find_window_proc, reinterpret_cast<LPARAM>(&result));
      return result;
    }

    bool pane_up() {
      com_ptr<framework_input_pane_t> pane;
      if (FAILED(CoCreateInstance(clsid_framework_input_pane, nullptr, CLSCTX_INPROC_SERVER, iid_framework_input_pane, pane.put_void())) || !pane) {
        return false;
      }
      RECT rect {};
      if (FAILED(pane->Location(&rect))) {
        return false;
      }
      return rect.right > rect.left && rect.bottom > rect.top;
    }

    bool wait_up(std::chrono::milliseconds timeout) {
      auto until = std::chrono::steady_clock::now() + timeout;
      do {
        if (up()) {
          return true;
        }
        std::this_thread::sleep_for(100ms);
      } while (std::chrono::steady_clock::now() < until);
      return false;
    }

    /// Starts TabTip.exe as the shell does. CreateProcess can't: it asks for UI access (error 740)
    bool start_tabtip() {
      wchar_t path[MAX_PATH];
      if (!ExpandEnvironmentStringsW(L"%CommonProgramFiles%\\microsoft shared\\ink\\TabTip.exe", path, MAX_PATH)) {
        say("warning", "couldn't find TabTip.exe: error " + std::to_string(GetLastError()));
        return false;
      }

      SHELLEXECUTEINFOW info {};
      info.cbSize = sizeof(info);
      info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
      info.lpVerb = L"open";
      info.lpFile = path;
      info.nShow = SW_SHOWNORMAL;
      if (!ShellExecuteExW(&info)) {
        say("warning", "couldn't start " + to_utf8(path) + ": error " + std::to_string(GetLastError()));
        return false;
      }
      if (info.hProcess) {
        WaitForInputIdle(info.hProcess, 2000);
        CloseHandle(info.hProcess);
      }
      say("info", "started TabTip.exe, the touch keyboard's process");
      return true;
    }

    source_e _source;
    std::chrono::steady_clock::time_point _last_toggle;
  };

  /// What has the focus, judged
  struct focus_t {
    std::optional<bool> editable;
    std::string why;  ///< For the log: the verdict, the control type, the class and the app
  };

  /// UI Automation, for the focused element
  class automation_t {
  public:
    bool init() {
      com_ptr<IUIAutomation2> uia2;
      auto hr = CoCreateInstance(clsid_cuiautomation8, nullptr, CLSCTX_INPROC_SERVER, iid_iuiautomation2, uia2.put_void());
      if (SUCCEEDED(hr) && uia2) {
        // Apps that don't answer within these are given up on, instead of the default 20 s
        uia2->put_ConnectionTimeout(1000);
        uia2->put_TransactionTimeout(1500);
        hr = uia2->QueryInterface(iid_iuiautomation, _uia.put_void());
      } else {
        hr = CoCreateInstance(clsid_cuiautomation, nullptr, CLSCTX_INPROC_SERVER, iid_iuiautomation, _uia.put_void());
      }
      if (FAILED(hr) || !_uia) {
        say("error", "UI Automation isn't available: " + hex(hr));
        return false;
      }

      hr = _uia->CreateCacheRequest(_cache.put());
      if (FAILED(hr)) {
        say("error", "UI Automation's cache request failed: " + hex(hr));
        return false;
      }
      for (auto property : {UIA_ControlTypePropertyId, UIA_IsEnabledPropertyId, UIA_ClassNamePropertyId, UIA_ProcessIdPropertyId, UIA_IsValuePatternAvailablePropertyId, UIA_ValueIsReadOnlyPropertyId, UIA_IsTextPatternAvailablePropertyId, UIA_NativeWindowHandlePropertyId, UIA_BoundingRectanglePropertyId}) {
        _cache->AddProperty(property);
      }
      return true;
    }

    /// The focused element; nothing if it can't be read
    bool focused(com_ptr<IUIAutomationElement> &element, HRESULT &hr) {
      hr = _uia->GetFocusedElementBuildCache(_cache.get(), element.put());
      return SUCCEEDED(hr) && element;
    }

    /**
     * @brief What tells the focused element apart from the last one, for --watch: its control type,
     * process, class, window and place. Not CompareElements, which some UI Automation
     * implementations answer "different" for each new read of the same element.
     */
    std::string signature(IUIAutomationElement *element) {
      CONTROLTYPEID control_type = 0;
      element->get_CachedControlType(&control_type);
      int pid = 0;
      element->get_CachedProcessId(&pid);
      UIA_HWND wnd = nullptr;
      element->get_CachedNativeWindowHandle(&wnd);
      RECT rect {};
      element->get_CachedBoundingRectangle(&rect);
      std::string text = std::to_string(control_type) + ' ' + std::to_string(pid) + ' ' + std::to_string((std::uintptr_t) wnd) + ' ' +
                         std::to_string(rect.left) + ',' + std::to_string(rect.top) + ',' + std::to_string(rect.right) + ',' + std::to_string(rect.bottom);
      BSTR cls = nullptr;
      if (SUCCEEDED(element->get_CachedClassName(&cls)) && cls) {
        text += ' ' + to_utf8(cls, (int) SysStringLen(cls));
        SysFreeString(cls);
      }
      return text;
    }

    focus_t judge_focus() {
      com_ptr<IUIAutomationElement> element;
      HRESULT hr;
      if (!focused(element, hr)) {
        return {std::nullopt, "focus unreadable (" + hex(hr) + ")"};
      }

      tkp::field_t field;
      CONTROLTYPEID control_type = 0;
      element->get_CachedControlType(&control_type);
      field.control_type = control_type;

      BOOL enabled = TRUE;
      element->get_CachedIsEnabled(&enabled);
      field.enabled = enabled;

      std::string class_name;
      BSTR cls = nullptr;
      if (SUCCEEDED(element->get_CachedClassName(&cls)) && cls) {
        class_name = to_utf8(cls, (int) SysStringLen(cls));
        SysFreeString(cls);
      }
      field.class_name = class_name;

      int pid = 0;
      element->get_CachedProcessId(&pid);
      auto process = process_name((DWORD) pid);
      field.process = process;

      field.has_value = cached_bool(element.get(), UIA_IsValuePatternAvailablePropertyId).value_or(false);
      if (field.has_value) {
        field.value_read_only = cached_bool(element.get(), UIA_ValueIsReadOnlyPropertyId).value_or(false);
      }
      if (tkp::needs_text(field) && cached_bool(element.get(), UIA_IsTextPatternAvailablePropertyId).value_or(false)) {
        field.text_read_only = text_read_only(element.get());
      }

      auto verdict = tkp::judge(field);
      std::string why {verdict.why};
      why += " (control type " + std::to_string(control_type);
      if (!class_name.empty()) {
        why += ", class " + class_name;
      }
      why += " in " + (process.empty() ? "process " + std::to_string(pid) : process) + ')';
      return {verdict.editable, why};
    }

  private:
    static std::optional<bool> cached_bool(IUIAutomationElement *element, PROPERTYID property) {
      VARIANT v;
      VariantInit(&v);
      std::optional<bool> result;
      if (SUCCEEDED(element->GetCachedPropertyValue(property, &v)) && v.vt == VT_BOOL) {
        result = v.boolVal != VARIANT_FALSE;
      }
      VariantClear(&v);
      return result;
    }

    static std::optional<bool> range_read_only(IUIAutomationTextRange *range) {
      VARIANT v;
      VariantInit(&v);
      std::optional<bool> result;
      // Not a bool when it's mixed or not supported
      if (SUCCEEDED(range->GetAttributeValue(is_read_only_attribute, &v)) && v.vt == VT_BOOL) {
        result = v.boolVal != VARIANT_FALSE;
      }
      VariantClear(&v);
      return result;
    }

    /// The Text pattern's IsReadOnly where the caret is, or over the whole text
    static std::optional<bool> text_read_only(IUIAutomationElement *element) {
      com_ptr<IUIAutomationTextPattern> text;
      if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, iid_text_pattern, text.put_void())) || !text) {
        return std::nullopt;
      }

      com_ptr<IUIAutomationTextRangeArray> selection;
      if (SUCCEEDED(text->GetSelection(selection.put())) && selection) {
        int length = 0;
        com_ptr<IUIAutomationTextRange> range;
        if (SUCCEEDED(selection->get_Length(&length)) && length > 0 && SUCCEEDED(selection->GetElement(0, range.put())) && range) {
          if (auto read_only = range_read_only(range.get())) {
            return read_only;
          }
        }
      }

      com_ptr<IUIAutomationTextRange> document;
      if (SUCCEEDED(text->get_DocumentRange(document.put())) && document) {
        return range_read_only(document.get());
      }
      return std::nullopt;
    }

    com_ptr<IUIAutomation> _uia;
    com_ptr<IUIAutomationCacheRequest> _cache;
  };

  /// Lines from standard input, handed from the reader thread to the main one
  struct inbox_t {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> lines;
    bool closed = false;
  };

  void read_input(inbox_t &inbox) {
    char line[64];
    while (std::fgets(line, sizeof(line), stdin)) {
      std::string text {line};
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
      }
      std::lock_guard lg(inbox.mutex);
      inbox.lines.push_back(std::move(text));
      inbox.cv.notify_one();
    }
    std::lock_guard lg(inbox.mutex);
    inbox.closed = true;
    inbox.cv.notify_one();
  }

  /// Carries out what the policy said
  void act(keyboard_t &keyboard, tkp::policy_t::action_e action, const std::string &why) {
    using action_e = tkp::policy_t::action_e;
    if (action == action_e::none) {
      return;
    }
    auto show = action == action_e::show;
    if (keyboard.toggle(show)) {
      say("info", (show ? "shown for "s : "hidden: "s) + why);
    }
  }
}  // namespace

int main(int argc, char **argv) {
  auto watch = argc > 1 && std::strcmp(argv[1], "--watch") == 0;
  if (argc > 1 && !watch) {
    std::fprintf(stderr,
                 "Usage: apollo-touch-keyboard [--watch]\n"
                 "Apollo starts it while stock Moonlight for iOS touches; --watch tries it by hand:\n"
                 "each focus change counts as a tap, and typing \"tap\" or \"key\" counts as Apollo's.\n");
    return 2;
  }

  auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    say("error", "COM didn't start: " + hex(hr));
    return 1;
  }

  automation_t automation;
  if (!automation.init()) {
    return 1;
  }

  keyboard_t keyboard;
  tkp::policy_t policy {keyboard.trusted()};
  say("info", "ready; whether the keyboard is up comes from "s + keyboard.source_name());

  inbox_t inbox;
  std::thread reader {read_input, std::ref(inbox)};
  reader.detach();

  std::string last_focus;
  std::string watched;
  if (watch) {
    com_ptr<IUIAutomationElement> element;
    HRESULT ignored;
    if (automation.focused(element, ignored)) {
      watched = automation.signature(element.get());
    }
  }

  while (true) {
    std::deque<std::string> lines;
    bool closed;
    {
      std::unique_lock ul(inbox.mutex);
      auto next = policy.next_check();
      auto until = std::chrono::steady_clock::now() + (watch ? 250ms : 1h);
      if (next && *next < until) {
        until = *next;
      }
      // --watch goes on when the console's input ends
      inbox.cv.wait_until(ul, until, [&inbox, watch] {
        return !inbox.lines.empty() || (inbox.closed && !watch);
      });
      lines.swap(inbox.lines);
      closed = inbox.closed;
    }

    auto now = std::chrono::steady_clock::now();
    for (const auto &line : lines) {
      if (line == "tap") {
        policy.tap(now);
      } else if (line == "key") {
        act(keyboard, policy.key(keyboard.up(), now), "the client typed");
      } else if (!line.empty()) {
        say("warning", "unknown command [" + line.substr(0, 32) + ']');
      }
    }
    if (closed && !watch) {
      break;
    }

    if (watch) {
      com_ptr<IUIAutomationElement> element;
      HRESULT ignored;
      if (automation.focused(element, ignored)) {
        auto signature = automation.signature(element.get());
        if (signature != watched) {
          say("debug", "the focus moved; taking it as a tap");
          policy.tap(now);
          watched = std::move(signature);
        }
      }
    }

    auto next = policy.next_check();
    if (next && now >= *next) {
      auto focus = automation.judge_focus();
      if (focus.why != last_focus) {
        say("debug", "focus: " + focus.why);
        last_focus = focus.why;
      }
      act(keyboard, policy.focus(focus.editable, keyboard.up(), std::chrono::steady_clock::now()), focus.why);
    }
  }

  if (policy.ours(keyboard.up(), std::chrono::steady_clock::now()) && keyboard.toggle(false)) {
    say("info", "hidden: Apollo is done with it");
  }
  say("info", "stopped");
  CoUninitialize();
  return 0;
}
