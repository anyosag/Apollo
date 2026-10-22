/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#ifndef BOOST_PROCESS_VERSION
 #define BOOST_PROCESS_VERSION 1
#endif
// standard includes
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "fork/app_encoder_overrides.h"
#include "fork/fork_config.h"
#include "fork/nv_stream.h"
#include "fork/nv_stream_policy.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "httpcommon.h"
#include "system_tray.h"
#include "utility.h"
#include "video.h"
#include "uuid.h"

#ifdef _WIN32
  #include <cstring>

  // [fork] The LuzVDA driver's requests
  #include "fork/luzvda_desc.h"
  // [fork] Bringing the virtual display back mid-stream
  #include "fork/vdisplay_recovery.h"
  // [fork] The Virtual Display app's extended desktop
  #include "fork/fork_config.h"
  #include "fork/vdisplay_extend.h"

  // from_utf8() string conversion function
  #include "platform/windows/misc.h"
  #include "platform/windows/utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;

  int input_only_app_id = -1;
  std::string input_only_app_id_str;
  int terminate_app_id = -1;
  std::string terminate_app_id_str;

  namespace {
    // [fork] display_name is also read by capture and renamed by the recovery monitor
    std::mutex &display_name_mutex() {
      static std::mutex mutex;
      return mutex;
    }

#ifdef _WIN32
    // [fork] Keeps a resume's display changes and the recovery monitor's apart (fork/vdisplay_recovery.h)
    std::recursive_mutex &vdisplay_mutex() {
      static std::recursive_mutex mutex;
      return mutex;
    }
#endif
  }  // namespace

#ifdef _WIN32
  VDISPLAY::DRIVER_STATUS vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::UNKNOWN;

  void onVDisplayWatchdogFailed() {
    vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED;
    VDISPLAY::closeVDisplayDevice();
  }

  void initVDisplayDriver() {
    vDisplayDriverStatus = VDISPLAY::openVDisplayDevice();
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      if (!VDISPLAY::startPingThread(onVDisplayWatchdogFailed)) {
        onVDisplayWatchdogFailed();
        return;
      }
    }
  }

  /**
   * @brief Waits up to about 3 s for Windows to accept display changes again.
   * [fork] Returns false at once while the session is locked, which outlasts the wait: HDR is now
   * set during the launch, so waiting would only make a launch on a locked PC 3 s slower.
   */
  static bool wait_for_display_settings() {
    if (is_user_session_locked()) {
      return false;
    }

    auto retryInterval = 200ms;
    while (is_changing_settings_going_to_fail()) {
      if (retryInterval > 2s) {
        return false;
      }
      std::this_thread::sleep_for(retryInterval);
      retryInterval *= 2;
    }
    return true;
  }
#endif

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, const boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
#ifdef _WIN32
    auto parts = boost::program_options::split_winmain(cmd);
#else
    auto parts = boost::program_options::split_unix(cmd);
#endif
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  void proc_t::launch_input_only() {
    _app_id = input_only_app_id;
    _app_name = "Remote Input";
    _app.uuid = REMOTE_INPUT_UUID;
    _app.terminate_on_pause = true;
    allow_client_commands = false;
    placebo = true;

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_playing(_app_name);
#endif
  }

  int proc_t::execute(const ctx_t& app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    if (_app_id == input_only_app_id) {
      terminate(false, false);
      std::this_thread::sleep_for(1s);
    } else {
      // Ensure starting from a clean slate
      terminate(false, false);
    }

    // [fork] For the virtual display's timing in the log
    [[maybe_unused]] auto launch_start = std::chrono::steady_clock::now();

    _app = app;
    _app_id = util::from_view(app.id);
    _app_name = app.name;
    _launch_session = launch_session;
    allow_client_commands = app.allow_client_commands;

    // [fork] NVIDIA's frame cap and V-Sync off for the stream, set before the app starts, as Smooth
    // Motion needs. With games capped, the virtual display refreshes 4x per stream frame, else 2x.
    _vdisplay_stream_mhz = vdisplay_mode::stream_mhz(launch_session->fps);
    bool nv_capped = fork_tweaks::nv_stream::apply(app.uuid, _vdisplay_stream_mhz);
    _vdisplay_multiplier = nv_stream_policy::refresh_multiplier(fork_tweaks::vdisplay_refresh_multiplier, nv_capped);

    // [fork] Shared with reconfigure_virtual_display(), in fork/vdisplay_mode.h
    int scale_factor = vdisplay_mode::effective_scale(launch_session->scale_factor, _app.scale_factor);
    auto vdisplay_target = vdisplay_mode::for_client(
      launch_session->width,
      launch_session->height,
      launch_session->fps,
      scale_factor,
      _vdisplay_multiplier
    );

    uint32_t render_width = vdisplay_target.width;
    uint32_t render_height = vdisplay_target.height;

    launch_session->width = render_width;
    launch_session->height = render_height;

    this->initial_display = config::video.output_name;
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
      // Restore to user defined output name
      config::video.output_name = this->initial_display;
      terminate();
      display_device::revert_configuration();
    });

    if (!app.gamepad.empty()) {
      _saved_input_config = std::make_shared<config::input_t>(config::input);
      if (app.gamepad == "disabled") {
        config::input.controller = false;
      } else {
        config::input.controller = true;
        config::input.gamepad = app.gamepad;
      }
    }

    fork_tweaks::apply_app_encoder_overrides(app.uuid);

