/**
 * @file src/fork/encoder_probe_reuse.cpp
 * @brief Fork-only: skip the full encoder probe on app launch when only the display layout changed.
 */
// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// local includes
#include "encoder_probe_reuse.h"
#include "fork_config.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/video.h"

#ifdef _WIN32
  #include <dxgi.h>
#endif

using namespace std::literals;

namespace video {
  // Defined in video.cpp but not declared in video.h; the full probe uses the same two calls.
  void reset_display(std::shared_ptr<platf::display_t> &disp, const platf::mem_type_e &type, const std::string &display_name, const config_t &config);
  int validate_config(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, const config_t &config);
}  // namespace video

namespace fork_tweaks {
  namespace {
    struct probe_snapshot_t {
      const video::encoder_t *encoder;
      std::array<unsigned long long, 3> capabilities;  // H.264, HEVC, AV1
      std::string gpus;

      bool operator==(const probe_snapshot_t &) const = default;
    };

    std::mutex mutex;
    // The probe result as it was before the most recent full probe, to check that probing gives a stable answer.
    std::optional<probe_snapshot_t> previous;

    // Hardware adapters with their driver versions, or "" if they can't be listed.
    std::string gpu_fingerprint() {
#ifdef _WIN32
      IDXGIFactory1 *factory = nullptr;
      if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory))) {
        return {};
      }

      std::vector<std::string> adapters;
      IDXGIAdapter1 *adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        // Skip software adapters, including the Microsoft Basic Render Driver (vendor 0x1414)
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId != 0x1414) {
          LARGE_INTEGER driver_version {};
          adapter->CheckInterfaceSupport(IID_IDXGIDevice, &driver_version);

          std::ostringstream id;
          id << std::hex << desc.AdapterLuid.HighPart << '.' << desc.AdapterLuid.LowPart << ':' << desc.VendorId << ':' << desc.DeviceId
             << ':' << desc.SubSysId << ':' << desc.Revision << ':' << driver_version.QuadPart;
          adapters.push_back(id.str());
        }
        adapter->Release();
      }
      factory->Release();

      std::sort(adapters.begin(), adapters.end());
      std::string fingerprint;
      for (const auto &id : adapters) {
        fingerprint += id + ';';
      }
      return fingerprint;
#else
      return {};
#endif
    }

    probe_snapshot_t snapshot(const video::encoder_t &encoder) {
      return {
        &encoder,
        {encoder.h264.capabilities.to_ullong(), encoder.hevc.capabilities.to_ullong(), encoder.av1.capabilities.to_ullong()},
        gpu_fingerprint(),
      };
    }
  }  // namespace

  bool try_reuse_encoder_probe(const video::encoder_t &encoder) {
    if (!encoder_probe_reuse) {
      return false;
    }

    std::lock_guard lg {mutex};

    auto current = snapshot(encoder);
    if (!previous || current.gpus.empty() || *previous != current) {
      auto reason = !previous                      ? "no earlier probe to compare with"sv :
                    current.gpus.empty()           ? "couldn't list the GPUs"sv :
                    previous->gpus != current.gpus ? "GPUs or drivers changed"sv :
                                                     "the last two probes differed"sv;
      BOOST_LOG(info) << "fork: running a full encoder probe ("sv << reason << ')';
      previous = current;
      return false;
    }

    // Same GPUs and a stable probe result. Check that the encoder still works on the display we're about to capture.
    auto start = std::chrono::steady_clock::now();
    int video_format = encoder.hevc[video::encoder_t::PASSED] ? 1 : 0;
    video::config_t config {1920, 1080, 60, 1000, 1, 0, 1, video_format, 0, 0};

    std::shared_ptr<platf::display_t> disp;
    video::reset_display(disp, encoder.platform_formats->dev_type, display_device::map_output_name(config::video.output_name), config);
    bool ok = disp && video::validate_config(disp, encoder, config) >= 0;
    disp.reset();

    if (!ok) {
      previous.reset();
      BOOST_LOG(warning) << "fork: test encode on the current display failed, running a full encoder probe"sv;
      return false;
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    BOOST_LOG(info) << "fork: reusing encoder probe for ["sv << encoder.name << "], "sv << (video_format ? "HEVC"sv : "H.264"sv)
                    << " test encode passed in "sv << ms << " ms"sv;
    return true;
  }

  void invalidate_encoder_probe(std::string_view reason) {
    std::lock_guard lg {mutex};
    if (previous) {
      previous.reset();
      BOOST_LOG(warning) << "fork: next launch will run a full encoder probe: "sv << reason;
    }
  }
}  // namespace fork_tweaks
