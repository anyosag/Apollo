/**
 * @file src/fork/display_group.cpp
 * @brief Fork-only: "Desktop (multiple displays)" for LuzLunar (display_group.h).
 */
// standard includes
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

// local includes
#include "display_group.h"
#include "fork_config.h"
#include "src/logging.h"
// As vdisplay_recovery.cpp has them: network.h's Boost.Asio before rtsp.h's windows.h, which would
// bring in WinSock 1 first
#include "src/network.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/process.h"
#include "src/video.h"

#ifdef _WIN32
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>

  #include "client_display.h"
  #include "luzvda_desc.h"
  #include "src/config.h"
  #include "src/display_device.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"
  #include "vdisplay_extend.h"
#endif

using namespace std::literals;

namespace fork_tweaks::display_group {
  namespace {
    /// A slot past 0
    struct slot_entry_t {
      display_group_policy::slot_t declared;
      guid_t guid {};
      bool created = false;  ///< The driver made its display
      vdisplay_mode::display_mode_t mode {};  ///< The mode it runs
      std::string display;  ///< Its GDI name, as last seen
      display_group_policy::rect_t place;  ///< Where it is on the desktop

      // The stream that has it
      std::string device_uuid;  ///< Empty while it's free
      std::string client;
      std::uint32_t launch_session_id = 0;
      std::weak_ptr<stream::session_t> session;
    };

    struct state_t {
      bool active = false;
      std::optional<display_group_policy::declaration_t> declaration;
      std::vector<slot_entry_t> slots;  ///< Slots 1 and up, by number
      std::string lead_display;
      display_group_policy::rect_t lead_place;
      std::uint32_t lead_session_id = 0;  ///< Slot 0's stream now; 0 for none
#ifdef _WIN32
      std::optional<vdisplay_extend::snapshot_t> before;  ///< The displays on before the group
#endif
    };

    std::mutex state_mutex;
    state_t state;
#ifdef _WIN32
    std::optional<vdisplay_extend::snapshot_t> ended_before;  ///< From end() to restore()
#endif

    std::string id_of(const state_t &s) {
      return s.declaration ? s.declaration->id : std::string {};
    }

    slot_entry_t *find(int slot) {
      for (auto &e : state.slots) {
        if (e.declared.slot == slot) {
          return &e;
        }
      }
      return nullptr;
    }

#ifdef _WIN32
    display_device::WinDisplayDevice make_device() {
      return display_device::WinDisplayDevice {std::make_shared<display_device::WinApiLayer>()};
    }

    GUID to_guid(const guid_t &bytes) {
      GUID guid;
      std::memcpy(&guid, bytes.data(), sizeof(guid));
      return guid;
    }

    std::string name_now(const guid_t &guid) {
      return platf::to_utf8(VDISPLAY::getVirtualDisplayName(to_guid(guid)));
    }

    /**
     * @brief Gives a display @p mode, or a slower multiple of the stream's rate Windows will run, as
     * proc_t's setup_virtual_display() does.
     * @return The mode it runs.
     */
    vdisplay_mode::display_mode_t set_mode(const std::string &display, const vdisplay_mode::display_mode_t &mode, const std::vector<uint32_t> &slower) {
      auto nameW = platf::from_utf8(display);
      int width = 0;
      int height = 0;
      int refresh_mhz = 0;
      auto runs = [&](const vdisplay_mode::display_mode_t &m) {
        return VDISPLAY::getDisplayMode(nameW.c_str(), width, height, refresh_mhz) && vdisplay_mode::runs(m, width, height, refresh_mhz);
      };

      auto running = mode;
      if (runs(mode)) {
        return running;
      }
      VDISPLAY::changeDisplaySettings(nameW.c_str(), mode.width, mode.height, mode.refresh_mhz);
      for (auto rate : slower) {
        if (runs(running)) {
          return running;
        }
        BOOST_LOG(warning) << "fork: display group: "sv << display << " runs "sv << width << 'x' << height << 'x' << refresh_mhz
                           << " mHz, not "sv << running.refresh_mhz << " mHz; trying "sv << rate << " mHz"sv;
        running.refresh_mhz = rate;
        VDISPLAY::changeDisplaySettings(nameW.c_str(), running.width, running.height, running.refresh_mhz);
      }
      if (!runs(running)) {
        BOOST_LOG(warning) << "fork: display group: "sv << display << " runs "sv << width << 'x' << height << 'x' << refresh_mhz
                           << " mHz, not "sv << running.refresh_mhz << " mHz"sv;
        if (width > 0 && height > 0 && refresh_mhz > 0) {
          running = {(uint32_t) width, (uint32_t) height, (uint32_t) refresh_mhz};
        }
      }
      return running;
    }

