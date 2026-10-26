#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>
#include <setupapi.h>
#include <initguid.h>
#include <combaseapi.h>
#include <thread>

#include <wrl/client.h>
#include <dxgi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <dxgi1_6.h>

#include "display_layout.h"
#include "src/fork/vdisplay_mode.h"
#include "misc.h"
#include "src/logging.h"
#include "virtual_display.h"

using namespace std::literals;

namespace VDISPLAY {
// {dff7fd29-5b75-41d1-9731-b32a17a17104}
// static const GUID DEFAULT_DISPLAY_GUID = { 0xdff7fd29, 0x5b75, 0x41d1, { 0x97, 0x31, 0xb3, 0x2a, 0x17, 0xa1, 0x71, 0x04 } };

HANDLE LUZVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;

// [fork] Each successful mode or layout change makes the screen blank for a moment; counted so a
// launch can log how many it took
static std::atomic<int> s_modeSets {0};
static std::atomic<int> s_hdrChanges {0};

mode_stats_t takeModeStats() {
	return {s_modeSets.exchange(0), s_hdrChanges.exchange(0)};
}

static std::string narrow(const wchar_t* s) {
	return platf::to_utf8(std::wstring_view(s));
}

static bool sameLuid(const LUID& a, const LUID& b) {
	return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static bool queryActiveConfig(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
	UINT32 pathCount = 0;
	UINT32 modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
		return false;
	}

	paths.resize(pathCount);
	modes.resize(modeCount);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
		return false;
	}

	paths.resize(pathCount);
	modes.resize(modeCount);
	return true;
}

static bool sourceNameOf(const DISPLAYCONFIG_PATH_INFO& path, std::wstring& name) {
	DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
	sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
	sourceName.header.size = sizeof(sourceName);
	sourceName.header.adapterId = path.sourceInfo.adapterId;
	sourceName.header.id = path.sourceInfo.id;

	if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
		return false;
	}

	name = sourceName.viewGdiDeviceName;
	return true;
}

static DISPLAYCONFIG_MODE_INFO* sourceModeOf(const DISPLAYCONFIG_PATH_INFO& path, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
	for (auto& mode : modes) {
		if (
			mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
			sameLuid(mode.adapterId, path.sourceInfo.adapterId) &&
			mode.id == path.sourceInfo.id
		) {
			return &mode;
		}
	}
	return nullptr;
}

static LONG applyConfig(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
	LONG status = SetDisplayConfig(
		(UINT32)paths.size(),
		paths.data(),
		(UINT32)modes.size(),
		modes.data(),
		SDC_APPLY
		| SDC_USE_SUPPLIED_DISPLAY_CONFIG
		| SDC_SAVE_TO_DATABASE
	);
	if (status == ERROR_SUCCESS) {
		s_modeSets++;
	}
	return status;
}

// [fork] Moves the other displays so the virtual one sits alone at the lower right. The geometry
// is in display_layout.h, where it is tested.
static void applyIsolatedLayout(const wchar_t* deviceName, std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
	std::vector<display_layout::rect_t> displays;
	std::vector<DISPLAYCONFIG_MODE_INFO*> sourceModes;
	bool foundVirtual = false;

	for (const auto& path : paths) {
		std::wstring name;
		if (!sourceNameOf(path, name)) {
			continue;
		}

		auto* mode = sourceModeOf(path, modes);
		if (!mode) {
			continue;
		}

		display_layout::rect_t rect {
			mode->sourceMode.position.x,
			mode->sourceMode.position.y,
			(int)mode->sourceMode.width,
			(int)mode->sourceMode.height,
		};

		// The virtual display goes first
		if (!foundVirtual && name == deviceName) {
			displays.insert(displays.begin(), rect);
			sourceModes.insert(sourceModes.begin(), mode);
			foundVirtual = true;
		} else {
			displays.push_back(rect);
			sourceModes.push_back(mode);
		}
	}

	if (!foundVirtual) {
		BOOST_LOG(warning) << "vdisplay: isolated layout skipped, "sv << narrow(deviceName) << " isn't active"sv;
		return;
	}

	auto layout = display_layout::isolated_layout(displays, 0);
	BOOST_LOG(debug) << "vdisplay: isolated layout before:\n"sv << display_layout::describe(displays);
	if (!layout) {
		BOOST_LOG(warning) << "vdisplay: isolated layout skipped, the displays can't be arranged without overlapping"sv;
		return;
	}
	BOOST_LOG(debug) << "vdisplay: isolated layout after:\n"sv << display_layout::describe(*layout);

	for (std::size_t i = 0; i < sourceModes.size(); ++i) {
		sourceModes[i]->sourceMode.position.x = (*layout)[i].x;
		sourceModes[i]->sourceMode.position.y = (*layout)[i].y;
	}

	if (applyConfig(paths, modes) != ERROR_SUCCESS) {
		BOOST_LOG(warning) << "vdisplay: failed to apply the isolated layout"sv;
	} else {
		BOOST_LOG(info) << "vdisplay: isolated layout applied"sv;
	}
}

