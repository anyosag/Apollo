# anyosag/Apollo

A personal fork of [ClassicOldSong/Apollo](https://github.com/ClassicOldSong/Apollo) for one Windows PC (NVIDIA GPU) streaming to an Apple Silicon Mac running [LuzLunar](https://github.com/anyosag/LuzLunar), a fork of Moonlight.

It stays a drop-in Apollo: stock Moonlight clients work as before, and every fork feature can be turned off or does nothing until it's configured.

## Builds

- `.github/workflows/build-windows.yml` builds the NSIS installer and the portable zip, following `docs/building.md`. It runs on pushes to `master` and `claude/**`, by hand, and from the sync workflow. The artifact is `Apollo-windows-<version>` with `Apollo-windows-installer.exe`, `Apollo-windows-portable.zip` and `SHA256SUMS.txt`.
  - The version is the latest upstream release tag plus the number of commits since it (e.g. `0.4.8.32`), so Apollo's update check compares correctly against upstream releases.
  - The virtual display driver is the fork's own, [LuzVDA](https://github.com/anyosag/LuzVDA). Its CI builds it unsigned; the owner signs it on their PC with a key that never leaves it and publishes the signed zip as a release (`signed-<version>`). The workflow downloads that release, pinned by SHA256, and fails unless the driver, its catalog and `luzvda-ctl.exe` are signed with the certificate in `src_assets/windows/drivers/luzvda/luzvda.cer` and its protocol header matches `third-party/luzvda/luzvda-ioctl.h`. A newer driver means a new release and new `LUZVDA_*` values in the workflow.
    - The installer stops Apollo's service, removes SudoVDA and SudoMaker's certificate (by its thumbprint), trusts the LuzVDA certificate, and installs LuzVDA, which shows display names of up to 63 characters (its default since 0.0.0.9; `edidSplitName` = 0 in `HKLM\SOFTWARE\LuzVDA` gives the 13-character form). The service starts again afterwards, as before. Any PC that installs this Apollo trusts drivers and programs signed with the owner's key.
  - The "Fork unit tests" step builds and runs `tests/fork/` before the main build (see [Tests](#tests)).
  - The installer and zip get a build provenance attestation. Check a download came from this workflow with `gh attestation verify Apollo-windows-installer.exe -R anyosag/Apollo`.
  - Actions are pinned to commits; Dependabot proposes updates (`.github/dependabot.yml`).
- `.github/workflows/sync-upstream.yml` merges upstream `master` daily (07:23 UTC) onto the `upstream-sync` branch, opens (or updates) a pull request into `master` and then calls the build, so upstream changes are looked over before they ship. Merge that pull request with "Create a merge commit". Opening it needs "Allow GitHub Actions to create and approve pull requests" in the repository's Actions settings. If the merge conflicts, it stops with an error instead of pushing. It re-enables its own schedule so GitHub doesn't turn it off after 60 days without activity. It pushes with `GITHUB_TOKEN`, which can't push changes to `.github/workflows/`; upstream has no workflows today, but if it adds some, the sync will need a token with the Workflows permission.

## Installing a build

1. Download the `Apollo-windows-…` artifact from the workflow run and unzip it.
2. Back up `C:\Program Files\Apollo\config` (the installer keeps it, but it holds the pairings, `apps.json` and `sunshine.conf`).
3. Run `Apollo-windows-installer.exe` as administrator (`/S` for a silent install). It replaces the program and keeps the config.
4. Check that the files in `config` are unchanged and that `ApolloService` is running.

## Keeping upstream merges clean

Fork code lives in new files: `src/fork/`, `tests/fork/`, `scripts/fork/`, `.github/workflows/` and this file. Upstream files only get the small hooks below. Most are calls into `fork_tweaks::`; the rest are marked `[fork]`, so `git grep -e fork_tweaks -e "\[fork\]"` finds them all. If a sync fails with a conflict, merge upstream manually, keep upstream's change and re-apply the hook.

| Upstream file | Hook |
|---|---|
| `cmake/compile_definitions/common.cmake` | Globs `src/fork/*.cpp` and `*.h` into the build, after the NVENC sources |
| `cmake/compile_definitions/windows.cmake` | Links `z` and defines `CLIPBUNDLE_WITH_ZLIB`; lists `third-party/luzvda/` instead of `third-party/sudovda/` |
| `src/config.cpp` | `fork_tweaks::apply_config(vars)` before the `flags` option is read |
| `src/process.cpp` | `fork_tweaks::apply_app_encoder_overrides(app.uuid)` in `proc_t::execute()`, after the per-app gamepad override; `fork_tweaks::restore_app_encoder_overrides()` in `terminate()`. Virtual display: the size and rate math in `execute()` comes from `fork/vdisplay_mode.h`; creating the display is `setup_virtual_display()` and the HDR workaround `apply_virtual_display_hdr()`, both also used by the new `reconfigure_virtual_display()`; `VDISPLAY::setRenderAdapter()` replaces the by-name call; timing and mode-change counts are logged. `setup_virtual_display()` describes the client's screen to the driver (`fork/client_display.h`, `fork/luzvda_desc.h`) and skips the mode change when the display arrived in the stream's mode |
| `src/process.h` | `reconfigure_virtual_display()` and the members above, including `_vdisplay_desc`; `ctx_t::virtual_display_primary` removed (never set or read) |
| `src/rtsp.h` | `launch_session_t::client_screen` |
| `src/video.cpp` | `fork_tweaks::try_reuse_encoder_probe()` in the "another probe is required" check of `probe_encoders()`; `fork_tweaks::invalidate_encoder_probe()` when `make_encode_session()` fails during a stream |
| `src/nvhttp.cpp` | `make_launch_session()` reads the client's screen from LuzLunar's `luz*` arguments. `reconfigure_virtual_display()` in `launch()` (for the app already running) and `resume()`, in the `else` of upstream's "no active sessions and no virtual display" block. `getClipboard`/`setClipboard` accept `type=bundle` and hand it to `fork_tweaks::clipboard` after upstream's permission and session checks; a missing `type` is a 400 instead of an exception; a failed text set returns after its 500 instead of also sending a 200; `max_request_streambuf_size` raised for bundles |
| `src/stream.cpp` | The `IDX_SET_CLIPBOARD` log line shows the size, not the contents. A handler for the client's FEC status (`0x5502`), counts in the keyframe-request and reference-invalidation handlers, and two log calls in `controlBroadcastThread()` (`fork/frame_loss.h`); `session_t::frame_loss` |
| `src/platform/windows/display_base.cpp` | Logs why the capture restarts (`Capture restarts: …`): the desktop duplication was lost, or DXGI's factory went stale because displays or GPUs changed |
| `src/input_validation.h` | The size check for LuzLunar's trackpad frame (`fork/luztouchpad.h`) |
| `src/input.cpp` | Trackpad passthrough: the frame needs the Mouse permission, goes to `platf::touchpad_frame()` (`passthrough_touchpad_frame()`), and `reset()` lifts the client's fingers with `platf::touchpad_cancel()` (`fork/touchpad_win.h`) |
| `src/platform/windows/input.cpp` | Trackpad passthrough: the client's `touchpad` session in `client_input_raw_t`, `touchpad_frame()` and `touchpad_cancel()`, and `get_capabilities()` offers `0x40000000` when `fork_touchpad::available()` |
| `src/main.cpp` | The encoder probe's temporary display uses `VDISPLAY::setRenderAdapter()`, is described for LuzVDA, and its rate is 60000 mHz instead of 60 |
| `src/platform/windows/virtual_display.*` | Talks to LuzVDA (`third-party/luzvda/`) instead of SudoVDA: `createVirtualDisplay()` takes the driver's description, `updateVirtualDisplayModes()` and `removeAllVirtualDisplays()` are new, opening the driver removes displays left from before, and the new display is found by adapter and target id, polled every 10 ms for up to 3 s. Logs through `BOOST_LOG` (it used `printf`, which the service loses); the isolated layout's geometry moved to `display_layout.h`; `getDisplayMode()`, `takeModeStats()`, `setRenderAdapter()` added; the ping thread gives up after 4 failures in a row instead of 4 in total; unused functions removed |
| `third-party/sudovda/` | Removed; replaced by the fork's `third-party/luzvda/`: `luzvda-ioctl.h` is the driver's MIT protocol header, copied from the release (CI checks it), and `luzvda.h` Apollo's calls into it |
| `src_assets/windows/drivers/sudovda/` | Removed; replaced by the fork's `src_assets/windows/drivers/luzvda/` (`install.bat`, `uninstall.bat`, `luzvda.cer`) |
| `cmake/packaging/windows.cmake`, `windows_nsis.cmake` | The driver component and the install and uninstall commands are LuzVDA's |
| `src_assets/common/assets/web/configs/tabs/AudioVideo.vue`, `locale/en.json` | The driver status and hints name LuzVDA |
| `tests/CMakeLists.txt` | `tests/fork/` excluded from upstream's test glob |

### Replaced images

The icons and app covers are replaced wholesale with the LuzLunar look (see [Fork features](#fork-features)), keeping upstream's file names and sizes so nothing that references them changes:

- `apollo.svg`, `apollo.png`, `apollo.ico`, `apollo.icns`
- `src_assets/common/assets/web/public/images/`: `apollo.ico`, `logo-apollo.svg`, `logo-apollo-16.png`, `logo-apollo-45.png`, and `apollo-playing`, `apollo-pausing`, `apollo-locked` as `.svg`, `.png`, `.ico`, `-16.png` and `-45.png`
- `src_assets/common/assets/`: `desktop.png`, `desktop-alt.png`, `virtual_desktop.png`, `steam.png`, `input_only.png`, `terminate.png`, `template.png`

If a sync conflicts on one of them, keep ours. If upstream adds an image, draw it in `scripts/fork/icons/` too. `box.png` stays upstream's: Moonlight clients treat a 130×180 image as "no box art" and draw their own placeholder with the app name.

### Upstream fixes carried by the fork

- `src/video.cpp`: the AMF H.264 `profile` option is removed (commit `b063ddaf`). Upstream added it reading `cfg.profile`, which `video::config_t` doesn't have, so upstream `master` doesn't compile on Windows. Drop this change once upstream fixes it.
- `patches/<name>/*.patch` are fixes to submodules this fork doesn't own. `scripts/fork/apply-patches.sh` applies them (the Windows build runs it right after checkout; run it by hand before a local build). A patch that no longer applies fails the build, so a submodule update can't drop a fix silently.
  - `patches/enet/0001-only-the-peer-s-host-may-send-for-it.patch`: ENet ignored where a packet came from, so anyone who could reach the control stream port and guess a peer's 2-bit session ID could take over the peer's address and have the host send its control traffic there. Now only the peer's own host (IP) may send for it; its port may still change, as it does when a NAT rebinds. Upstream disabled this check on purpose in 2019 (enet cf38f85) for clients talking to multi-homed hosts; on the host side its cost is that a client whose IP changes mid-stream is dropped.
  - `patches/moonlight-common-c/0001-rtsp-don-t-read-past-a-message-that-ends-mid-option.patch`: the RTSP parser (used by `src/rtsp.cpp`) read past the end of a message whose last option has no line break after it. moonlight-common-c rewrote the parser upstream (aeba988), so drop this patch once the submodule moves past it. Shared with anyosag/LuzLunar.
- Windows install and service hardening:
  - The installer gives the install folder its own permissions before copying any file (owner Administrators; SYSTEM and Administrators full control, Users read and run) and limits `config\` to SYSTEM and Administrators. The service runs as SYSTEM from that folder, so in a folder like `C:\Apollo` other users could otherwise replace its files. A DLL another user had placed in a pre-existing custom folder would stay there, so install to the default `Program Files` location.
  - The firewall rules apply to Private and Domain networks only. If streaming stops at home, set the home network to Private in Windows' network settings.
  - The NVIDIA settings undo file's folder (`%ProgramData%\Sunshine`) is SYSTEM and Administrators only, and an existing one is used only if SYSTEM or Administrators own it and it isn't a link. Users no longer get write and delete on the undo file, which the service reads and deletes.

## Fork options

In `sunshine.conf`. `apply_config()` reads them and removes them from the option list, so Apollo doesn't report them as unrecognized. All accept `enabled`/`disabled` (also `true`/`false`, `on`/`off`, `yes`/`no`, `1`/`0`).

| Option | Default | Effect |
|---|---|---|
| `fork_encoder_probe_reuse` | `enabled` | Reuse the encoder probe when only the display layout changed |
| `fork_clipboard_rich` | `enabled` | Rich clipboard sync for LuzLunar; `disabled` answers `type=bundle` with 400, like stock Apollo |
| `fork_web_ui_remote_first_setup` | `disabled` | Let the first Web UI password be set from the addresses `origin_web_ui_allowed` allows. Off, only the Apollo PC itself (`https://localhost:47990`) can set it, or `sunshine --creds <username> <password>` |
| `fork_touchpad_passthrough` | `enabled` | Offer LuzLunar's trackpad passthrough (a synthetic Windows touchpad). Off, the host doesn't advertise it, drops the frames, and LuzLunar's trackpad stays a mouse |

## Fork features

- **Per-app encoder settings** (`src/fork/app_encoder_overrides.*`):
  - An app in `apps.json` can have an `"encoder-overrides"` object. Its keys and values are the same as in `sunshine.conf`:
    ```json
    "encoder-overrides": {
      "nvenc_preset": 6,
      "nvenc_twopass": "full_res",
      "nvenc_spatial_aq": "enabled",
      "nvenc_latency_over_power": "disabled"
    }
    ```
  - Supported keys: `nvenc_preset`, `nvenc_twopass`, `nvenc_spatial_aq`, `nvenc_vbv_increase`, `nvenc_realtime_hags`, `nvenc_latency_over_power` and `minimum_fps_target`. Anything else is logged and ignored.
  - They apply when the app launches and the global settings come back when it ends, the same save/restore pattern as upstream's per-app gamepad override. The encoder reads them when a stream starts, so they cover that app's streams.
  - Logs: `fork: encoder overrides for app [<name>]: …` and `fork: restored global encoder settings`.
- **Encoder probe reuse** (`src/fork/encoder_probe_reuse.*`):
  - With headless mode, every app launch creates a virtual display and turns the other displays off. Windows then reports that the GPUs or displays changed, so upstream reprobes every encoder on every launch: about 1.8 s on an RTX 5080, roughly a third of the time to the first frame.
  - The probe result depends on the GPU and driver, not on which displays are on. So the current result is kept if the last two full probes agreed, the adapters (LUID, vendor, device, subsystem, revision, driver version) are unchanged, and one H.264/HEVC test encode on the display about to be captured succeeds. Otherwise the full probe runs as before.
  - The first launch after the service starts always probes fully, keeping upstream's protection against boot-time races. If creating an encoder session fails during a stream, the next launch probes fully again.
  - Logs: `fork: reusing encoder probe for [<encoder>], …` or `fork: running a full encoder probe (<reason>)`.
- **Frame loss in the log** (`src/fork/frame_loss.h`):
  - Moonlight clients report each frame that needed repair from FEC parity packets or couldn't be repaired (`SS_FRAME_FEC_STATUS`, type `0x5502`). Upstream had no handler, so the reports only reached the debug log as unknown messages. A report is told apart as repaired or lost by whether the client had as many packets, data and parity together, as the frame had data packets.
  - While a client reports lost or repaired frames, or reference invalidations, at most every 10 s (keyframe requests alone don't count: clients ask for them at every start): `fork: frame loss for client [<device>] in the last 10.0 s, <n> frames repaired with FEC, <n> frames lost (<n> of <n> data packets missing in those frames), <n> reference invalidations, <n> keyframe requests; FEC <n>%`.
  - When a stream ends, the same for the whole stream (`… over 30.0 min, …`), also when nothing was lost.
  - Only logging: nothing changes the bitrate or FEC yet. It's the measurement for adaptive bitrate.
- **Rich clipboard sync for LuzLunar** (`src/fork/clipbundle.*`, `clipboard_win.*`, `clipboard_bundle.*`):
  - `GET`/`POST /actions/clipboard?type=bundle&v=1` carry a *clipbundle* (content type `application/x-luzlunar-clipboard`) with plain text, RTF, HTML, PNG/JPEG and Office's drawing format, so formatting survives Word, Excel and PowerPoint copies between the Mac and the PC. `type=text` is unchanged, and LuzLunar falls back to it when a host answers 400.
  - `clipbundle.*` is shared byte for byte with anyosag/LuzLunar (`app/fork/clipbundle.*`), and `tests/fork/clipbundle_test.cpp` runs in both repos' CI. Keep the copies identical. It has the compact little-endian format, zlib compression for text formats, a content hash, the Windows "HTML Format" header, and the send rules:
    - a file copy sends only its text
    - an image is sent only for picture copies (no text, or an image offered first), so an Excel range doesn't send a rendered bitmap
    - Word's HTML that points at local image files is dropped when RTF (which embeds the pictures) is there
    - bundles are limited to 64 MiB; items are dropped from the end, never the plain text, until it fits
  - Formats (`clipbundle::types()`):

    | Bundle type | Windows format | Mac pasteboard type |
    |---|---|---|
    | `text/plain` | `CF_UNICODETEXT` | `public.utf8-plain-text` |
    | `text/rtf` | `Rich Text Format` | `public.rtf` |
    | `text/html` | `HTML Format` | `public.html` |
    | `image/png` | `PNG` | `public.png` |
    | `image/jpeg` | `JFIF` | `public.jpeg` |
    | `ms/gvml` | `Art::GVML ClipFormat` | `com.microsoft.Art--GVML-ClipFormat` |

    Add an Office format only once it's confirmed to exist, with the same contents, on both systems, and add it to `types()` and the tests in both repos.
  - Only what changed is transferred. Every reply carries a state token (the PC's `GetClipboardSequenceNumber`) and the content hash. LuzLunar sends them back as `since` and `have`, so a `GET` of an unchanged clipboard answers "unchanged" without opening the clipboard, and content the Mac already has isn't sent again. A `POST` replies with the new token and hash, so the content isn't sent back to the Mac.
  - Requests run one at a time on their own thread (COM initialized for WIC), because Office renders some formats on demand and nvhttp's HTTPS server handles requests on a single thread. A request that arrives while another runs gets 503.
  - On the Windows side (`clipboard_win.*`), the clipboard is held open as briefly as possible. Bitmaps are converted to PNG with WIC; PNG/JPEG writes also add a `CF_DIBV5` for Paint and older apps. Explorer file copies send only their text.
  - Private content: password managers mark clips with `ExcludeClipboardContentFromMonitorProcessing` or `Clipboard Viewer Ignore`. Such clips are never sent for an automatic sync (`auto=1`), and when fetched with the shortcut they're marked private. Private clips from the Mac are written with `ExcludeClipboardContentFromMonitorProcessing`, and with `CanIncludeInClipboardHistory` and `CanUploadToCloudClipboard` set to 0.
  - Status codes: 200, 400 (unknown type, or `fork_clipboard_rich = disabled`), 413 (too large), 422 (malformed bundle), 500 (the clipboard couldn't be written), 503 (busy, or another app held the clipboard).
  - The device needs Apollo's Clipboard permissions; upstream's checks apply unchanged.
  - Logs show types and sizes, never content: `clipboard: sent <types> to [<device>], <n> bytes on the wire` and `clipboard: received <types> from [<device>]`. The formats the source app offered are logged at debug level.

- **Virtual display** (`src/fork/vdisplay_mode.h`, `src/fork/client_display.h`, `src/fork/luzvda_desc.h`, `src/platform/windows/display_layout.h`, `src/process.cpp`):
  - The virtual display comes from the fork's own driver, [anyosag/LuzVDA](https://github.com/anyosag/LuzVDA), and describes the screen the client streams to. LuzLunar sends that screen with each launch and resume (`luzDisplay=1`, `luzPanel`, `luzSafe`, `luzMm`, `luzHz`, `luzGamut`, `luzHdr`, `luzEdr`, `luzName`); the display then reports the client's physical size, color primaries and HDR brightness to Windows, so Windows scales and tone-maps for it. Its modes are the stream's mode first, then the screen's full and below-the-notch shapes, each at the stream's rate, the screen's fastest and 60 Hz (at most 32; none the driver would refuse).
    - Other clients get the description SudoVDA gave every display (70x39 cm, HDR at about 3,800 nits, sRGB), with only the stream's mode and 60 Hz.
    - Log: `Client [<device>] streams to 3024x1964 (3024x1890 below the notch), 302x196 mm, 120 Hz, P3, HDR 1600/1000 nits, EDR headroom 16.00`, then `vdisplay: added <display> in <n> ms: <mode>, one of <m> modes, <size> mm, <primaries>, HDR`.
    - A resume from another screen re-creates the display, since Windows reads a display's description only when it arrives. A resume to a mode the display doesn't have gives it the new mode list first (`UPDATE_MODES`) and tries the change up to four times over about a second, as Windows may take a moment to offer the new modes; the display is re-created only if that fails. A re-created display gets the launch's display configuration again (for example `ensure_only_display`), since Windows brings back its own arrangement, the PC's monitors included. Removing a display waits (up to 2 s) until Windows drops its path: the display comes back on the same target, and taking the old path for it left display_device without its device id, so the configuration went to whichever display was primary.
    - A mode change is one `SetDisplayConfig` with the target mode cleared, so Windows picks the signal timing for the new size and rate (as libdisplaydevice does), checked afterwards. Only if that doesn't take does it fall back to upstream's two steps (`ChangeDisplaySettingsExW`, then `SetDisplayConfig`), each of which blanks the screen.
    - A resume sets HDR again only when it should change or the display is new: Windows keeps a display's HDR state through a mode change.
    - Each encoding session logs the HDR values the client is told: `HDR metadata for the client: peak <n> nits, full frame <n> nits, black <n> nits`.
    - `dd_wa_virtual_display_hdr_toggle` (default **off**) switches a new virtual display's HDR off and on at stream start, as upstream always did. Off, Apollo only sets the state the client asked for, saving two HDR switches and a flash per start. Round 3 on the RTX 5080 showed LuzVDA's displays don't need the workaround: with it off, HDR launches looked identical to those with it on (the same measured brightness and colors, Main10, `Display is HDR: true`), including launches that arrived with HDR off after an SDR stream, where the display configuration's single switch to HDR was the only one.
    - Reading a virtual display's HDR state looks for it among every adapter's DXGI outputs. Upstream looked only under the adapter CCD names for it, but a virtual display's output belongs to the GPU that renders it, so HDR always read as off: a resume that kept HDR switched it off and on, and one to SDR left it on.
  - A resume, or a launch of the app already running, gives the virtual display the size, rate and HDR state the client now asks for. Upstream kept the display as the first launch set it up, so resuming from another screen or aspect ratio streamed a scaled picture. Only when no one else is streaming; otherwise the log says the mode was kept.
    - If the display can't take the new mode, it is removed and created again with the same identity. Windows moves the app's windows off it and back, which a mode change wouldn't.
    - Logs: `vdisplay: resume asks for <mode>, <display> runs <mode>`, then `vdisplay: resume handled in <n> ms, <m> mode changes, <h> HDR switches`.
  - Without an `adapter_name`, the virtual display renders on the hardware GPU with the most dedicated video memory, which then also encodes. Upstream left the choice to the driver. Log: `vdisplay: rendering on <GPU> (<n> MB dedicated)`.
  - The isolated layout (`isolated_virtual_display_option`) had two bugs: a leak of every display but the first on each launch, and an uninitialized x offset when no display sat at (0, 0). Testing its geometry on 2,000 random desktops found a third: about 1 in 40 ended with displays overlapping each other. Where upstream's layout is valid it's used unchanged; otherwise moves that would overlap are skipped, and if a display still can't be placed, the layout is left as Windows made it (logged).
  - Each launch logs `vdisplay: ready in <n> ms after launch began, <m> mode changes` and `vdisplay: HDR set <n> ms after launch began, <h> HDR switches; the display configuration left HDR <on|off>, now <on|off>`: the baseline for cutting the flashing at launch. `<h>` counts Apollo's own switches only; with `dd_hdr_option` on auto, the display configuration (libdisplaydevice) first sets HDR to the client's request itself, and its `Changing HDR states to:` line shows when it did.
  - A launch sets the virtual display's HDR before the encoder probe and the app start, so a game starts on the display as the client will see it. Upstream set it on a detached thread after the app had started, which read the display's name while `terminate()` could clear it, and for a physical display only logged a warning 3 s later.
  - When it arrives, the display is waited for (up to 2 s) until DXGI lists it, where capture, the encoder probe and the HDR state read look for it. Log: `vdisplay: DXGI lists <display> after <n> ms; it arrived with HDR <on|off>`, the state Windows saved for it. The startup encoder probe waits the same way instead of a fixed 500 ms.
  - Waiting for Windows to accept display changes (before setting HDR) gives up at once while the session is locked, instead of after about 3 s.
- **Trackpad passthrough for LuzLunar** (`src/fork/luztouchpad.h`, `touchpad.h`, `touchpad_win.*`, `touchpad_win_api.h`):
  - LuzLunar can send the fingers on the Mac trackpad instead of mouse input, and Apollo feeds them to a synthetic Precision Touchpad made with Windows 11's `CreateSyntheticPointerDevice2` (`PT_TOUCHPAD`, pre-release in Microsoft's docs). Windows then makes the pointer motion, taps, scrolling with inertia, pinch and three- and four-finger gestures itself, with the PC's touchpad settings.
  - Wire: one 50-byte input packet per change, magic `0x55004C01`, in the encrypted control stream like Moonlight's touch events, on its touch channel. It holds up to 5 contacts (slot, phase, position on the pad), whether the pad is pressed, a cancel-all flag, the pad's size in 1/100 mm and the client's clock. Every frame lists every finger still down. `luztouchpad.h` is shared byte for byte with anyosag/LuzLunar (`app/fork/luztouchpad.h`), and `tests/fork/luztouchpad_test.cpp` runs in both repos' CI; keep the copies identical.
  - `get_capabilities()` offers it as feature flag `0x40000000` in `x-ss-general.featureFlags` when `fork_touchpad_passthrough` is on, `mouse` input is on, and Windows makes a test touchpad (made and removed once; a failed try is repeated with the next stream). LuzLunar only sends frames to a host that offers it. A frame needs the client's Mouse permission.
  - `touchpad.h` (header-only, tested) turns frames into contacts: in contact, in range and confident while down, plus the first button while the pad is pressed; a lift is one more injection with only "confident". The client's clock gives the times (Windows reads speed from them, for inertia), kept no later than now and at most 300 ms behind, always increasing.
  - Each client gets its own device, made with its first frame at the pad's size and removed with the session. While fingers are down and the client is quiet, they're injected again every 250 ms (Windows drops held contacts after about a second), and lifted after 1.5 s.
  - Windows' own requirements: the touchpad switched on in Settings › Bluetooth & devices › Touchpad, and "Leave touchpad on when a mouse is connected" on if a mouse is plugged into the PC. On the lock screen and UAC prompts only the pointer is expected to work, since the gestures belong to Explorer.
  - Logs: `fork: synthetic touchpads work, so LuzLunar can send its trackpad` (or why not), `fork: made a 135.01 x 81.99 mm synthetic touchpad for LuzLunar`, `fork: removed LuzLunar's synthetic touchpad`, and `fork: LuzLunar's trackpad went quiet with fingers down; lifting them`.
- **LuzLunar look** (`scripts/fork/icons/`):
  - Apollo's icon is the daytime partner of LuzLunar's: a cream sun disc (LuzLunar's moon color, `#F4EBD0`) ringed by white rays drawn with LuzLunar's sparkle curve, on the same indigo tile (`#34419A` to `#18205A`, around LuzLunar's `#1F296B`). Its eight rays keep the outline of upstream's eight-point star.
  - The tray icons keep the sun and color its disc by state, each with its own glyph: green ▶ playing, blue ‖ paused, orange padlock when a PIN is needed. Each size has its own layout: four rays and a bigger disc at 24 px and below, glyphs on the pixel grid at 16 px, and the app icon's inset tile above 48 px.
  - The built-in apps' covers share one indigo night sky with LuzLunar sparkles, a cream line glyph and a Segoe UI Semibold label. Steam's cover shows a game controller rather than Valve's logo.
  - Everything is drawn in code (`icons.js`, `covers.js`). To change it, edit those and run `npm install` and `npm run build` in `scripts/fork/icons/` on Windows; `build.js` rewrites every file listed under [Replaced images](#replaced-images), including the `.ico` sizes `scripts/icons/convert_and_pack.sh` packs and the `.icns`. Set `APOLLO_COVER_FONT` to Segoe UI Semibold's path if it isn't `C:/Windows/Fonts/seguisb.ttf`.

## Tests

`tests/fork/` holds standalone programs with no Apollo dependencies, built and run by the "Fork unit tests" step of the Windows build:

- `clipbundle_test.cpp`: the bundle format and send rules (shared with LuzLunar).
- `luztouchpad_test.cpp`: trackpad passthrough's frame format (shared with LuzLunar).
- `clipboard_win_smoke.cpp`: a round trip through the real Windows clipboard; it skips when the runner has no clipboard.
- `touchpad_win_smoke.cpp` (also uploaded as `Apollo-touchpad-smoke-…`, a static `.exe`): makes a real synthetic touchpad and plays gestures on it through `touchpad.h`: one-finger moves with three ways of setting the time, held fingers, two-finger scrolling, three-finger swipes (taps and clicks with `--clicks`). It skips when Windows has no synthetic touchpads; `--strict` fails it instead. It moves the pointer, scrolls and opens Task View, so run it by hand on a PC you're watching, and as SYSTEM in your session (`psexec -s -i 1 touchpad_win_smoke.exe --strict`) to try Apollo's own context.
- `header/*_test.cpp`: one program each for header-only code: what clients send (`client_args`), control packets, input packets (with the trackpad frame), Web UI login, the virtual display's mode (`vdisplay_mode`), the client's screen (`client_display`), the driver's requests built from it (`luzvda_desc`), the isolated layout (`display_layout`, including the random desktops above), the client's loss reports (`frame_loss`) and trackpad frames turned into touchpad contacts (`touchpad`).
- Each `src/fork/*.h` is also compiled on its own with the build's include paths: the repository root and `third-party`, not `src/`, which the header tests have. A fork header includes its siblings by bare name (`"vdisplay_mode.h"`).
