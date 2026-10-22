#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <luzvda/luzvda.h>

namespace VDISPLAY {
	enum class DRIVER_STATUS {
		UNKNOWN              = 1,
		OK                   = 0,
		FAILED               = -1,
		VERSION_INCOMPATIBLE = -2,
		WATCHDOG_FAILED      = -3
	};

	extern HANDLE LUZVDA_DRIVER_HANDLE;

	LONG changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate);
	LONG changeDisplaySettings2(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated=false);
	bool getDisplayHDRByName(const wchar_t* displayName);
	bool setDisplayHDRByName(const wchar_t* displayName, bool enableAdvancedColor);

	// [fork] Waits until DXGI lists the display, where capture and the encoder probe look for it;
	// false if it doesn't within the timeout
	bool waitForOutput(const wchar_t* displayName, std::chrono::milliseconds timeout);

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

	// [fork] Adds the display LuzVDA is asked for (fork/luzvda_desc.h) and returns its GDI name once
	// Windows has given it one, or an empty name. A display already there with this GUID but
	// another description is removed and added again.
	std::wstring createVirtualDisplay(const LUZVDA_DISPLAY_DESC& desc);
	// [fork] Gives a display new modes; Windows only offers those it also had when it arrived
	bool updateVirtualDisplayModes(const LUZVDA_UPDATE_MODES& update);
	bool removeVirtualDisplay(const GUID& guid);
	// [fork] The GDI name the display with this GUID has now, or an empty name
	std::wstring getVirtualDisplayName(const GUID& guid);

	// [fork] Whether the display with this GUID is still there, for the recovery monitor
	// (fork/vdisplay_recovery.h)
	enum class PRESENCE {
		UNKNOWN,   // The driver or Windows couldn't be asked
		ACTIVE,    // The driver has it and Windows shows it; name is its GDI name
		INACTIVE,  // The driver has it, but Windows switched it off
		MISSING    // The driver doesn't have it any more
	};
	PRESENCE getVirtualDisplayPresence(const GUID& guid, std::wstring& name);
	// [fork] Removes every display the driver has; returns how many
	int removeAllVirtualDisplays();
}
