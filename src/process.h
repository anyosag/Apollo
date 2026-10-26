/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

// lib includes
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/group.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "fork/client_display.h"
#include "fork/display_group.h"  // [fork]
#include "fork/vdisplay_mode.h"
#include "fork/vdisplay_recovery.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
#endif

#define VIRTUAL_DISPLAY_UUID "8902CB19-674A-403D-A587-41B092E900BA"
#define FALLBACK_DESKTOP_UUID "EAAC6159-089A-46A9-9E24-6436885F6610"
#define REMOTE_INPUT_UUID "8CB5C136-DA67-4F99-B4A1-F9CD35005CF4"
#define TERMINATE_APP_UUID "E16CBE1B-295D-4632-9A76-EC4180C857D3"
// [fork] "Desktop (multiple displays)" for LuzLunar (fork/display_group.h)
#define DISPLAY_GROUP_UUID "5B1E7C3A-2D84-4F69-9C0B-7A3E1F6D2C58"

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

#ifdef _WIN32
  extern VDISPLAY::DRIVER_STATUS vDisplayDriverStatus;
#endif

  typedef config::prep_cmd_t cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;
    std::vector<cmd_t> state_cmds;

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string idx;
    std::string uuid;
    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string id;
    std::string gamepad;
    bool elevated;
    bool auto_detach;
    bool wait_all;
    bool virtual_display;
    bool use_app_identity;
    bool per_client_app_identity;
    bool allow_client_commands;
    bool terminate_on_pause;
    int  scale_factor;
    std::chrono::seconds exit_timeout;
  };

  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    std::string display_name;
    std::string initial_display;
    std::string mode_changed_display;
    bool initial_hdr = false;
    bool virtual_display = false;
    bool allow_client_commands = false;

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    void launch_input_only();

    int execute(const ctx_t& _app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    const std::vector<ctx_t> &get_apps() const;
    std::vector<ctx_t> &get_apps();
    std::string get_app_image(int app_id);
    std::string get_last_run_app_name();
    std::string get_running_app_uuid();
    boost::process::v1::environment get_env();
    void resume();
    void pause();
    void terminate(bool immediate = false, bool needs_refresh = true);

    /// [fork] display_name, read under a lock: capture reads it and the recovery monitor renames it
    std::string stream_display();

    /**
     * @brief [fork] Capture moved to @p display: picked with the display shortcut (@p chosen), or
     * the display it captured is gone. A virtual display that's gone for a moment stays the display
     * to capture, so capture goes back to it once the recovery monitor brings it back.
     */
    void capture_moved(const std::string &display, bool chosen);

    /// [fork] The app is "Desktop (multiple displays)", whose displays fork/display_group.h arranges
    bool display_group_app() const;

    /**
     * @brief [fork] Slot 0's launch or resume of the running display group: the same layout takes
     * the declaration's new id, another makes the other slots' displays again while nobody streams.
     * @return 200, 409 (another layout while someone streams) or 503.
     */
    int lead_display_group(const display_group_policy::declaration_t &declaration, bool streaming);

#ifdef _WIN32
    /// [fork] What reconfigure_virtual_display() did
    enum class vdisplay_change_e {
      unchanged,  ///< The display already ran the mode
      mode_changed,  ///< The mode or HDR state changed
      replugged,  ///< The driver didn't offer the mode, so the display was re-created with it
      failed,  ///< Re-creating the display failed; there is none now
    };

    /**
     * @brief [fork] Gives the running app's virtual display the mode and HDR state a resuming client
     * asks for. A resume, or a launch of the app already running, used to keep the display as the
     * first client set it up.
     */
    vdisplay_change_e reconfigure_virtual_display(const rtsp_stream::launch_session_t &session);

    /// [fork] For the recovery monitor (fork/vdisplay_recovery.h): held while it acts, and while a
    /// resume changes the display; empty when one is changing it already
    static std::unique_lock<std::recursive_mutex> try_lock_virtual_display();

    /// [fork] The virtual display's GDI name, as last seen; display_name may be another display
    /// picked with the shortcut. With the lock held.
    std::string virtual_display_name() const;

    /// [fork] Switches the virtual display on again after Windows switched it off. With the lock held.
    bool reactivate_virtual_display();

    /// [fork] Removes the virtual display and adds it again. With the lock held.
    bool recreate_virtual_display();

    /**
     * @brief [fork] The virtual display is on as @p name: after it was off (@p was_off), or Windows
     * renamed it. Gives it its mode, layout and HDR state again if they changed. With the lock held.
     */
    void virtual_display_back(const std::string &name, bool was_off);
#endif

  private:
    int _app_id = 0;
    std::string _app_name;

    boost::process::v1::environment _env;

    std::shared_ptr<rtsp_stream::launch_session_t> _launch_session;
    std::shared_ptr<config::input_t> _saved_input_config;

    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;

    /// [fork] The virtual display refreshes this many times per stream frame
    /// (nv_stream_policy::refresh_multiplier()), for a stream at this rate, and runs at that refresh
    int _vdisplay_multiplier = 1;
    uint32_t _vdisplay_stream_mhz = 0;
    uint32_t _vdisplay_refresh_mhz = 0;

#ifdef _WIN32
    // [fork] The virtual display's identity, kept to re-create it on resume
    std::string _vdisplay_uuid;
    std::string _vdisplay_client_name;

    /// [fork] What the driver was last asked for, to tell on resume whether the screen changed
    LUZVDA_DISPLAY_DESC _vdisplay_desc {};

    /// [fork] The Virtual Display app with `fork_virtual_display_app_extends`: the client's screen
    /// goes next to the PC's displays, which stay on (fork/vdisplay_extend.h)
    bool extends_desktop() const;
    void configure_launch_display(const rtsp_stream::launch_session_t &session);

    bool setup_virtual_display(const vdisplay_mode::display_mode_t &mode, const std::optional<client_display::screen_t> &screen, const GUID &guid, bool apply_mode);
    void apply_virtual_display_hdr(const std::string &display, bool enable_hdr, bool record_initial);

    /// [fork] Re-creates the virtual display in @p want for @p session, then applies the launch's
    /// display configuration again
    vdisplay_change_e replug_virtual_display(const vdisplay_mode::display_mode_t &want, const rtsp_stream::launch_session_t &session, const char *why);

    /// [fork] Whether a display configuration (display_device) applies to the launch
    bool display_configured() const;

    /// [fork] The virtual display as it was set up: its GDI name now, its display_device id, the
    /// mode it should run and the monitor that brings it back (fork/vdisplay_recovery.h)
    std::string _vdisplay_name;
    std::string _vdisplay_id;
    vdisplay_mode::display_mode_t _vdisplay_mode {};
    std::unique_ptr<fork_tweaks::vdisplay_recovery::monitor_t> _vdisplay_recovery;

    void start_vdisplay_recovery();
    void stop_vdisplay_recovery();

    /// [fork] Slot 0 of a display group: this app's own display
    fork_tweaks::display_group::lead_t display_group_lead() const;
    /// [fork] Only the group's displays on, each in its place; follows slot 0's display if it's renamed
    void arrange_display_group();
#endif
  };

  boost::filesystem::path
  find_working_directory(const std::string &cmd, const boost::process::v1::environment &env);

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  std::string validate_app_image_path(std::string app_image_path);
  void refresh(const std::string &file_name, bool needs_terminate = true);
  void migrate_apps(nlohmann::json* fileTree_p, nlohmann::json* inputTree_p);
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;

  extern int input_only_app_id;
  extern std::string input_only_app_id_str;
  extern int terminate_app_id;
  extern std::string terminate_app_id_str;
}  // namespace proc

#ifdef BOOST_PROCESS_VERSION
  #undef BOOST_PROCESS_VERSION
#endif