LONG changeDisplaySettings2(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated) {
	std::vector<DISPLAYCONFIG_PATH_INFO> pathArray;
	std::vector<DISPLAYCONFIG_MODE_INFO> modeArray;
	if (!queryActiveConfig(pathArray, modeArray)) {
		BOOST_LOG(warning) << "vdisplay: failed to query the display configuration"sv;
		return ERROR_INVALID_PARAMETER;
	}

	if (bApplyIsolated) {
		applyIsolatedLayout(deviceName, pathArray, modeArray);
	}

	// After performing the isolated display movements, do the regular movements
	for (auto& path : pathArray) {
		std::wstring name;
		if (!sourceNameOf(path, name) || name != deviceName) {
			continue;
		}

		auto* mode = sourceModeOf(path, modeArray);
		if (!mode) {
			BOOST_LOG(warning) << "vdisplay: no source mode for "sv << narrow(deviceName);
			return ERROR_INVALID_PARAMETER;
		}

		auto* sourceMode = &mode->sourceMode;
		BOOST_LOG(debug) << "vdisplay: current mode "sv << sourceMode->width << 'x' << sourceMode->height
			<< ", setting "sv << width << 'x' << height << 'x' << refresh_rate << " mHz"sv;

		sourceMode->width = width;
		sourceMode->height = height;
		path.targetInfo.refreshRate = {(UINT32)refresh_rate, 1000};

		// [fork] Without its target mode, Windows has to pick the signal timing for the new size and
		// rate, as libdisplaydevice does; kept, the old timing could stay with the desktop scaled into it
		path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

		LONG status = applyConfig(pathArray, modeArray);
		if (status != ERROR_SUCCESS) {
			BOOST_LOG(warning) << "vdisplay: failed to apply display settings ("sv << status << ')';
		} else {
			BOOST_LOG(debug) << "vdisplay: display settings updated"sv;
		}

		return status;
	}

	BOOST_LOG(warning) << "vdisplay: display not found: "sv << narrow(deviceName);
	return ERROR_DEVICE_NOT_CONNECTED;
}

LONG changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate) {
	// [fork] One mode change when the exact one takes; each change blanks the screen
	if (changeDisplaySettings2(deviceName, width, height, refresh_rate) == ERROR_SUCCESS) {
		int nowWidth = 0;
		int nowHeight = 0;
		int nowRefresh = 0;
		if (getDisplayMode(deviceName, nowWidth, nowHeight, nowRefresh) && vdisplay_mode::runs({(uint32_t) width, (uint32_t) height, (uint32_t) refresh_rate}, nowWidth, nowHeight, nowRefresh)) {
			return ERROR_SUCCESS;
		}
		BOOST_LOG(debug) << "vdisplay: "sv << narrow(deviceName) << " runs "sv << nowWidth << 'x' << nowHeight << 'x' << nowRefresh << " mHz after the exact change, trying the baseline mode"sv;
	}

	DEVMODEW devMode = {};
	devMode.dmSize = sizeof(devMode);

	// Old method to set at least baseline refresh rate
	if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
		DWORD targetRefreshRate = refresh_rate / 1000;
		DWORD altRefreshRate = targetRefreshRate;

		if (refresh_rate % 1000) {
			if (refresh_rate % 1000 >= 900) {
				targetRefreshRate += 1;
			} else {
				altRefreshRate += 1;
			}
		} else {
			altRefreshRate -= 1;
		}

		BOOST_LOG(debug) << "vdisplay: applying baseline mode "sv << width << 'x' << height << 'x' << targetRefreshRate << " for "sv << narrow(deviceName);

		devMode.dmPelsWidth = width;
		devMode.dmPelsHeight = height;
		devMode.dmDisplayFrequency = targetRefreshRate;
		devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

		auto res = ChangeDisplaySettingsExW(deviceName, &devMode, NULL, CDS_UPDATEREGISTRY, NULL);

		if (res != DISP_CHANGE_SUCCESSFUL) {
			BOOST_LOG(debug) << "vdisplay: baseline mode failed ("sv << res << "), trying "sv << altRefreshRate << " Hz"sv;
			devMode.dmDisplayFrequency = altRefreshRate;
			res = ChangeDisplaySettingsExW(deviceName, &devMode, NULL, CDS_UPDATEREGISTRY, NULL);
			if (res != DISP_CHANGE_SUCCESSFUL) {
				BOOST_LOG(debug) << "vdisplay: alternate baseline mode failed ("sv << res << ')';
			}
		}

		if (res == DISP_CHANGE_SUCCESSFUL) {
			s_modeSets++;
		}
	}

	// Use new method to set refresh rate if fine tuned
	return changeDisplaySettings2(deviceName, width, height, refresh_rate);
}