    /// Makes slot @p s's display, described as its Mac display
    slot_entry_t make_display(const display_group_policy::slot_t &s, const lead_t &lead) {
      slot_entry_t e;
      e.declared = s;
      e.guid = display_group_policy::slot_guid(lead.guid, s.slot);

      auto mode = vdisplay_mode::for_client(s.width, s.height, (int) s.fps_mhz, 100, lead.multiplier);
      auto slower = vdisplay_mode::slower_rates(mode, s.fps_mhz);
      auto name = s.screen && !s.screen->name.empty() ? s.screen->name : lead.device_name + " (" + std::to_string(s.slot + 1) + ')';
      auto desc = luzvda_desc::display(client_display::describe(s.screen, mode, slower), name, e.guid);

      auto displayW = VDISPLAY::createVirtualDisplay(desc);
      if (displayW.empty()) {
        BOOST_LOG(warning) << "fork: display group: couldn't make slot "sv << s.slot << "'s display"sv;
        return e;
      }
      e.created = true;
      e.display = platf::to_utf8(displayW);
      if (!VDISPLAY::waitForOutput(displayW.c_str(), 2s)) {
        BOOST_LOG(warning) << "fork: display group: DXGI doesn't list "sv << e.display << " after 2 s"sv;
      }

      e.mode = set_mode(e.display, mode, slower);
      e.place = {s.x, s.y, (int) e.mode.width, (int) e.mode.height};
      BOOST_LOG(info) << "fork: display group: slot "sv << s.slot << " is "sv << e.display << ", "sv << e.mode.width << 'x' << e.mode.height
                      << " at "sv << e.mode.refresh_mhz / 1000.0 << " Hz, for "sv << (s.screen ? client_display::summary(*s.screen) : "a screen it wasn't told about"s)
                      << (s.external ? ", external"sv : ""sv);
      return e;
    }

    std::vector<slot_entry_t> make_displays(const display_group_policy::declaration_t *declaration, const lead_t &lead) {
      std::vector<slot_entry_t> made;
      if (!declaration) {
        return made;
      }
      for (const auto &s : declaration->slots) {
        if (s.slot > 0) {
          made.push_back(make_display(s, lead));
        }
      }
      return made;
    }

    void remove_displays(const std::vector<slot_entry_t> &slots) {
      for (const auto &e : slots) {
        if (e.created && !VDISPLAY::removeVirtualDisplay(to_guid(e.guid))) {
          BOOST_LOG(warning) << "fork: display group: couldn't remove slot "sv << e.declared.slot << "'s display"sv;
        }
      }
    }
#endif

    /// The streams of slots past 0, taken out of their claims
    std::vector<std::shared_ptr<stream::session_t>> take_slot_sessions() {
      std::vector<std::shared_ptr<stream::session_t>> sessions;
      for (auto &e : state.slots) {
        if (auto session = e.session.lock()) {
          sessions.push_back(std::move(session));
        }
        e.device_uuid.clear();
        e.client.clear();
        e.launch_session_id = 0;
        e.session.reset();
      }
      return sessions;
    }

