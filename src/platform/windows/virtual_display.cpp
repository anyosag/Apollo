#include <windows.h>
#include <atomic>
#include <chrono>
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
#include "misc.h"
#include "src/logging.h"
#include "virtual_display.h"

using namespace SUDOVDA;
using namespace std::literals;

namespace VDISPLAY {
// {dff7fd29-5b75-41d1-9731-b32a17a17104}
// static const GUID DEFAULT_DISPLAY_GUID = { 0xdff7fd29, 0x5b75, 0x41d1, { 0x97, 0x31, 0xb3, 0x2a, 0x17, 0xa1, 0x71, 0x04 } };

HANDLE SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;

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

bool getDisplayHDR(const LUID& adapterLuid, const wchar_t* displayName) {
	Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
	if (FAILED(hr)) {
		BOOST_LOG(warning) << "vdisplay: CreateDXGIFactory1 failed in getDisplayHDR, hr=0x"sv << std::hex << hr;
		return false;
	}

	for (UINT adapterIdx = 0; ; ++adapterIdx) {
		Microsoft::WRL::ComPtr<IDXGIAdapter1> currentAdapter;
		hr = dxgiFactory->EnumAdapters1(adapterIdx, currentAdapter.ReleaseAndGetAddressOf());

		if (hr == DXGI_ERROR_NOT_FOUND) {
			break; // No more adapters
		}
		if (FAILED(hr)) {
			BOOST_LOG(warning) << "vdisplay: EnumAdapters1 failed for index "sv << adapterIdx << " in getDisplayHDR"sv;
			break;
		}

		DXGI_ADAPTER_DESC1 adapterDesc;
		hr = currentAdapter->GetDesc1(&adapterDesc);
		if (FAILED(hr)) {
			continue;
		}

		if (sameLuid(adapterDesc.AdapterLuid, adapterLuid)) {
			std::wstring_view displayName_view{displayName};

			// Adapter found. Now iterate its outputs and match against targetGdiDeviceName.
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
				if (FAILED(hr)) {
					continue;
				}

				MONITORINFOEXW monitorInfoEx = {};
				monitorInfoEx.cbSize = sizeof(MONITORINFOEXW);
				if (GetMonitorInfoW(dxgiOutputDesc.Monitor, &monitorInfoEx)) {
					if (displayName_view == monitorInfoEx.szDevice) {
						// This is the correct output!
						Microsoft::WRL::ComPtr<IDXGIOutput6> dxgiOutput6;
						hr = dxgiOutput.As(&dxgiOutput6);

						if (SUCCEEDED(hr) && dxgiOutput6) {
							DXGI_OUTPUT_DESC1 outputDesc1;
							hr = dxgiOutput6->GetDesc1(&outputDesc1);
							if (SUCCEEDED(hr)) {
								if (outputDesc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
									return true; // HDR Active
								}
							} else {
								BOOST_LOG(debug) << "vdisplay: GetDesc1 (output) failed for "sv << narrow(monitorInfoEx.szDevice);
							}
						} else {
							BOOST_LOG(debug) << "vdisplay: no IDXGIOutput6 for "sv << narrow(monitorInfoEx.szDevice) << ", can't read its HDR state"sv;
						}
						// Matched the output, checked HDR (it was false or error). This is the only output we care about for this adapter.
						return false; // Return false as HDR not active or error for this specific display
					}
				}
			} // end output enumeration loop for the matched adapter

			// If output loop completes, the targetGdiDeviceName was not found among this adapter's DXGI outputs.
			BOOST_LOG(debug) << "vdisplay: "sv << narrow(displayName) << " not found among the adapter's DXGI outputs"sv;
			return false;
		}
	} // end adapter enumeration loop

	BOOST_LOG(debug) << "vdisplay: adapter of "sv << narrow(displayName) << " not found via DXGI"sv;
	return false;
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
	LUID adapterId;
	uint32_t targetId;

	if (!findDisplayIds(displayName, adapterId, targetId)) {
		BOOST_LOG(warning) << "vdisplay: failed to find display IDs for "sv << narrow(displayName);
		return false;
	}