// [fork]
bool getDisplayMode(const wchar_t* deviceName, int& width, int& height, int& refresh_mhz) {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!queryActiveConfig(paths, modes)) {
		return false;
	}

	for (const auto& path : paths) {
		std::wstring name;
		if (!sourceNameOf(path, name) || name != deviceName) {
			continue;
		}

		auto* mode = sourceModeOf(path, modes);
		if (!mode) {
			return false;
		}

		width = (int)mode->sourceMode.width;
		height = (int)mode->sourceMode.height;

		const auto& rate = path.targetInfo.refreshRate;
		refresh_mhz = rate.Denominator ? (int)((uint64_t)rate.Numerator * 1000 / rate.Denominator) : 0;
		return true;
	}

	return false;
}

bool findDisplayIds(const wchar_t* displayName, LUID& adapterId, uint32_t& targetId) {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!queryActiveConfig(paths, modes)) {
		return false;
	}

	for (const auto& path : paths) {
		std::wstring name;
		if (sourceNameOf(path, name) && name == displayName) {
			adapterId = path.sourceInfo.adapterId;
			targetId = path.targetInfo.id;
			return true;
		}
	}

	return false;
}

// [fork] Finds the display among every adapter's outputs. A virtual display's DXGI output belongs
// to the GPU that renders it, not to the adapter CCD reports for it, so matching that adapter
// never found it and HDR always read as off. A new factory each time: one lists the outputs
// there were when it was made.
static bool findOutput(const wchar_t* displayName, Microsoft::WRL::ComPtr<IDXGIOutput>& found) {
	Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
	if (FAILED(hr)) {
		BOOST_LOG(warning) << "vdisplay: CreateDXGIFactory1 failed, hr=0x"sv << std::hex << hr;
		return false;
	}

	std::wstring_view displayName_view{displayName};
	for (UINT adapterIdx = 0; ; ++adapterIdx) {
		Microsoft::WRL::ComPtr<IDXGIAdapter1> currentAdapter;
		hr = dxgiFactory->EnumAdapters1(adapterIdx, currentAdapter.ReleaseAndGetAddressOf());

		if (hr == DXGI_ERROR_NOT_FOUND) {
			break; // No more adapters
		}
		if (FAILED(hr)) {
			BOOST_LOG(warning) << "vdisplay: EnumAdapters1 failed for index "sv << adapterIdx;
			break;
		}

		for (UINT outputIdx = 0; ; ++outputIdx) {
			Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
			hr = currentAdapter->EnumOutputs(outputIdx, dxgiOutput.ReleaseAndGetAddressOf());

			if (hr == DXGI_ERROR_NOT_FOUND) {
				break; // No more outputs on this adapter
			}
			if (FAILED(hr) || !dxgiOutput) {
				continue; // Error, try next output
			}

			DXGI_OUTPUT_DESC dxgiOutputDesc;
			hr = dxgiOutput->GetDesc(&dxgiOutputDesc);
			if (SUCCEEDED(hr) && displayName_view == dxgiOutputDesc.DeviceName) {
				found = dxgiOutput;
				return true;
			}
		}
	}

	return false;
}