#ifdef _WIN32
    if (
      config::video.headless_mode        // Headless mode
      || launch_session->virtual_display // User requested virtual display
      || _app.virtual_display            // App is configured to use virtual display
      || !video::allow_encoder_probing() // No active display presents
    ) {
      if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        // Try init driver again
        initVDisplayDriver();
      }

      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        // [fork] Count the mode changes this launch makes
        VDISPLAY::takeModeStats();

        // [fork] The adapter the user configured, or else the GPU with the most video memory, which
        // then also encodes
        VDISPLAY::setRenderAdapter(config::video.adapter_name);

        std::string device_name;
        std::string device_uuid_str;
        uuid_util::uuid_t device_uuid;

        if (_app.use_app_identity) {
          device_name = _app.name;
          if (_app.per_client_app_identity) {
            device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
            auto app_uuid = uuid_util::uuid_t::parse(_app.uuid);

            // Use XOR to mix the two UUIDs
            device_uuid.b64[0] ^= app_uuid.b64[0];
            device_uuid.b64[1] ^= app_uuid.b64[1];

            device_uuid_str = device_uuid.string();
          } else {
            device_uuid_str = _app.uuid;
            device_uuid = uuid_util::uuid_t::parse(_app.uuid);
          }
        } else {
          device_name = launch_session->device_name;
          device_uuid_str = launch_session->unique_id;
          device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);

          // [fork] To Windows, the extended desktop's display is another one than the display this
          // client streams other apps to (which may be the only one on), so it keeps a layout of its
          // own: where the display goes next to the PC's
          if (extends_desktop()) {
            auto app_uuid = uuid_util::uuid_t::parse(_app.uuid);
            device_uuid.b64[0] ^= app_uuid.b64[0];
            device_uuid.b64[1] ^= app_uuid.b64[1];
            device_uuid_str = device_uuid.string();
          }
        }

        memcpy(&launch_session->display_guid, &device_uuid, sizeof(GUID));

        _vdisplay_uuid = device_uuid_str;
        _vdisplay_client_name = device_name;

        // Don't change display settings when no params are given
        setup_virtual_display(vdisplay_target, launch_session->client_screen, launch_session->display_guid, launch_session->fps != 0);

        // No matter we get the display name or not, the virtual display might still be created.
        // We need to track it properly to remove the display when the session terminates.
        launch_session->virtual_display = true;
      } else {
        // Driver isn't working so we don't need to track virtual display.
        launch_session->virtual_display = false;
      }
    }

    // [fork] The Virtual Display app keeps the PC's displays on
    configure_launch_display(*launch_session);

    // We should not preserve display state when using virtual display.
    // It is already handled by Windows properly.
    if (this->virtual_display) {
      display_device::reset_persistence();

      auto stats = VDISPLAY::takeModeStats();
      BOOST_LOG(info) << "vdisplay: ready in "sv << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - launch_start).count()
                      << " ms after launch began, "sv << stats.mode_sets << " mode changes"sv;

      // [fork] HDR is set here, before the encoder probe and the app start, so both find the display
      // as the client will see it. It used to be set on a detached thread after the app had started,
      // which read display_name while terminate() could clear it.
      if (wait_for_display_settings()) {
        // The display configuration above sets HDR to the client's request itself when dd_hdr_option
        // is auto; Apollo's switches below don't count that one
        auto displayW = platf::from_utf8(_vdisplay_name);
        bool configured_hdr = VDISPLAY::getDisplayHDRByName(displayW.c_str());
        apply_virtual_display_hdr(_vdisplay_name, launch_session->enable_hdr, true);

        stats = VDISPLAY::takeModeStats();
        BOOST_LOG(info) << "vdisplay: HDR set "sv << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - launch_start).count()
                        << " ms after launch began, "sv << stats.hdr_changes << " HDR switches; the display configuration left HDR "sv
                        << (configured_hdr ? "on"sv : "off"sv) << ", now "sv << (VDISPLAY::getDisplayHDRByName(displayW.c_str()) ? "on"sv : "off"sv);
      } else {
        BOOST_LOG(warning) << "vdisplay: Windows doesn't accept display changes now (a locked session?), HDR left as it is"sv;
      }

      // [fork] Brings the display back if Windows switches it off mid-stream
      start_vdisplay_recovery();
    }

#else

    display_device::configure_display(config::video, *launch_session);