	return getDisplayHDR(adapterId, displayName);
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
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return;
	}

	CloseHandle(SUDOVDA_DRIVER_HANDLE);

	SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
}

DRIVER_STATUS openVDisplayDevice() {
	uint32_t retryInterval = 20;
	while (true) {
		SUDOVDA_DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
		if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
			if (retryInterval > 320) {
				BOOST_LOG(warning) << "vdisplay: failed to open the SudoVDA driver"sv;
				return DRIVER_STATUS::FAILED;
			}
			retryInterval *= 2;
			Sleep(retryInterval);
			continue;
		}

		break;
	}

	if (!CheckProtocolCompatible(SUDOVDA_DRIVER_HANDLE)) {
		BOOST_LOG(warning) << "vdisplay: the SudoVDA driver's protocol version isn't compatible"sv;
		closeVDisplayDevice();
		return DRIVER_STATUS::VERSION_INCOMPATIBLE;
	}

	return DRIVER_STATUS::OK;
}

bool startPingThread(std::function<void()> failCb) {
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdogOut;
	if (GetWatchdogTimeout(SUDOVDA_DRIVER_HANDLE, watchdogOut)) {
		BOOST_LOG(info) << "vdisplay: watchdog timeout "sv << watchdogOut.Timeout << " s, countdown "sv << watchdogOut.Countdown;
	} else {
		BOOST_LOG(warning) << "vdisplay: failed to read the driver's watchdog"sv;
		return false;
	}

	if (watchdogOut.Timeout) {
		auto sleepInterval = watchdogOut.Timeout * 1000 / 3;
		std::thread ping_thread([sleepInterval, failCb = std::move(failCb)]{
			// [fork] Give up after 4 failures in a row; one success starts the count again
			uint8_t fail_count = 0;
			for (;;) {
				if (!sleepInterval) return;
				if (PingDriver(SUDOVDA_DRIVER_HANDLE)) {
					fail_count = 0;
				} else {
					fail_count += 1;
					if (fail_count > 3) {
						failCb();
						return;
					}
				};
				Sleep(sleepInterval);
			}
		});

		ping_thread.detach();
	}

	return true;
}

bool setRenderAdapterByName(const std::wstring& adapterName) {
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
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

		if (SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, desc.AdapterLuid)) {
			return true;
		}
	}

	return false;
}

// [fork] Without an adapter_name, render on the hardware GPU with the most dedicated video memory.
// The encoder runs on the GPU that renders the captured display, so this is also the one that
// encodes. Integrated GPUs have little or no dedicated memory.
bool setRenderAdapterAuto() {
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
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

	if (!SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, best.AdapterLuid)) {
		BOOST_LOG(warning) << "vdisplay: failed to render the virtual display on "sv << narrow(best.Description);
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

std::wstring createVirtualDisplay(
	const char* s_client_uid,
	const char* s_client_name,
	uint32_t width,
	uint32_t height,
	uint32_t fps,
	const GUID& guid
) {
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return std::wstring();
	}

	auto start = std::chrono::steady_clock::now();

	VIRTUAL_DISPLAY_ADD_OUT output;
	if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, fps, guid, s_client_name, s_client_uid, output)) {
		BOOST_LOG(warning) << "vdisplay: failed to add a virtual display"sv;
		return std::wstring();
	}

	uint32_t retryInterval = 20;
	wchar_t deviceName[CCHDEVICENAME]{};
	while (!GetAddedDisplayName(output, deviceName)) {
		Sleep(retryInterval);
		if (retryInterval > 320) {
			BOOST_LOG(warning) << "vdisplay: no name for the new virtual display in time"sv;
			return std::wstring();
		}
		retryInterval *= 2;
	}

	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	BOOST_LOG(info) << "vdisplay: added "sv << narrow(deviceName) << " ("sv << width << 'x' << height << 'x' << fps
		<< " mHz) in "sv << ms << " ms"sv;

	return std::wstring(deviceName);
}

bool removeVirtualDisplay(const GUID& guid) {
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
		BOOST_LOG(debug) << "vdisplay: virtual display removed"sv;
		return true;
	} else {
		return false;
	}
}
}