    /// Their control thread disconnects them; graceful_stop() would send on ENet from this thread
    void stop(const std::vector<std::shared_ptr<stream::session_t>> &sessions) {
      for (const auto &session : sessions) {
        stream::session::stop(*session);
      }
    }
  }  // namespace

  int max_slots() {
#ifdef _WIN32
    bool desktop_duplication = config::video.capture.empty() || config::video.capture == "ddx";
    static_assert(display_group_policy::max_slots == LUZVDA_MAX_DISPLAYS);
    if (!display_group_enabled || proc::vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK || !desktop_duplication || !video::fork_parallel_encoding()) {
      return 0;
    }
    return display_group_policy::max_slots;
#else
    return 0;
#endif
  }

  std::string app_id() {
    for (const auto &app : proc::proc.get_apps()) {
      if (app.uuid == DISPLAY_GROUP_UUID) {
        return app.id;
      }
    }
    return "0";
  }

  bool is_app(const std::string &appid, const std::string &appuuid) {
    if (appuuid == DISPLAY_GROUP_UUID) {
      return true;
    }
    auto id = app_id();
    return appuuid.empty() && id != "0" && appid == id;
  }

  void begin() {
    std::lock_guard lock {state_mutex};
    state = {};
#ifdef _WIN32
    ended_before.reset();
    state.before = vdisplay_extend::snapshot();
    BOOST_LOG(info) << "fork: display group: the displays on before the group: "sv << extend_layout::describe(state.before->topology);
#endif
  }

  void create(const display_group_policy::declaration_t *declaration, [[maybe_unused]] const lead_t &lead) {
#ifdef _WIN32
    auto made = make_displays(declaration, lead);
#else
    std::vector<slot_entry_t> made;
#endif
    std::lock_guard lock {state_mutex};
    state.active = true;
    if (declaration) {
      state.declaration = *declaration;
    } else {
      state.declaration.reset();
    }
    state.slots = std::move(made);
    state.lead_display = lead.display;
    BOOST_LOG(info) << "fork: display group: "sv << (declaration ? "group "s + declaration->id + " with "s + std::to_string(state.slots.size() + 1) + " slots"s : "slot 0 alone, without a declaration"s);
  }