#endif

    // Probe encoders again before streaming to ensure our chosen
    // encoder matches the active GPU (which could have changed
    // due to hotplugging, driver crash, primary monitor change,
    // or any number of other factors).
    if (rtsp_stream::session_count() == 0 && video::probe_encoders()) {
      if (config::video.ignore_encoder_probe_failure) {
        BOOST_LOG(warning) << "Encoder probe failed, but continuing due to user configuration.";
      } else {
        return 503;
      }
    }

    std::string fps_str;
    char fps_buf[8];
    snprintf(fps_buf, sizeof(fps_buf), "%.3f", (float)launch_session->fps / 1000.0f);
    fps_str = fps_buf;

    // Add Stream-specific environment variables
    // Sunshine Compatibility
    _env["SUNSHINE_APP_ID"] = _app.id;
    _env["SUNSHINE_APP_NAME"] = _app.name;
    _env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["SUNSHINE_CLIENT_FPS"] = config::sunshine.envvar_compatibility_mode ? std::to_string(std::round((float)launch_session->fps / 1000.0f)) : fps_str;
    _env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    _env["APOLLO_APP_ID"] = _app.id;
    _env["APOLLO_APP_NAME"] = _app.name;
    _env["APOLLO_APP_UUID"] = _app.uuid;
    _env["APOLLO_APP_STATUS"] = "STARTING";
    _env["APOLLO_CLIENT_UUID"] = launch_session->unique_id;
    _env["APOLLO_CLIENT_NAME"] = launch_session->device_name;
    _env["APOLLO_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["APOLLO_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["APOLLO_CLIENT_RENDER_WIDTH"] = std::to_string(launch_session->width);
    _env["APOLLO_CLIENT_RENDER_HEIGHT"] = std::to_string(launch_session->height);
    _env["APOLLO_CLIENT_SCALE_FACTOR"] = std::to_string(scale_factor);
    _env["APOLLO_CLIENT_FPS"] = fps_str;
    _env["APOLLO_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["APOLLO_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["APOLLO_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["APOLLO_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;
    _env["APOLLO_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = platf::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Sunshine.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait();
      auto ret = child.exit_code();
      if (ret != 0 && ec != std::errc::permission_denied) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
        return -1;
      }
    }

    _env["APOLLO_APP_STATUS"] = "RUNNING";

    for (auto &cmd : _app.detached) {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (_app.cmd.empty()) {
      BOOST_LOG(info) << "No commands configured, showing desktop..."sv;
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(_app.cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << _app.cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, _app.cmd, working_dir, _env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << _app.cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

    _app_launch_time = std::chrono::steady_clock::now();

    fg.disable();

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_playing(_app.name);
#endif

    return 0;
  }

#ifdef _WIN32
  // [fork] The Virtual Display app adds the client's screen next to the PC's displays (Desktop and
  // the other apps keep the configured display configuration, e.g. this display only)
  bool proc_t::extends_desktop() const {
    return _app.uuid == VIRTUAL_DISPLAY_UUID && fork_tweaks::virtual_display_app_extends;
  }

  // [fork] Applies the launch's display configuration. An extended desktop leaves the other
  // displays on and the primary display where it is, and with no virtual display to configure
  // leaves the displays alone: display_device would otherwise configure the primary one.
  void proc_t::configure_launch_display(const rtsp_stream::launch_session_t &session) {
    // [fork] display_device::configure_display(), but the virtual display keeps running at a multiple
    // of the stream's rate, which the automatic refresh rate would set it back to. A rate the user
    // set (a manual one, or a remapping) stays theirs, and so does any rate while display_device
    // doesn't list the virtual display, as it would configure the primary display instead.
    auto configure_display = [&](const config::video_t &video_config) {
      if (!virtual_display || !_vdisplay_refresh_mhz || session.fps <= 0 || config::video.output_name.empty()) {
        display_device::configure_display(video_config, session);
        return;
      }
      auto result = display_device::parse_configuration(video_config, session);
      if (auto *parsed = std::get_if<display_device::SingleDisplayConfiguration>(&result)) {
        const display_device::FloatingPoint stream_rate {display_device::Rational {(unsigned int) session.fps, 1000}};
        if (parsed->m_refresh_rate == stream_rate) {
          parsed->m_refresh_rate = display_device::Rational {_vdisplay_refresh_mhz, 1000};
        }
        display_device::configure_display(*parsed);
      } else if (std::holds_alternative<display_device::configuration_disabled_tag_t>(result)) {
        display_device::revert_configuration();
      }
    };

    if (!extends_desktop()) {
      configure_display(config::video);
    } else if (virtual_display && !config::video.output_name.empty()) {
      configure_display(fork_tweaks::vdisplay_extend::display_config(config::video));
    } else {
      BOOST_LOG(warning) << "vdisplay: no virtual display to extend the desktop with, the displays are left as they are"sv;
    }
  }

  // [fork] Creates the virtual display, describing the client's screen when it sent one, sets its
  // mode and, if configured, the isolated layout, and makes it the display Apollo captures
  bool proc_t::setup_virtual_display(const vdisplay_mode::display_mode_t &mode, const std::optional<client_display::screen_t> &screen, const GUID &guid, bool apply_mode) {
    luzvda_desc::guid_bytes_t guid_bytes;
    std::memcpy(guid_bytes.data(), &guid, guid_bytes.size());
    // [fork] With slower rates to fall back to, if Windows won't run this one
    auto slower = vdisplay_mode::slower_rates(mode, _vdisplay_stream_mhz);
    auto desc = luzvda_desc::display(client_display::describe(screen, mode, slower), _vdisplay_client_name, guid_bytes);

    // [fork] The displays on before the virtual display arrives stay on, for the Virtual Display app
    std::optional<fork_tweaks::vdisplay_extend::snapshot_t> extend_from;
    if (extends_desktop()) {
      extend_from = fork_tweaks::vdisplay_extend::snapshot();
    }

    std::wstring vdisplayName = VDISPLAY::createVirtualDisplay(desc);
    if (vdisplayName.empty()) {
      BOOST_LOG(warning) << "Virtual Display creation failed, or cannot get created display name in time!";
      return false;
    }
    _vdisplay_desc = desc;

    BOOST_LOG(info) << "Virtual Display created at " << platf::to_utf8(vdisplayName);

    // [fork] Capture, the encoder probe and the HDR state read find the display through DXGI, which
    // may list it a little after it has a name. The HDR state it arrives with is what Windows saved
    // for it, the end of its last stream.
    auto listing_start = std::chrono::steady_clock::now();
    if (VDISPLAY::waitForOutput(vdisplayName.c_str(), 2s)) {
      BOOST_LOG(info) << "vdisplay: DXGI lists "sv << platf::to_utf8(vdisplayName) << " after "sv
                      << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - listing_start).count()
                      << " ms; it arrived with HDR "sv << (VDISPLAY::getDisplayHDRByName(vdisplayName.c_str()) ? "on"sv : "off"sv);
    } else {
      BOOST_LOG(warning) << "vdisplay: DXGI doesn't list "sv << platf::to_utf8(vdisplayName) << " after 2 s"sv;
    }

    // [fork] Windows may have switched the PC's displays off, applying the layout it saved for this
    // set of displays when a stream left only the virtual display on
    if (extend_from) {
      fork_tweaks::vdisplay_extend::restore(*extend_from, display_device::map_display_name(platf::to_utf8(vdisplayName)));

      // Changing which displays are on can give the virtual display another name
      auto renamed = VDISPLAY::getVirtualDisplayName(guid);
      if (!renamed.empty() && renamed != vdisplayName) {
        BOOST_LOG(info) << "vdisplay: "sv << platf::to_utf8(vdisplayName) << " is now "sv << platf::to_utf8(renamed);
        vdisplayName = renamed;
      }
    }

    // The display usually arrives in its preferred mode, the stream's; Windows restores another
    // only for a display it has seen before
    auto running = mode;
    if (apply_mode) {
      int width = 0;
      int height = 0;
      int refresh_mhz = 0;
      auto runs = [&](const vdisplay_mode::display_mode_t &m) {
        return VDISPLAY::getDisplayMode(vdisplayName.c_str(), width, height, refresh_mhz) && vdisplay_mode::runs(m, width, height, refresh_mhz);
      };
      if (!runs(mode)) {
        VDISPLAY::changeDisplaySettings(vdisplayName.c_str(), mode.width, mode.height, mode.refresh_mhz);

        // [fork] A slower multiple of the stream's rate if Windows won't run this one
        for (auto rate : slower) {
          if (runs(running)) {
            break;
          }
          BOOST_LOG(warning) << "vdisplay: "sv << platf::to_utf8(vdisplayName) << " runs "sv << width << 'x' << height << 'x' << refresh_mhz
                             << " mHz, not "sv << running.refresh_mhz << " mHz; trying "sv << rate << " mHz"sv;
          running.refresh_mhz = rate;
          VDISPLAY::changeDisplaySettings(vdisplayName.c_str(), running.width, running.height, running.refresh_mhz);
        }
        if (!runs(running)) {
          BOOST_LOG(warning) << "vdisplay: "sv << platf::to_utf8(vdisplayName) << " runs "sv << width << 'x' << height << 'x' << refresh_mhz
                             << " mHz, not "sv << running.refresh_mhz << " mHz"sv;
          if (refresh_mhz > 0) {
            running.refresh_mhz = refresh_mhz;
          }
        }
      }
    }
    _vdisplay_refresh_mhz = running.refresh_mhz;
    BOOST_LOG(info) << "vdisplay: Display refresh rate "sv << running.refresh_mhz / 1000.0 << " Hz, "sv
                    << (_vdisplay_stream_mhz ? running.refresh_mhz / (double) _vdisplay_stream_mhz : 1.0) << "x the stream's"sv;

    // Check the ISOLATED DISPLAY configuration setting and rearrange the displays
    // [fork] Not for an extended desktop, which the mouse is meant to cross into
    if (config::video.isolated_virtual_display_option == true && !extend_from) {
      // Apply the isolated display settings
      VDISPLAY::changeDisplaySettings2(vdisplayName.c_str(), running.width, running.height, running.refresh_mhz, true);
    }

    // Set virtual_display to true when everything went fine
    this->virtual_display = true;
    _vdisplay_name = platf::to_utf8(vdisplayName);
    // [fork] The mode it runs, a slower rate it fell back to included: the one it gets when it comes back
    _vdisplay_mode = running;
    {
      std::lock_guard lock {display_name_mutex()};
      this->display_name = _vdisplay_name;
    }

    // When using virtual display, we don't care which display user configured to use.
    // So we always set output_name to the newly created virtual display as a workaround for
    // empty name when probing graphics cards.
    _vdisplay_id = display_device::map_display_name(_vdisplay_name);
    config::video.output_name = _vdisplay_id;
    if (config::video.output_name.empty()) {
      // display_device then configures whichever display is primary
      BOOST_LOG(warning) << "vdisplay: display_device doesn't list "sv << _vdisplay_name << " yet"sv;
    }
    return true;
  }

  // [fork] The HDR workaround from execute(): Windows doesn't set HDR correctly right after a
  // display connects or changes mode, so HDR is switched off and back on
  void proc_t::apply_virtual_display_hdr(const std::string &display, bool enable_hdr, bool record_initial) {
    auto displayW = platf::from_utf8(display);

    bool hdr = VDISPLAY::getDisplayHDRByName(displayW.c_str());
    if (record_initial) {
      initial_hdr = hdr;
    }

    if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic) {
      mode_changed_display = display;

      if (!config::video.dd.wa.virtual_display_hdr_toggle) {
        // [fork] Only set the state the client asked for, when it differs
        if (hdr != enable_hdr) {
          bool ok = VDISPLAY::setDisplayHDRByName(displayW.c_str(), enable_hdr);
          BOOST_LOG(info) << "HDR "sv << (enable_hdr ? "enable"sv : "disable"sv) << (ok ? "d"sv : " failed"sv) << " for display "sv << display << ", without switching it off first"sv;
        }
        return;
      }

      // Try turn off HDR whatever
      // As we always have to apply the workaround by turining off HDR first
      VDISPLAY::setDisplayHDRByName(displayW.c_str(), false);

      if (enable_hdr) {
        if (VDISPLAY::setDisplayHDRByName(displayW.c_str(), true)) {
          BOOST_LOG(info) << "HDR enabled for display " << display;
        } else {
          BOOST_LOG(info) << "HDR enable failed for display " << display;
        }
      }
    } else if (hdr) {
      if (VDISPLAY::setDisplayHDRByName(displayW.c_str(), false) && VDISPLAY::setDisplayHDRByName(displayW.c_str(), true)) {
        BOOST_LOG(info) << "HDR toggled successfully for display " << display;
      } else {
        BOOST_LOG(info) << "HDR toggle failed for display " << display;
      }
    }
  }

  proc_t::vdisplay_change_e proc_t::reconfigure_virtual_display(const rtsp_stream::launch_session_t &session) {
    // [fork] Not while the recovery monitor brings the display back
    std::lock_guard vdisplay_lock {vdisplay_mutex()};

    if (!virtual_display || _vdisplay_name.empty() || !_launch_session || vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      return vdisplay_change_e::unchanged;
    }

    auto start = std::chrono::steady_clock::now();
    VDISPLAY::takeModeStats();

    int scale_factor = vdisplay_mode::effective_scale(session.scale_factor, _app.scale_factor);
    auto want = vdisplay_mode::for_client(session.width, session.height, session.fps, scale_factor, _vdisplay_multiplier);
    _vdisplay_stream_mhz = vdisplay_mode::stream_mhz(session.fps);

    // The display's name now, from the driver: an old one may belong to a monitor of the PC by now,
    // and a display Windows switched off has none
    auto current = platf::to_utf8(VDISPLAY::getVirtualDisplayName(_launch_session->display_guid));
    if (!current.empty() && current != _vdisplay_name) {
      virtual_display_back(current, false);
    }

    auto displayW = platf::from_utf8(_vdisplay_name);
    int width = 0;
    int height = 0;
    int refresh_mhz = 0;
    bool known = !current.empty() && VDISPLAY::getDisplayMode(displayW.c_str(), width, height, refresh_mhz);

    // [fork] A display that fell back to a slower multiple of the stream's rate keeps it, rather than
    // being asked for the faster one, and re-created when it won't, at every resume
    if (known && (uint32_t) width == want.width && (uint32_t) height == want.height) {
      for (auto rate : vdisplay_mode::slower_rates(want, _vdisplay_stream_mhz)) {
        if (rate == _vdisplay_refresh_mhz && vdisplay_mode::runs({want.width, want.height, rate}, width, height, refresh_mhz)) {
          want.refresh_mhz = rate;
          break;
        }
      }
    }

    bool hdr_automatic = config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic;
    bool hdr_differs = hdr_automatic && VDISPLAY::getDisplayHDRByName(displayW.c_str()) != session.enable_hdr;

    // What the display would be for this client now. Windows reads a display's description only
    // when it arrives, so another screen needs it to arrive again; another mode doesn't.
    luzvda_desc::guid_bytes_t guid_bytes;
    std::memcpy(guid_bytes.data(), &_launch_session->display_guid, guid_bytes.size());
    auto desc = luzvda_desc::display(client_display::describe(session.client_screen, want, vdisplay_mode::slower_rates(want, _vdisplay_stream_mhz)), _vdisplay_client_name, guid_bytes);
    bool same_screen = luzvda_desc::same_screen(_vdisplay_desc, desc);

    auto replug = [&](const char *why) {
      return replug_virtual_display(want, session, why);
    };

    auto result = vdisplay_change_e::unchanged;
    if (!same_screen) {
      result = replug("the client streams to another screen");
    } else if (current.empty()) {
      result = replug("Windows switched the display off");
    } else if (!known || !vdisplay_mode::runs(want, width, height, refresh_mhz)) {
      BOOST_LOG(info) << "vdisplay: resume asks for "sv << want.width << 'x' << want.height << 'x' << want.refresh_mhz
                      << " mHz, "sv << _vdisplay_name << " runs "sv << width << 'x' << height << 'x' << refresh_mhz << " mHz"sv;

      // A display takes modes it didn't arrive with once the driver offers them (seen on Windows 11
      // build 26200), so a new one doesn't need the display to arrive again
      if (!luzvda_desc::offers(_vdisplay_desc, want) && VDISPLAY::updateVirtualDisplayModes(luzvda_desc::update(desc))) {
        BOOST_LOG(info) << "vdisplay: gave "sv << _vdisplay_name << ' ' << desc.mode_count << " modes for the new one"sv;
        _vdisplay_desc = desc;
      }
      // Windows may take a moment to offer modes the display just got, so the change is tried again
      bool took = false;
      if (luzvda_desc::offers(_vdisplay_desc, want)) {
        for (auto wait : {0ms, 150ms, 300ms, 600ms}) {
          std::this_thread::sleep_for(wait);
          VDISPLAY::changeDisplaySettings(displayW.c_str(), want.width, want.height, want.refresh_mhz);
          took = VDISPLAY::getDisplayMode(displayW.c_str(), width, height, refresh_mhz) && vdisplay_mode::runs(want, width, height, refresh_mhz);
          if (took) {
            _vdisplay_mode = want;
            break;
          }
          BOOST_LOG(info) << "vdisplay: "sv << _vdisplay_name << " runs "sv << width << 'x' << height << 'x' << refresh_mhz << " mHz after the change"sv;
        }
      }
      // [fork] A slower multiple of the stream's rate before re-creating the display
      for (auto rate : vdisplay_mode::slower_rates(want, _vdisplay_stream_mhz)) {
        vdisplay_mode::display_mode_t slower {want.width, want.height, rate};
        if (took || !luzvda_desc::offers(_vdisplay_desc, slower)) {
          continue;
        }
        VDISPLAY::changeDisplaySettings(displayW.c_str(), slower.width, slower.height, slower.refresh_mhz);
        took = VDISPLAY::getDisplayMode(displayW.c_str(), width, height, refresh_mhz) && vdisplay_mode::runs(slower, width, height, refresh_mhz);
        if (took) {
          BOOST_LOG(info) << "vdisplay: "sv << display_name << " runs "sv << rate << " mHz, slower than asked"sv;
          want.refresh_mhz = rate;
        }
      }
      if (took) {
        _vdisplay_refresh_mhz = want.refresh_mhz;
        _vdisplay_mode = want;
      }
      result = took ? vdisplay_change_e::mode_changed : replug("the display didn't take that mode");
    } else if (hdr_differs) {
      result = vdisplay_change_e::mode_changed;
    }

    if (result == vdisplay_change_e::mode_changed || result == vdisplay_change_e::replugged) {
      // A display keeps its HDR state through a mode change (LuzVDA's first test and the stream
      // test's resumes), so HDR is set again only when it should change or the display is new
      if (result == vdisplay_change_e::replugged || hdr_differs) {
        if (wait_for_display_settings()) {
          apply_virtual_display_hdr(_vdisplay_name, session.enable_hdr, false);
        } else {
          BOOST_LOG(warning) << "vdisplay: Windows didn't accept display changes in time, HDR left as it is"sv;
        }
      }

      _launch_session->width = want.width;
      _launch_session->height = want.height;
      _launch_session->fps = session.fps;
      _launch_session->enable_hdr = session.enable_hdr;
      _launch_session->client_screen = session.client_screen;

      // [fork] The recovery monitor judges the display afresh once it has settled
      if (_vdisplay_recovery) {
        _vdisplay_recovery->settle();
      }
    }

    auto stats = VDISPLAY::takeModeStats();
    BOOST_LOG(info) << "vdisplay: resume handled in "sv << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
                    << " ms, "sv << stats.mode_sets << " mode changes, "sv << stats.hdr_changes << " HDR switches"sv;

    return result;
  }

  proc_t::vdisplay_change_e proc_t::replug_virtual_display(const vdisplay_mode::display_mode_t &want, const rtsp_stream::launch_session_t &session, const char *why) {
    BOOST_LOG(info) << "vdisplay: "sv << why << ", re-creating "sv << _vdisplay_name;

    VDISPLAY::removeVirtualDisplay(_launch_session->display_guid);
    virtual_display = false;
    _vdisplay_name.clear();
    {
      std::lock_guard lock {display_name_mutex()};
      display_name.clear();
    }
    mode_changed_display.clear();

    if (!setup_virtual_display(want, session.client_screen, _launch_session->display_guid, true)) {
      return vdisplay_change_e::failed;
    }

    // A new display brings Windows' own arrangement back, the PC's monitors included, so the
    // display configuration a launch applies (e.g. this display only) is applied again
    _launch_session->width = want.width;
    _launch_session->height = want.height;
    _launch_session->fps = session.fps;
    _launch_session->enable_hdr = session.enable_hdr;
    configure_launch_display(*_launch_session);
    display_device::reset_persistence();
    return vdisplay_change_e::replugged;
  }

  bool proc_t::display_configured() const {
    return _launch_session && std::holds_alternative<display_device::SingleDisplayConfiguration>(display_device::parse_configuration(config::video, *_launch_session));
  }

  std::unique_lock<std::recursive_mutex> proc_t::try_lock_virtual_display() {
    return std::unique_lock {vdisplay_mutex(), std::try_to_lock};
  }

  std::string proc_t::virtual_display_name() const {
    return _vdisplay_name;
  }

  bool proc_t::reactivate_virtual_display() {
    if (!virtual_display || !_launch_session) {
      return false;
    }
    if (_vdisplay_id.empty()) {
      BOOST_LOG(warning) << "vdisplay: display_device has no id for the virtual display, so it can't be switched on again"sv;
      return false;
    }

    // The launch's display configuration again (e.g. this display only); without one, or for the
    // extended desktop, the display goes next to whatever is on, and nothing is switched off
    if (extends_desktop() || !display_configured()) {
      return fork_tweaks::vdisplay_extend::switch_on(_vdisplay_id);
    }
    BOOST_LOG(info) << "vdisplay: applying the launch's display configuration again"sv;
    configure_launch_display(*_launch_session);
    display_device::reset_persistence();
    return true;
  }

  bool proc_t::recreate_virtual_display() {
    if (!_launch_session || vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      return false;
    }
    VDISPLAY::takeModeStats();
    return replug_virtual_display(_vdisplay_mode, *_launch_session, "the virtual display didn't come back") == vdisplay_change_e::replugged;
  }

  void proc_t::virtual_display_back(const std::string &name, bool was_off) {
    if (name != _vdisplay_name) {
      BOOST_LOG(info) << "vdisplay: "sv << _vdisplay_name << " is now "sv << name;
      {
        // Capture follows it unless another display was picked with the shortcut
        std::lock_guard lock {display_name_mutex()};
        if (display_name == _vdisplay_name) {
          display_name = name;
        }
      }
      _vdisplay_name = name;
    }
    if (_vdisplay_id.empty()) {
      _vdisplay_id = display_device::map_display_name(name);
      if (!_vdisplay_id.empty()) {
        config::video.output_name = _vdisplay_id;
      }
    }
    if (!was_off || !_launch_session) {
      return;
    }

    auto nameW = platf::from_utf8(name);
    // A display configuration sets the mode it's configured for; without one, the display runs the
    // mode it was set up with
    if (!display_configured()) {
      int was_width = 0;
      int was_height = 0;
      int was_refresh_mhz = 0;
      if (!VDISPLAY::getDisplayMode(nameW.c_str(), was_width, was_height, was_refresh_mhz) || !vdisplay_mode::runs(_vdisplay_mode, was_width, was_height, was_refresh_mhz)) {
        BOOST_LOG(info) << "vdisplay: "sv << name << " came back in "sv << was_width << 'x' << was_height << 'x' << was_refresh_mhz << " mHz, setting "sv
                        << _vdisplay_mode.width << 'x' << _vdisplay_mode.height << 'x' << _vdisplay_mode.refresh_mhz << " mHz"sv;
        VDISPLAY::changeDisplaySettings(nameW.c_str(), _vdisplay_mode.width, _vdisplay_mode.height, _vdisplay_mode.refresh_mhz);
      }
    }

    // Windows placed it again; the isolated layout keeps whatever mode it runs now
    int width = 0;
    int height = 0;
    int refresh_mhz = 0;
    if (config::video.isolated_virtual_display_option && !extends_desktop() && VDISPLAY::getDisplayMode(nameW.c_str(), width, height, refresh_mhz)) {
      VDISPLAY::changeDisplaySettings2(nameW.c_str(), width, height, refresh_mhz, true);
    }

    if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic && VDISPLAY::getDisplayHDRByName(nameW.c_str()) != _launch_session->enable_hdr) {
      if (wait_for_display_settings()) {
        apply_virtual_display_hdr(name, _launch_session->enable_hdr, false);
      } else {
        BOOST_LOG(warning) << "vdisplay: Windows doesn't accept display changes now, HDR left as it is"sv;
      }
    }
  }

  void proc_t::start_vdisplay_recovery() {
    stop_vdisplay_recovery();
    if (!fork_tweaks::vdisplay_recovery_enabled || !virtual_display || !_launch_session) {
      return;
    }
    std::array<std::uint8_t, 16> guid;
    std::memcpy(guid.data(), &_launch_session->display_guid, guid.size());
    _vdisplay_recovery = std::make_unique<fork_tweaks::vdisplay_recovery::monitor_t>(guid);
  }

  void proc_t::stop_vdisplay_recovery() {
    // Waits for an action in progress; the monitor never waits for the lock, so it can't be waiting
    // for whoever stops it
    _vdisplay_recovery.reset();
  }
#endif

  std::string proc_t::stream_display() {
    std::lock_guard lock {display_name_mutex()};
    return display_name;
  }

  void proc_t::capture_moved(const std::string &display, bool chosen) {
    std::lock_guard lock {display_name_mutex()};
    if (chosen || !virtual_display || display_name.empty()) {
      display_name = display;
    }
  }

  int proc_t::running() {
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << "] within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      placebo = true;

    #if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      if (_process.native_exit_code() != 0) {
        system_tray::update_tray_launch_error(proc::proc.get_last_run_app_name(), _process.native_exit_code());
      }
    #endif

      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      terminate();
    }

    return 0;
  }

  void proc_t::resume() {
    BOOST_LOG(info) << "Session resuming for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {

        _env["APOLLO_APP_STATUS"] = "RESUMING";

        std::error_code ec;
        auto _state_resume_it = std::begin(cmd_list);

        for (; _state_resume_it != std::end(cmd_list); ++_state_resume_it) {
          auto &cmd = *_state_resume_it;

          // Skip empty commands
          if (cmd.do_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.do_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Resume Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }
  }

  void proc_t::pause() {
    if (!running()) {
      BOOST_LOG(info) << "Session already stopped, do not run pause commands.";
      return;
    }

    if (_app.terminate_on_pause) {
      BOOST_LOG(info) << "Terminating app [" << _app_name << "] when all clients are disconnected. Pause commands are skipped.";
      terminate();
      return;
    }

    BOOST_LOG(info) << "Session pausing for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {
        _env["APOLLO_APP_STATUS"] = "PAUSING";

        std::error_code ec;
        auto _state_pause_it = std::begin(cmd_list);

        for (; _state_pause_it != std::end(cmd_list); ++_state_pause_it) {
          auto &cmd = *_state_pause_it;

          // Skip empty commands
          if (cmd.undo_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.undo_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Pause Cmd: ["sv << cmd.undo_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.undo_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.undo_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif
  }

  void proc_t::terminate(bool immediate, bool needs_refresh) {
#ifdef _WIN32
    // [fork] Before anything changes the display it watches; then a resume waits until the app's
    // display is gone, rather than changing it as it goes
    stop_vdisplay_recovery();
    std::unique_lock vdisplay_lock {vdisplay_mutex()};
#endif

    std::error_code ec;
    placebo = false;

    if (!immediate) {
      terminate_process_group(_process, _process_group, _app.exit_timeout);
    }

    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    _env["APOLLO_APP_STATUS"] = "TERMINATING";

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

    bool has_run = _app_id > 0;

#ifdef _WIN32
    // Revert HDR state
    if (has_run && !mode_changed_display.empty()) {
      auto displayNameW = platf::from_utf8(mode_changed_display);
      if (VDISPLAY::setDisplayHDRByName(displayNameW.c_str(), initial_hdr)) {
        BOOST_LOG(info) << "HDR reverted for display " << mode_changed_display;
      } else {
        BOOST_LOG(info) << "HDR revert failed for display " << mode_changed_display;
      }
    }

    bool used_virtual_display = vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK && _launch_session && _launch_session->virtual_display;
    if (used_virtual_display) {
      if (VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
        BOOST_LOG(info) << "Virtual Display removed successfully";
      } else if (this->virtual_display) {
        BOOST_LOG(warning) << "Virtual Display remove failed";
      } else {
        BOOST_LOG(warning) << "Virtual Display remove failed, but it seems it was not created correctly either.";
      }
    }

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      if (used_virtual_display) {
        display_device::reset_persistence();
      } else {
        display_device::revert_configuration();
      }
#else
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      display_device::revert_configuration();
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif
    }

    // Load the configured output_name first
    // to prevent the value being write to empty when the initial terminate happens
    if (!has_run && initial_display.empty()) {
      initial_display = config::video.output_name;
    } else {
      // Restore output name to its original value
      config::video.output_name = initial_display;
    }

    _app_id = -1;
    _app_name.clear();
    _app = {};
    {
      std::lock_guard lock {display_name_mutex()};
      display_name.clear();
    }
#ifdef _WIN32
    _vdisplay_name.clear();
    _vdisplay_id.clear();
    _vdisplay_mode = {};
#endif
    initial_display.clear();
    mode_changed_display.clear();
    _launch_session.reset();
    virtual_display = false;
    allow_client_commands = false;

    if (_saved_input_config) {
      config::input = *_saved_input_config;
      _saved_input_config.reset();
    }

    fork_tweaks::restore_app_encoder_overrides();

    // [fork] Puts back NVIDIA's settings once the app has quit
    fork_tweaks::nv_stream::restore();
    _vdisplay_refresh_mhz = 0;

    if (needs_refresh) {
      refresh(config::stream.file_apps, false);
    }
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    return _app_name;
  }

  std::string proc_t::get_running_app_uuid() {
    return _app.uuid;
  }

  boost::process::environment proc_t::get_env() {
    return _env;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path;
    if (std::filesystem::exists(full_image_path)) {
      return full_image_path.string();
    } else if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    std::error_code code;
    if (!std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Migrate the applications stored in the file tree by merging in a new app.
   *
   * This function updates the application entries in *fileTree_p* using the data in *inputTree_p*.
   * If an app in the file tree does not have a UUID, one is generated and inserted.
   * If an app with the same UUID as the new app is found, it is replaced.
   * Additionally, empty keys (such as "prep-cmd" or "detached") and keys no longer needed ("launching", "index")
   * are removed from the input.
   *
   * Legacy versions of Sunshine/Apollo stored boolean and integer values as strings.
   * The following keys are converted:
   *   - Boolean keys: "exclude-global-prep-cmd", "elevated", "auto-detach", "wait-all",
   *                     "use-app-identity", "per-client-app-identity", "virtual-display"
   *   - Integer keys: "exit-timeout"
   *
   * A migration version is stored in the file tree (under "version") so that future changes can be applied.
   *
   * @param fileTree_p Pointer to the JSON object representing the file tree.
   * @param inputTree_p Pointer to the JSON object representing the new app.
   */
  void migrate_apps(nlohmann::json* fileTree_p, nlohmann::json* inputTree_p) {
    std::string new_app_uuid;

    if (inputTree_p) {
      // If the input contains a non-empty "uuid", use it; otherwise generate one.
      if (inputTree_p->contains("uuid") && !(*inputTree_p)["uuid"].get<std::string>().empty()) {
        new_app_uuid = (*inputTree_p)["uuid"].get<std::string>();
      } else {
        new_app_uuid = uuid_util::uuid_t::generate().string();
        (*inputTree_p)["uuid"] = new_app_uuid;
      }

      // Remove "prep-cmd" if empty.
      if (inputTree_p->contains("prep-cmd") && (*inputTree_p)["prep-cmd"].empty()) {
        inputTree_p->erase("prep-cmd");
      }

      // Remove "detached" if empty.
      if (inputTree_p->contains("detached") && (*inputTree_p)["detached"].empty()) {
        inputTree_p->erase("detached");
      }

      // Remove keys that are no longer needed.
      inputTree_p->erase("launching");
      inputTree_p->erase("index");
    }

    // Get the current apps array; if it doesn't exist, create one.
    nlohmann::json newApps = nlohmann::json::array();
    if (fileTree_p->contains("apps") && (*fileTree_p)["apps"].is_array()) {
      for (auto &app : (*fileTree_p)["apps"]) {
        // For apps without a UUID, generate one and remove "launching".
        if (!app.contains("uuid") || app["uuid"].get<std::string>().empty()) {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          app.erase("launching");
          newApps.push_back(std::move(app));
        } else {
          // If an app with the same UUID as the new app is found, replace it.
          if (!new_app_uuid.empty() && app["uuid"].get<std::string>() == new_app_uuid) {
            newApps.push_back(*inputTree_p);
            new_app_uuid.clear();
          } else {
            newApps.push_back(std::move(app));
          }
        }
      }
    }
    // If the new app's UUID has not been merged yet, add it.
    if (!new_app_uuid.empty() && inputTree_p) {
      newApps.push_back(*inputTree_p);
    }
    (*fileTree_p)["apps"] = newApps;
  }

  void migration_v2(nlohmann::json& fileTree) {
    static const int this_version = 2;
    // Determine the current migration version (default to 1 if not present).
    int file_version = 1;
    if (fileTree.contains("version")) {
      try {
        file_version = fileTree["version"].get<int>();
      } catch (const std::exception& e) {
        BOOST_LOG(info) << "Cannot parse apps.json version, treating as v1: " << e.what();
      }
    }

    // If the version is less than this_version, perform legacy conversion.
    if (file_version < this_version) {
      BOOST_LOG(info) << "Migrating app list from v1 to v2...";
      migrate_apps(&fileTree, nullptr);

      // List of keys to convert to booleans.
      std::vector<std::string> boolean_keys = {
        "allow-client-commands",
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all",
        "use-app-identity",
        "per-client-app-identity",
        "virtual-display"
      };

      // List of keys to convert to integers.
      std::vector<std::string> integer_keys = {
        "exit-timeout",
        "scale-factor"
      };

      // Walk through each app and convert legacy string values.
      for (auto &app : fileTree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key)) {
            auto& _key = app[key];
            if (_key.is_string()) {
              std::string s = _key.get<std::string>();
              std::transform(s.begin(), s.end(), s.begin(), ::tolower);  // Normalize to lowercase for comparison
              _key = (s == "true" || s == "on" || s == "yes");
            } else if (_key.is_array()) {
              // Check if the array contains at least one item and interpret the first element
              if (!_key.empty() && _key[0].is_string()) {
                std::string first = _key[0].get<std::string>();
                std::transform(first.begin(), first.end(), first.begin(), ::tolower);  // Normalize
                if (first == "on" || first == "true" || first == "yes") {
                  _key = true;
                } else if (first == "off" || first == "false" || first == "no") {
                  _key = false;
                } else {
                  _key = false;  // Default for unknown values
                }
              } else {
                _key = false;  // Treat empty arrays or non-string first elements as false
              }
            } else {
              // Fallback: Treat truthy/falsey cases
              if (_key.is_boolean()) {
                // Leave booleans as they are
              } else if (_key.is_number()) {
                _key = (_key.get<double>() != 0);  // Non-zero numbers are truthy
              } else if (_key.is_null()) {
                _key = false;  // Null is false
              } else {
                _key = !_key.empty();  // Non-empty objects/arrays are truthy, empty ones are falsey
              }
            }
          }
        }

        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            std::string s = app[key].get<std::string>();
            app[key] = std::stoi(s);
          }
        }

        // For each entry in the "prep-cmd" array, convert "elevated" if necessary.
        if (app.contains("prep-cmd") && app["prep-cmd"].is_array()) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              std::string s = prep["elevated"].get<std::string>();
              prep["elevated"] = (s == "true");
            }
          }
        }
      }

      // Update migration version to this_version.
      fileTree["version"] = this_version;

      BOOST_LOG(info) << "Migrated app list from v1 to v2.";
    }
  }

  void migrate(nlohmann::json& fileTree, const std::string& fileName) {
    int last_version = 2;

    int file_version = 0;
    if (fileTree.contains("version")) {
      file_version = fileTree["version"].get<int>();
    }

    if (file_version < last_version) {
      migration_v2(fileTree);
      file_handler::write_file(fileName.c_str(), fileTree.dump(4));
    }
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {

    // Prepare environment variables.
    auto this_env = boost::this_process::environment();

    std::set<std::string> ids;
    std::vector<proc::ctx_t> apps;
    int i = 0;

    size_t fail_count = 0;
    do {
      // Read the JSON file into a tree.
      nlohmann::json tree;
      try {
        std::string content = file_handler::read_file(file_name.c_str());
        tree = nlohmann::json::parse(content);
      } catch (const std::exception& e) {
        BOOST_LOG(warning) << "Couldn't read apps.json properly! Apps will not be loaded."sv;
        break;
      }

      try {
        migrate(tree, file_name);

        if (tree.contains("env") && tree["env"].is_object()) {
          for (auto &item : tree["env"].items()) {
            this_env[item.key()] = parse_env_val(this_env, item.value().get<std::string>());
          }
        }

        // Ensure the "apps" array exists.
        if (!tree.contains("apps") || !tree["apps"].is_array()) {
          BOOST_LOG(warning) << "No apps were defined in apps.json!!!"sv;
          break;
        }

        // Iterate over each application in the "apps" array.
        for (auto &app_node : tree["apps"]) {
          proc::ctx_t ctx;
          ctx.idx = std::to_string(i);
          ctx.uuid = app_node.at("uuid");

          // Build the list of preparation commands.
          std::vector<proc::cmd_t> prep_cmds;
          bool exclude_global_prep = app_node.value("exclude-global-prep-cmd", false);
          if (!exclude_global_prep) {
            prep_cmds.reserve(config::sunshine.prep_cmds.size());
            for (auto &prep_cmd : config::sunshine.prep_cmds) {
              auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(prep_cmd.elevated)
              );
            }
          }
          if (app_node.contains("prep-cmd") && app_node["prep-cmd"].is_array()) {
            for (auto &prep_node : app_node["prep-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of pause/resume commands.
          std::vector<proc::cmd_t> state_cmds;
          bool exclude_global_state_cmds = app_node.value("exclude-global-state-cmd", false);
          if (!exclude_global_state_cmds) {
            state_cmds.reserve(config::sunshine.state_cmds.size());
            for (auto &state_cmd : config::sunshine.state_cmds) {
              auto do_cmd = parse_env_val(this_env, state_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, state_cmd.undo_cmd);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(state_cmd.elevated)
              );
            }
          }
          if (app_node.contains("state-cmd") && app_node["state-cmd"].is_array()) {
            for (auto &prep_node : app_node["state-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of detached commands.
          std::vector<std::string> detached;
          if (app_node.contains("detached") && app_node["detached"].is_array()) {
            for (auto &detached_val : app_node["detached"]) {
              detached.emplace_back(parse_env_val(this_env, detached_val.get<std::string>()));
            }
          }

          // Process other fields.
          if (app_node.contains("output"))
            ctx.output = parse_env_val(this_env, app_node.value("output", ""));
          std::string name = parse_env_val(this_env, app_node.value("name", ""));
          if (app_node.contains("cmd"))
            ctx.cmd = parse_env_val(this_env, app_node.value("cmd", ""));
          if (app_node.contains("working-dir")) {
            ctx.working_dir = parse_env_val(this_env, app_node.value("working-dir", ""));
    #ifdef _WIN32
            // The working directory, unlike the command itself, should not be quoted.
            boost::erase_all(ctx.working_dir, "\"");
            ctx.working_dir += '\\';
    #endif
          }
          if (app_node.contains("image-path"))
            ctx.image_path = parse_env_val(this_env, app_node.value("image-path", ""));

          ctx.elevated = app_node.value("elevated", false);
          ctx.auto_detach = app_node.value("auto-detach", true);
          ctx.wait_all = app_node.value("wait-all", true);
          ctx.exit_timeout = std::chrono::seconds { app_node.value("exit-timeout", 5) };
          ctx.virtual_display = app_node.value("virtual-display", false);
          ctx.scale_factor = app_node.value("scale-factor", 100);
          ctx.use_app_identity = app_node.value("use-app-identity", false);
          ctx.per_client_app_identity = app_node.value("per-client-app-identity", false);
          ctx.allow_client_commands = app_node.value("allow-client-commands", true);
          ctx.terminate_on_pause = app_node.value("terminate-on-pause", false);
          ctx.gamepad = app_node.value("gamepad", "");

          // Calculate a unique application id.
          auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
          if (ids.count(std::get<0>(possible_ids)) == 0) {
            ctx.id = std::get<0>(possible_ids);
          } else {
            ctx.id = std::get<1>(possible_ids);
          }
          ids.insert(ctx.id);

          ctx.name = std::move(name);
          ctx.prep_cmds = std::move(prep_cmds);
          ctx.state_cmds = std::move(state_cmds);
          ctx.detached = std::move(detached);

          apps.emplace_back(std::move(ctx));
        }

        fail_count = 0;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Error happened during app loading: "sv << e.what();

        fail_count += 1;

        if (fail_count >= 3) {
          // No hope for recovering
          BOOST_LOG(warning) << "Couldn't parse/migrate apps.json properly! Apps will not be loaded."sv;
          break;
        }

        BOOST_LOG(warning) << "App format is still invalid! Trying to re-migrate the app list..."sv;

        // Always try migrating from scratch when error happened
        tree["version"] = 0;

        try {
          migrate(tree, file_name);
        } catch (std::exception &e) {
          BOOST_LOG(error) << "Error happened during migration: "sv << e.what();
          break;
        }

        this_env = boost::this_process::environment();
        ids.clear();
        apps.clear();
        i = 0;

        continue;
      }

      break;
    } while (fail_count < 3);

    if (fail_count > 0) {
      BOOST_LOG(warning) << "No applications configured, adding fallback Desktop entry.";
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = FALLBACK_DESKTOP_UUID; // Placeholder UUID
      ctx.name = "Desktop (fallback)";
      ctx.image_path = parse_env_val(this_env, "desktop-alt.png");
      ctx.virtual_display = false;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false; // Desktop doesn't have a specific command to wait for
      ctx.exit_timeout = 5s;

      // Calculate unique ID
      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      } else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }

    // Virtual Display entry
  #ifdef _WIN32
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = VIRTUAL_DISPLAY_UUID;
      ctx.name = "Virtual Display";
      ctx.image_path = parse_env_val(this_env, "virtual_desktop.png");
      ctx.virtual_display = true;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false;
      ctx.exit_timeout = 5s;

      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      }
      else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }
  #endif

    if (config::input.enable_input_only_mode) {
      // Input Only entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = REMOTE_INPUT_UUID;
        ctx.name = "Remote Input";
        ctx.image_path = parse_env_val(this_env, "input_only.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = true; // There's no need to keep an active input only session ongoing

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        input_only_app_id_str = ctx.id;
        input_only_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }

      // Terminate entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = TERMINATE_APP_UUID;
        ctx.name = "Terminate";
        ctx.image_path = parse_env_val(this_env, "terminate.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = false;

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        // ids.insert(ctx.id);

        terminate_app_id_str = ctx.id;
        terminate_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }
    }

    return proc::proc_t {
      std::move(this_env),
      std::move(apps)
    };
  }

  void refresh(const std::string &file_name, bool needs_terminate) {
    if (needs_terminate) {
      proc.terminate(false, false);
    }

  #ifdef _WIN32
    size_t fail_count = 0;
    while (fail_count < 5 && vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      initVDisplayDriver();
      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        break;
      }

      fail_count += 1;
      std::this_thread::sleep_for(1s);
    }
  #endif

    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
      // [fork] Capture reads display_name under this lock; no recovery monitor runs here, as the app
      // was terminated first
      std::lock_guard lock {display_name_mutex()};
      proc = std::move(*proc_opt);
    }
  }
}  // namespace proc
