#pragma once

#include <functional>
#include <string>
#include <vector>

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#include <ddk/d4iface.h>
#include <ddk/d4drvif.h>
#include <sudovda/sudovda.h>

namespace VDISPLAY {
	enum class DRIVER_STATUS {
		UNKNOWN              = 1,
		OK                   = 0,
		FAILED               = -1,
		VERSION_INCOMPATIBLE = -2,
		WATCHDOG_FAILED      = -3
	};

	extern HANDLE SUDOVDA_DRIVER_HANDLE;

	LONG changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate);
	LONG changeDisplaySettings2(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated=false);
	bool getDisplayHDRByName(const wchar_t* displayName);
	bool setDisplayHDRByName(const wchar_t* displayName, bool enableAdvancedColor);

	// [fork] The display's current size, and its refresh rate in millihertz
	bool getDisplayMode(const wchar_t* deviceName, int& width, int& height, int& refresh_mhz);

	// [fork] Mode or layout changes and HDR switches since the last call, each of which blanks the screen
	struct mode_stats_t {
		int mode_sets;
		int hdr_changes;
	};
	mode_stats_t takeModeStats();

	void closeVDisplayDevice();
	DRIVER_STATUS openVDisplayDevice();
	bool startPingThread(std::function<void()> failCb);
	bool setRenderAdapterByName(const std::wstring& adapterName);

	// [fork] The named adapter, or without a name the hardware GPU with the most dedicated memory
	bool setRenderAdapter(const std::string& adapterName);

	std::wstring createVirtualDisplay(
		const char* s_client_uid,
		const char* s_client_name,
		uint32_t width,
		uint32_t height,
		uint32_t fps,
		const GUID& guid
	);
	bool removeVirtualDisplay(const GUID& guid);
}