bool getDisplayHDR(const wchar_t* displayName) {
	Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
	if (!findOutput(displayName, dxgiOutput)) {
		BOOST_LOG(debug) << "vdisplay: "sv << narrow(displayName) << " not found among DXGI outputs"sv;
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIOutput6> dxgiOutput6;
	HRESULT hr = dxgiOutput.As(&dxgiOutput6);
	if (FAILED(hr) || !dxgiOutput6) {
		BOOST_LOG(debug) << "vdisplay: no IDXGIOutput6 for "sv << narrow(displayName) << ", can't read its HDR state"sv;
		return false;
	}

	DXGI_OUTPUT_DESC1 outputDesc1;
	hr = dxgiOutput6->GetDesc1(&outputDesc1);
	if (FAILED(hr)) {
		BOOST_LOG(debug) << "vdisplay: GetDesc1 (output) failed for "sv << narrow(displayName);
		return false;
	}

	return outputDesc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

bool waitForOutput(const wchar_t* displayName, std::chrono::milliseconds timeout) {
	auto start = std::chrono::steady_clock::now();
	Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
	while (!findOutput(displayName, dxgiOutput)) {
		if (std::chrono::steady_clock::now() - start > timeout) {
			return false;
		}
		Sleep(10);
	}
	return true;
}

bool setDisplayHDR(const LUID& adapterId, const uint32_t& targetId, bool enableAdvancedColor) {
	DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setHdrInfo = {};
	setHdrInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
	setHdrInfo.header.size = sizeof(setHdrInfo);
	setHdrInfo.header.adapterId = adapterId;
	setHdrInfo.header.id = targetId;
	setHdrInfo.enableAdvancedColor = enableAdvancedColor;

	bool ok = DisplayConfigSetDeviceInfo(&setHdrInfo.header) == ERROR_SUCCESS;
	if (ok) {
		s_hdrChanges++;
	}
	return ok;
}

bool getDisplayHDRByName(const wchar_t* displayName) {
	return getDisplayHDR(displayName);
}

bool setDisplayHDRByName(const wchar_t* displayName, bool enableAdvancedColor) {
	LUID adapterId;
	uint32_t targetId;

	if (!findDisplayIds(displayName, adapterId, targetId)) {
		return false;
	}

	return setDisplayHDR(adapterId, targetId, enableAdvancedColor);
}

void closeVDisplayDevice() {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return;
	}

	CloseHandle(LUZVDA_DRIVER_HANDLE);

	LUZVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
}

DRIVER_STATUS openVDisplayDevice() {
	DWORD error = 0;
	uint32_t retryInterval = 20;
	while (true) {
		LUZVDA_DRIVER_HANDLE = LUZVDA::openDevice(error);
		if (LUZVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
			break;
		}
		if (retryInterval > 320) {
			BOOST_LOG(warning) << "vdisplay: failed to open the LuzVDA driver, error "sv << error;
			return DRIVER_STATUS::FAILED;
		}
		retryInterval *= 2;
		Sleep(retryInterval);
	}

	LUZVDA_VERSION version {};
	error = LUZVDA::getVersion(LUZVDA_DRIVER_HANDLE, version);
	if (error || !LUZVDA::isCompatible(version)) {
		BOOST_LOG(warning) << "vdisplay: the LuzVDA driver speaks protocol "sv << version.major << '.' << version.minor
			<< " (error "sv << error << "), Apollo needs "sv << LUZVDA_PROTOCOL_MAJOR << '.' << LUZVDA_PROTOCOL_MINOR << " or a later minor"sv;
		closeVDisplayDevice();
		return DRIVER_STATUS::VERSION_INCOMPATIBLE;
	}

	BOOST_LOG(info) << "vdisplay: LuzVDA "sv << version.driver[0] << '.' << version.driver[1] << '.' << version.driver[2] << '.' << version.driver[3]
		<< ", protocol "sv << version.major << '.' << version.minor;

	// Apollo is the driver's only user, so any display it has was left by an Apollo that stopped
	// without removing it
	removeAllVirtualDisplays();

	return DRIVER_STATUS::OK;
}

bool startPingThread(std::function<void()> failCb) {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	LUZVDA_WATCHDOG watchdog {};
	if (DWORD error = LUZVDA::getWatchdog(LUZVDA_DRIVER_HANDLE, watchdog)) {
		BOOST_LOG(warning) << "vdisplay: failed to read the driver's watchdog, error "sv << error;
		return false;
	}
	BOOST_LOG(info) << "vdisplay: watchdog timeout "sv << watchdog.timeout_s << " s, countdown "sv << watchdog.countdown_s;

	if (watchdog.timeout_s) {
		auto sleepInterval = watchdog.timeout_s * 1000 / 3;
		std::thread ping_thread([sleepInterval, failCb = std::move(failCb)]{
			// [fork] Give up after 4 failures in a row; one success starts the count again
			uint8_t fail_count = 0;
			for (;;) {
				if (LUZVDA::ping(LUZVDA_DRIVER_HANDLE) == 0) {
					fail_count = 0;
				} else {
					fail_count += 1;
					if (fail_count > 3) {
						failCb();
						return;
					}
				}
				Sleep(sleepInterval);
			}
		});

		ping_thread.detach();
	}

	return true;
}

bool setRenderAdapterByName(const std::wstring& adapterName) {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
	if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc;
	int i = 0;
	while (SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
		i += 1;

		if (!SUCCEEDED(adapter->GetDesc(&desc))) {
			continue;
		}

		if (std::wstring_view(desc.Description) != adapterName) {
			continue;
		}

		if (LUZVDA::setRenderAdapter(LUZVDA_DRIVER_HANDLE, desc.AdapterLuid) == 0) {
			return true;
		}
	}

	return false;
}

// [fork] Without an adapter_name, render on the hardware GPU with the most dedicated video memory.
// The encoder runs on the GPU that renders the captured display, so this is also the one that
// encodes. Integrated GPUs have little or no dedicated memory.
bool setRenderAdapterAuto() {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
	if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}

	constexpr UINT microsoftVendorId = 0x1414;  // Basic Render Driver, WARP

	DXGI_ADAPTER_DESC1 best {};
	bool found = false;
	Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc;
		if (FAILED(adapter->GetDesc1(&desc))) {
			continue;
		}
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId == microsoftVendorId) {
			continue;
		}
		if (!found || desc.DedicatedVideoMemory > best.DedicatedVideoMemory) {
			best = desc;
			found = true;
		}
	}

	if (!found) {
		BOOST_LOG(warning) << "vdisplay: no hardware GPU found to render the virtual display"sv;
		return false;
	}

	if (DWORD error = LUZVDA::setRenderAdapter(LUZVDA_DRIVER_HANDLE, best.AdapterLuid)) {
		BOOST_LOG(warning) << "vdisplay: failed to render the virtual display on "sv << narrow(best.Description) << ", error "sv << error;
		return false;
	}

	BOOST_LOG(info) << "vdisplay: rendering on "sv << narrow(best.Description) << " ("sv
		<< best.DedicatedVideoMemory / (1024 * 1024) << " MB dedicated)"sv;
	return true;
}