  std::string arrange([[maybe_unused]] const lead_t &lead) {
#ifdef _WIN32
    struct item_t {
      int slot;
      guid_t guid;
      std::string display;
      std::string id;
      vdisplay_mode::display_mode_t mode;
      display_group_policy::rect_t declared;
      bool hdr;
    };

    std::vector<item_t> items;
    {
      std::lock_guard lock {state_mutex};
      if (!state.active) {
        return {};
      }
      items.push_back({0, lead.guid, {}, {}, lead.mode, {0, 0, (int) lead.mode.width, (int) lead.mode.height}, false});
      for (const auto &e : state.slots) {
        if (e.created) {
          items.push_back({e.declared.slot, e.guid, {}, {}, e.mode, {e.declared.x, e.declared.y, (int) e.mode.width, (int) e.mode.height}, e.declared.hdr});
        }
      }
    }

    auto resolve = [&]() {
      for (auto &item : items) {
        item.display = name_now(item.guid);
        if (item.display.empty() && item.slot == 0) {
          item.display = lead.display;
        }
      }
    };
    resolve();

    // Only the group's displays on: the PC's own are off while it runs. A display that has no name
    // or no display_device id would be switched off too, so the topology is left as it is then.
    display_device::ActiveTopology topology;
    bool complete = true;
    for (auto &item : items) {
      item.id = item.display.empty() ? std::string {} : display_device::map_display_name(item.display);
      if (item.id.empty()) {
        complete = false;
        BOOST_LOG(warning) << "fork: display group: display_device doesn't list slot "sv << item.slot << "'s display "sv << item.display;
      }
      topology.push_back({item.id});
    }
    auto device = make_device();
    if (complete) {
      auto now = device.getCurrentTopology();
      if (!device.isTopologyTheSame(now, topology)) {
        BOOST_LOG(info) << "fork: display group: "sv << extend_layout::describe(now) << " are on, setting "sv << extend_layout::describe(topology);
        if (!device.setTopology(topology)) {
          BOOST_LOG(warning) << "fork: display group: Windows didn't take the group's displays alone"sv;
        }
        // Switching displays off can rename the others
        resolve();
      }
    }

    // Each display in its mode; Windows may restore another for a set of displays it has seen
    for (auto &item : items) {
      if (item.display.empty()) {
        continue;
      }
      auto nameW = platf::from_utf8(item.display);
      int width = 0;
      int height = 0;
      int refresh_mhz = 0;
      // A mode of 0x0 is one that was never set, as for a launch without a mode
      bool known = item.mode.width && item.mode.height && item.mode.refresh_mhz;
      if (known && VDISPLAY::getDisplayMode(nameW.c_str(), width, height, refresh_mhz) && !vdisplay_mode::runs(item.mode, width, height, refresh_mhz)) {
        BOOST_LOG(info) << "fork: display group: "sv << item.display << " runs "sv << width << 'x' << height << 'x' << refresh_mhz << " mHz, setting "sv
                        << item.mode.width << 'x' << item.mode.height << 'x' << item.mode.refresh_mhz << " mHz"sv;
        VDISPLAY::changeDisplaySettings(nameW.c_str(), item.mode.width, item.mode.height, item.mode.refresh_mhz);
      }
      if (VDISPLAY::getDisplayMode(nameW.c_str(), width, height, refresh_mhz) && width > 0 && height > 0) {
        item.declared.width = width;
        item.declared.height = height;
      }
    }

    // Every display in its place in one pass, slot 0 at the origin and so primary
    std::vector<display_group_policy::rect_t> declared;
    for (const auto &item : items) {
      declared.push_back(item.declared);
    }
    bool as_declared = false;
    auto places = display_group_policy::arrange(declared, as_declared);
    if (!as_declared && items.size() > 1) {
      BOOST_LOG(warning) << "fork: display group: the declared places overlap or leave a display apart, so the displays go in a row"sv;
    }
    std::vector<VDISPLAY::display_position_t> positions;
    std::ostringstream layout;
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (items[i].display.empty()) {
        continue;
      }
      positions.push_back({platf::from_utf8(items[i].display), places[i].x, places[i].y});
      layout << (i ? "; "sv : ""sv) << "slot "sv << items[i].slot << ' ' << items[i].display << ' ' << places[i].width << 'x' << places[i].height
             << " at "sv << places[i].x << ',' << places[i].y;
    }
    if (!VDISPLAY::setDisplayPositions(positions)) {
      BOOST_LOG(warning) << "fork: display group: Windows didn't take the displays' places"sv;
    }
    if (!items[0].id.empty() && !device.isPrimary(items[0].id)) {
      BOOST_LOG(info) << "fork: display group: making slot 0's display primary"sv;
      if (!device.setAsPrimary(items[0].id)) {
        BOOST_LOG(warning) << "fork: display group: couldn't make slot 0's display primary"sv;
      }
    }
    BOOST_LOG(info) << "fork: display group: "sv << layout.str();

    // Slot 0's HDR is proc_t's, as for any app
    if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic) {
      for (std::size_t i = 1; i < items.size(); ++i) {
        if (items[i].display.empty()) {
          continue;
        }
        auto nameW = platf::from_utf8(items[i].display);
        if (VDISPLAY::getDisplayHDRByName(nameW.c_str()) != items[i].hdr) {
          bool ok = VDISPLAY::setDisplayHDRByName(nameW.c_str(), items[i].hdr);
          BOOST_LOG(info) << "fork: display group: HDR "sv << (items[i].hdr ? "on"sv : "off"sv) << (ok ? ""sv : " failed"sv) << " for "sv << items[i].display;
        }
      }
    }

    std::lock_guard lock {state_mutex};
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i == 0) {
        state.lead_display = items[0].display;
        state.lead_place = places[0];
      } else if (auto *e = find(items[i].slot)) {
        e->display = items[i].display;
        e->place = places[i];
      }
    }
    return items[0].display;
