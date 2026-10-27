/**
 * @file src/fork/display_group.h
 * @brief Fork-only: "Desktop (multiple displays)" for LuzLunar. One virtual display per Mac display,
 * arranged like the Mac's, each streamed by a session of its own; the PC's own displays are off
 * while the app runs.
 *
 * proc_t runs the group with the app: begin() before slot 0's display is made, create() once it's
 * there, arrange() whenever slot 0's display changed or came back, end() when the app quits, and
 * restore() once slot 0's display is gone too. Slot 0
 * is the app's own virtual display, which proc_t sets up as for any app; this module makes, places
 * and removes the others. The group outlives its streams, so Windows keeps its windows where they
 * were until the app quits.
 *
 * nvhttp decides which slot a launch or resume takes (display_group_policy.h), RTSP claims it once
 * the stream is announced, and the stream gives it back when it ends; when slot 0's stream ends, the
 * other slots' streams end with it. Each slot past 0 captures its own display on a capture thread of
 * its own (video.cpp), with no audio and no input.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// local includes
#include "display_group_policy.h"
#include "vdisplay_mode.h"

namespace crypto {
  enum class PERM : uint32_t;
}

namespace rtsp_stream {
  struct launch_session_t;
}

namespace stream {
  struct session_t;
}

namespace fork_tweaks::display_group {
  using guid_t = std::array<std::uint8_t, 16>;

  /// Slot 0's display, as proc_t set it up
  struct lead_t {
    guid_t guid {};  ///< Its identity; slot k's is this with k mixed in (display_group_policy::slot_guid())
    std::string display;  ///< Its GDI name now
    vdisplay_mode::display_mode_t mode {};  ///< The mode it runs
    int multiplier = 1;  ///< Refreshes per stream frame (nv_stream_policy::refresh_multiplier())
    std::string device_name;  ///< The client's name, for a display without a screen name
  };

  /// The most slots a group can have now, for /serverinfo's LuzDisplayGroup: 0 while
  /// `fork_display_group` is off, LuzVDA isn't working, capture isn't Desktop Duplication or the
  /// encoder can't encode several streams at once
  int max_slots();

  /// The app's id, for /serverinfo's LuzDisplayGroupApp; "0" when it isn't listed
  std::string app_id();

  /// Whether a launch names the app, by id or UUID
  bool is_app(const std::string &appid, const std::string &appuuid);

  /// @name proc_t's side (Windows; nothing happens elsewhere). With proc_t's display lock held.
  /// @{

  /// Before slot 0's display is made: notes the displays that are on, to put back at end()
  void begin();

  /**
   * @brief Makes the displays of slots 1 and up.
   * @param declaration Slot 0's declaration; none for the app launched without one (slot 0 alone).
   */
  void create(const display_group_policy::declaration_t *declaration, const lead_t &lead);

  /**
   * @brief Only the group's displays on, each with its mode and in its place (as declared, or in a
   * row when that isn't a valid desktop), slot 0 primary at the origin, and each slot's HDR state.
   * @return Slot 0's GDI name now, which can change when other displays switch off.
   */
  std::string arrange(const lead_t &lead);

  /// The running group's declaration; none without one
  std::optional<display_group_policy::declaration_t> declaration();

  /// Slot 0 resumed with the same layout and a new id
  void adopt(const std::string &id);

  /// Slot 0 resumed with another layout while nobody streams: the other slots' displays are made again
  void rebuild(const display_group_policy::declaration_t &declaration, const lead_t &lead);

  /// The app quits: the other slots' streams stop and their displays go
  void end();

  /**
   * @brief After end(), once slot 0's display is gone too: the PC's displays as begin() found them.
   * Not before: Windows remembers which displays were on for each set of connected displays, so
   * switching the PC's back on with a group display still there had it remember that display off,
   * and the next group's slot 0 display arrived off.
   */
  void restore();

  /// @}

  /// A group runs; the display shortcut (Ctrl+Alt+Shift+F1...) does nothing then
  bool active();

  /// The running group for nvhttp's check_join(); none while there's no group
  std::optional<display_group_policy::group_view_t> view();

  /// What a slot past 0 may do: view only, with no input, server commands or clipboard writes
  crypto::PERM slot_permission(crypto::PERM permission);

  /**
   * @brief The stream announced for @p slot takes it; a stream of the same device that held it
   * already is stopped. Slot 0 always can.
   * @return Whether it may stream that slot.
   */
  bool claim(int slot, const rtsp_stream::launch_session_t &launch_session, const std::shared_ptr<stream::session_t> &session);

  /// The stream of @p slot ended (stream::session::join()); for slot 0, the others' streams end too
  void release(int slot, std::uint32_t launch_session_id);

  /**
   * @brief Slot @p slot's display, under the name Windows gives it now, for its capture thread.
   * @return None once the slot is gone; empty while the display has no name (Windows switched it off).
   */
  std::optional<std::string> slot_display(int slot);

  /// GET /actions/displaygroup's answer (display_group_policy::status_json())
  std::string status_json();
}  // namespace fork_tweaks::display_group