bool setRenderAdapter(const std::string& adapterName) {
	if (!adapterName.empty()) {
		return setRenderAdapterByName(platf::from_utf8(adapterName));
	}
	return setRenderAdapterAuto();
}

// [fork] The active path to the display at this adapter and target. Target ids are only unique per
// adapter; the id alone is the fallback in case the path names another adapter than the driver
// reported.
static const DISPLAYCONFIG_PATH_INFO* pathFor(const std::vector<DISPLAYCONFIG_PATH_INFO>& paths, const LUID& adapter, uint32_t targetId) {
	for (const auto& path : paths) {
		if (path.targetInfo.id == targetId && sameLuid(path.targetInfo.adapterId, adapter)) {
			return &path;
		}
	}
	for (const auto& path : paths) {
		if (path.targetInfo.id == targetId) {
			return &path;
		}
	}
	return nullptr;
}

// [fork] The GDI name of the display at this adapter and target, once Windows has given it a path
static bool displayNameFor(const LUID& adapter, uint32_t targetId, std::wstring& name) {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!queryActiveConfig(paths, modes)) {
		return false;
	}

	const auto* found = pathFor(paths, adapter, targetId);
	return found && sourceNameOf(*found, name);
}

// [fork] Where the driver has the display with this GUID
static bool findListed(const GUID& guid, LUID& adapter, uint32_t& targetId) {
	LUZVDA_LIST list;
	if (LUZVDA::listDisplays(LUZVDA_DRIVER_HANDLE, list)) {
		return false;
	}
	auto want = LUZVDA::toProtocol(guid);
	for (uint32_t i = 0; i < list.count; ++i) {
		if (std::memcmp(list.displays[i].monitor_guid.bytes, want.bytes, sizeof(want.bytes)) == 0) {
			adapter = LUZVDA::fromProtocol(list.displays[i].adapter);
			targetId = list.displays[i].target_id;
			return true;
		}
	}
	return false;
}