#else
    return {};
#endif
  }

  std::optional<display_group_policy::declaration_t> declaration() {
    std::lock_guard lock {state_mutex};
    return state.declaration;
  }

  void adopt(const std::string &id) {
    std::lock_guard lock {state_mutex};
    if (state.declaration && state.declaration->id != id) {
      BOOST_LOG(info) << "fork: display group: group "sv << state.declaration->id << " is group "sv << id << " now, with the same layout"sv;
      state.declaration->id = id;
    }
  }

  void rebuild(const display_group_policy::declaration_t &declaration, [[maybe_unused]] const lead_t &lead) {
    std::vector<std::shared_ptr<stream::session_t>> sessions;
    std::vector<slot_entry_t> old;
    {
      std::lock_guard lock {state_mutex};
      BOOST_LOG(info) << "fork: display group: slot 0 declares another layout, making the group's displays again as group "sv << declaration.id;
      sessions = take_slot_sessions();
      old = std::move(state.slots);
      state.slots.clear();
    }
    stop(sessions);
#ifdef _WIN32
    remove_displays(old);
    auto made = make_displays(&declaration, lead);
#else
    std::vector<slot_entry_t> made;
#endif
    std::lock_guard lock {state_mutex};
    state.declaration = declaration;
    state.slots = std::move(made);
  }

  void end() {
    std::vector<std::shared_ptr<stream::session_t>> sessions;
    std::vector<slot_entry_t> slots;
    {
      std::lock_guard lock {state_mutex};
      if (!state.active
#ifdef _WIN32
          && !state.before
#endif
      ) {
        return;
      }
      BOOST_LOG(info) << "fork: display group: the app quits, ending group "sv << id_of(state);
      sessions = take_slot_sessions();
      slots = std::move(state.slots);
#ifdef _WIN32
      ended_before = std::move(state.before);
#endif
      state = {};
    }
    stop(sessions);

#ifdef _WIN32
    remove_displays(slots);
#endif
  }

  void restore() {
#ifdef _WIN32
    std::optional<vdisplay_extend::snapshot_t> before;
    {
      std::lock_guard lock {state_mutex};
      before = std::move(ended_before);
      ended_before.reset();
    }

    // The PC's displays as they were before the group, now that no group display is connected
    if (before && !before->topology.empty()) {
      auto device = make_device();
      BOOST_LOG(info) << "fork: display group: switching on "sv << extend_layout::describe(before->topology) << " again"sv;
      if (!device.setTopology(before->topology)) {
        BOOST_LOG(warning) << "fork: display group: couldn't switch the PC's displays on again"sv;
      }
      if (!before->primary.empty() && !device.isPrimary(before->primary) && !device.setAsPrimary(before->primary)) {
        BOOST_LOG(warning) << "fork: display group: couldn't make "sv << before->primary << " the primary display again"sv;
      }
    }
#endif
  }

  bool active() {
    std::lock_guard lock {state_mutex};
    return state.active;
  }

  std::optional<display_group_policy::group_view_t> view() {
    std::lock_guard lock {state_mutex};
    if (!state.active) {
      return std::nullopt;
    }
    display_group_policy::group_view_t view;
    view.id = id_of(state);
    for (const auto &e : state.slots) {
      if (e.created) {
        view.slots.push_back({e.declared.slot, e.declared.external, e.device_uuid});
      }
    }
    return view;
  }

  crypto::PERM slot_permission(crypto::PERM permission) {
    using crypto::PERM;
    auto denied = (uint32_t) PERM::_all_inputs | (uint32_t) PERM::server_cmd | (uint32_t) PERM::clipboard_set | (uint32_t) PERM::file_upload;
    return static_cast<PERM>((uint32_t) permission & ~denied);
  }

  bool claim(int slot, const rtsp_stream::launch_session_t &launch_session, const std::shared_ptr<stream::session_t> &session) {
    std::shared_ptr<stream::session_t> replaced;
    {
      std::lock_guard lock {state_mutex};
      if (slot == 0) {
        state.lead_session_id = launch_session.id;
        BOOST_LOG(info) << "fork: display group: client ["sv << launch_session.device_name << "] streams slot 0"sv;
        return true;
      }
      auto *e = state.active ? find(slot) : nullptr;
      if (!e || !e->created) {
        BOOST_LOG(warning) << "fork: display group: client ["sv << launch_session.device_name << "] asked for slot "sv << slot << ", which the group doesn't have now"sv;
        return false;
      }
      if (!e->device_uuid.empty() && e->device_uuid != launch_session.unique_id) {
        BOOST_LOG(warning) << "fork: display group: client ["sv << launch_session.device_name << "] asked for slot "sv << slot << ", which ["sv << e->client << "] has"sv;
        return false;
      }
      replaced = e->session.lock();
      e->device_uuid = launch_session.unique_id;
      e->client = launch_session.device_name;
      e->launch_session_id = launch_session.id;
      e->session = session;
      BOOST_LOG(info) << "fork: display group: client ["sv << launch_session.device_name << "] streams slot "sv << slot << " ("sv << e->display << ')';
    }
    if (replaced && replaced != session) {
      BOOST_LOG(info) << "fork: display group: slot "sv << slot << "'s earlier stream from the same client ends"sv;
      stream::session::stop(*replaced);
    }
    return true;
  }

  void release(int slot, std::uint32_t launch_session_id) {
    std::vector<std::shared_ptr<stream::session_t>> sessions;
    {
      std::lock_guard lock {state_mutex};
      if (slot == 0) {
        // An earlier stream of slot 0 that ended after a newer one began changes nothing
        if (state.lead_session_id != launch_session_id) {
          return;
        }
        state.lead_session_id = 0;
        sessions = take_slot_sessions();
        if (!sessions.empty()) {
          BOOST_LOG(info) << "fork: display group: slot 0's stream ended, so the other slots' "sv << sessions.size() << " streams end too"sv;
        }
      } else if (auto *e = find(slot); e && e->launch_session_id == launch_session_id) {
        BOOST_LOG(info) << "fork: display group: client ["sv << e->client << "] stopped streaming slot "sv << slot;
        e->device_uuid.clear();
        e->client.clear();
        e->launch_session_id = 0;
        e->session.reset();
      }
    }
    stop(sessions);
  }

  std::optional<std::string> slot_display(int slot) {
#ifdef _WIN32
    guid_t guid;
    {
      std::lock_guard lock {state_mutex};
      auto *e = state.active ? find(slot) : nullptr;
      if (!e || !e->created) {
        return std::nullopt;
      }
      guid = e->guid;
    }
    // Windows renames displays when others switch on or off
    auto name = name_now(guid);
    if (!name.empty()) {
      std::lock_guard lock {state_mutex};
      if (auto *e = find(slot); e && e->guid == guid) {
        e->display = name;
      }
    }
    return name;
#else
    return std::nullopt;
#endif
  }

  std::string status_json() {
    std::lock_guard lock {state_mutex};
    std::vector<display_group_policy::slot_status_t> slots;
    for (const auto &e : state.slots) {
      if (!e.created) {
        continue;
      }
      slots.push_back({e.declared.slot, e.display, e.mode.width, e.mode.height, e.place.x, e.place.y, e.declared.external, !e.device_uuid.empty(), e.client});
    }
    return display_group_policy::status_json(state.active, id_of(state), slots);
  }
}  // namespace fork_tweaks::display_group