static bool listed(const GUID& guid) {
	LUID adapter;
	uint32_t targetId;
	return findListed(guid, adapter, targetId);
}

static bool hasPath(const LUID& adapter, uint32_t targetId) {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!queryActiveConfig(paths, modes)) {
		return false;
	}
	for (const auto& path : paths) {
		if (path.targetInfo.id == targetId && sameLuid(path.targetInfo.adapterId, adapter)) {
			return true;
		}
	}
	return false;
}

std::wstring createVirtualDisplay(const LUZVDA_DISPLAY_DESC& desc) {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return std::wstring();
	}

	auto start = std::chrono::steady_clock::now();
	GUID guid;
	std::memcpy(&guid, desc.monitor_guid.bytes, sizeof(guid));

	LUZVDA_ADD_OUT output {};
	DWORD error = LUZVDA::addDisplay(LUZVDA_DRIVER_HANDLE, desc, output);
	if (error && listed(guid)) {
		// Connected with another description, e.g. from the client's other screen: Windows reads the
		// description only when a display arrives, so it has to arrive again
		BOOST_LOG(info) << "vdisplay: the display is already there with another description, re-adding it"sv;
		removeVirtualDisplay(guid);
		error = LUZVDA::addDisplay(LUZVDA_DRIVER_HANDLE, desc, output);
	}
	if (error) {
		BOOST_LOG(warning) << "vdisplay: failed to add a virtual display, error "sv << error;
		return std::wstring();
	}

	// Windows gives the display a path shortly after it arrives
	auto adapter = LUZVDA::fromProtocol(output.adapter);
	std::wstring name;
	while (!displayNameFor(adapter, output.target_id, name)) {
		if (std::chrono::steady_clock::now() - start > 3s) {
			BOOST_LOG(warning) << "vdisplay: no name for the new virtual display in time"sv;
			return std::wstring();
		}
		Sleep(10);
	}

	const auto& mode = desc.modes[desc.preferred_mode < desc.mode_count ? desc.preferred_mode : 0];
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	BOOST_LOG(info) << "vdisplay: added "sv << narrow(name.c_str()) << " in "sv << ms << " ms: "sv
		<< mode.width << 'x' << mode.height << 'x' << mode.refresh_mhz << " mHz, one of "sv << desc.mode_count << " modes, "sv
		<< desc.width_mm << 'x' << desc.height_mm << " mm, "sv << (desc.primaries[6] ? "own primaries, "sv : "sRGB, "sv)
		<< ((desc.flags & LUZVDA_DISPLAY_HDR) ? "HDR"sv : "SDR"sv);
	if (desc.flags & LUZVDA_DISPLAY_HDR) {
		BOOST_LOG(info) << "vdisplay: HDR peak "sv << desc.max_luminance << " nits, full screen "sv << desc.max_full_frame_luminance
			<< " nits, black "sv << desc.min_luminance / 10000.0 << " nits"sv;
	}

	return name;
}

bool updateVirtualDisplayModes(const LUZVDA_UPDATE_MODES& update) {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	if (DWORD error = LUZVDA::updateModes(LUZVDA_DRIVER_HANDLE, update)) {
		BOOST_LOG(warning) << "vdisplay: failed to update the display's modes, error "sv << error;
		return false;
	}
	return true;
}

bool removeVirtualDisplay(const GUID& guid) {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	LUID adapter {};
	uint32_t targetId = 0;
	bool found = findListed(guid, adapter, targetId);

	if (DWORD error = LUZVDA::removeDisplay(LUZVDA_DRIVER_HANDLE, LUZVDA::toProtocol(guid))) {
		BOOST_LOG(debug) << "vdisplay: failed to remove the virtual display, error "sv << error;
		return false;
	}

	// Windows drops the display's path a moment after the driver does. A display added again with
	// the same GUID gets the same target, so until then the old path passes for the new display's:
	// round 2's re-created displays were "added in 0 ms", before display_device could see them.
	if (found) {
		auto start = std::chrono::steady_clock::now();
		while (hasPath(adapter, targetId)) {
			if (std::chrono::steady_clock::now() - start > 2s) {
				BOOST_LOG(warning) << "vdisplay: Windows still shows the removed virtual display after 2 s"sv;
				break;
			}
			Sleep(10);
		}
	}

	BOOST_LOG(debug) << "vdisplay: virtual display removed"sv;
	return true;
}

// [fork]
std::wstring getVirtualDisplayName(const GUID& guid) {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return std::wstring();
	}

	LUID adapter {};
	uint32_t targetId = 0;
	std::wstring name;
	if (!findListed(guid, adapter, targetId) || !displayNameFor(adapter, targetId, name)) {
		return std::wstring();
	}
	return name;
}

// [fork]
PRESENCE getVirtualDisplayPresence(const GUID& guid, std::wstring& name) {
	name.clear();
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return PRESENCE::UNKNOWN;
	}

	LUZVDA_LIST list;
	if (LUZVDA::listDisplays(LUZVDA_DRIVER_HANDLE, list)) {
		return PRESENCE::UNKNOWN;
	}
	auto want = LUZVDA::toProtocol(guid);
	const LUZVDA_LIST_ENTRY* listed = nullptr;
	for (uint32_t i = 0; i < list.count; ++i) {
		if (std::memcmp(list.displays[i].monitor_guid.bytes, want.bytes, sizeof(want.bytes)) == 0) {
			listed = &list.displays[i];
			break;
		}
	}
	if (!listed) {
		return PRESENCE::MISSING;
	}

	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!queryActiveConfig(paths, modes)) {
		return PRESENCE::UNKNOWN;
	}
	const auto* found = pathFor(paths, LUZVDA::fromProtocol(listed->adapter), listed->target_id);
	if (!found) {
		return PRESENCE::INACTIVE;
	}
	return sourceNameOf(*found, name) ? PRESENCE::ACTIVE : PRESENCE::UNKNOWN;
}

// [fork]
bool setDisplayPositions(const std::vector<display_position_t>& positions) {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (positions.empty() || !queryActiveConfig(paths, modes)) {
		return false;
	}

	std::size_t placed = 0;
	bool moved = false;
	for (const auto& path : paths) {
		std::wstring name;
		if (!sourceNameOf(path, name)) {
			continue;
		}
		for (const auto& want : positions) {
			if (want.name != name) {
				continue;
			}
			if (auto* mode = sourceModeOf(path, modes)) {
				moved = moved || mode->sourceMode.position.x != want.x || mode->sourceMode.position.y != want.y;
				mode->sourceMode.position.x = want.x;
				mode->sourceMode.position.y = want.y;
				placed++;
			}
			break;
		}
	}
	if (placed != positions.size()) {
		BOOST_LOG(warning) << "vdisplay: only "sv << placed << " of "sv << positions.size() << " displays to place are active"sv;
	}
	if (!placed) {
		return false;
	}
	// Every display in its place already: no change, which would blank the screens
	if (!moved) {
		return true;
	}

	LONG status = applyConfig(paths, modes);
	if (status != ERROR_SUCCESS) {
		BOOST_LOG(warning) << "vdisplay: failed to place the displays ("sv << status << ')';
		return false;
	}
	return true;
}

int removeAllVirtualDisplays() {
	if (LUZVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return 0;
	}

	LUZVDA_LIST list;
	if (DWORD error = LUZVDA::listDisplays(LUZVDA_DRIVER_HANDLE, list)) {
		BOOST_LOG(warning) << "vdisplay: failed to list the driver's displays, error "sv << error;
		return 0;
	}

	int removed = 0;
	for (uint32_t i = 0; i < list.count; ++i) {
		if (LUZVDA::removeDisplay(LUZVDA_DRIVER_HANDLE, list.displays[i].monitor_guid) == 0) {
			removed++;
		}
	}
	if (removed) {
		BOOST_LOG(info) << "vdisplay: removed "sv << removed << " virtual displays left from before"sv;
	}
	return removed;
}
}
