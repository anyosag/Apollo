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

Fork code lives in new files: `src/fork/`, `tests/fork/`, `.github/workflows/` and this file. Upstream files only get the small hooks below. Most are calls into `fork_tweaks::`; the rest are marked `[fork]`, so `git grep -e fork_tweaks -e "\[fork\]"` finds them all. If a sync fails with a conflict, merge upstream manually, keep upstream's change and re-apply the hook.

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
| `src/stream.cpp` | The `IDX_SET_CLIPBOARD` log line shows the size, not the contents |
| `src/platform/windows/display_base.cpp` | Logs why the capture restarts (`Capture restarts: …`): the desktop duplication was lost, or DXGI's factory went stale because displays or GPUs changed |
| `src/main.cpp` | The encoder probe's temporary display uses `VDISPLAY::setRenderAdapter()`, is described for LuzVDA, and its rate is 60000 mHz instead of 60 |
| `src/platform/windows/virtual_display.*` | Talks to LuzVDA (`third-party/luzvda/`) instead of SudoVDA: `createVirtualDisplay()` takes the driver's description, `updateVirtualDisplayModes()` and `removeAllVirtualDisplays()` are new, opening the driver removes displays left from before, and the new display is found by adapter and target id, polled every 10 ms for up to 3 s. Logs through `BOOST_LOG` (it used `printf`, which the service loses); the isolated layout's geometry moved to `display_layout.h`; `getDisplayMode()`, `takeModeStats()`, `setRenderAdapter()` added; the ping thread gives up after 4 failures in a row instead of 4 in total; unused functions removed |
| `third-party/sudovda/` | Removed; replaced by the fork's `third-party/luzvda/`: `luzvda-ioctl.h` is the driver's MIT protocol header, copied from the release (CI checks it), and `luzvda.h` Apollo's calls into it |
| `src_assets/windows/drivers/sudovda/` | Removed; replaced by the fork's `src_assets/windows/drivers/luzvda/` (`install.bat`, `uninstall.bat`, `luzvda.cer`) |
| `cmake/packaging/windows.cmake`, `windows_nsis.cmake` | The driver component and the install and uninstall commands are LuzVDA's |
| `src_assets/common/assets/web/configs/tabs/AudioVideo.vue`, `locale/en.json` | The driver status and hints name LuzVDA |
| `tests/CMakeLists.txt` | `tests/fork/` excluded from upstream's test glob |

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
    - Reading a virtual display's HDR state looks for it among every adapter's DXGI outputs. Upstream looked only under the adapter CCD names for it, but a virtual display's output belongs to the GPU that renders it, so HDR always read as off: a resume that kept HDR switched it off and on, and one to SDR left it on.
  - A resume, or a launch of the app already running, gives the virtual display the size, rate and HDR state the client now asks for. Upstream kept the display as the first launch set it up, so resuming from another screen or aspect ratio streamed a scaled picture. Only when no one else is streaming; otherwise the log says the mode was kept.
    - If the display can't take the new mode, it is removed and created again with the same identity. Windows moves the app's windows off it and back, which a mode change wouldn't.
    - Logs: `vdisplay: resume asks for <mode>, <display> runs <mode>`, then `vdisplay: resume handled in <n> ms, <m> mode changes, <h> HDR switches`.
  - Without an `adapter_name`, the virtual display renders on the hardware GPU with the most dedicated video memory, which then also encodes. Upstream left the choice to the driver. Log: `vdisplay: rendering on <GPU> (<n> MB dedicated)`.
  - The isolated layout (`isolated_virtual_display_option`) had two bugs: a leak of every display but the first on each launch, and an uninitialized x offset when no display sat at (0, 0). Testing its geometry on 2,000 random desktops found a third: about 1 in 40 ended with displays overlapping each other. Where upstream's layout is valid it's used unchanged; otherwise moves that would overlap are skipped, and if a display still can't be placed, the layout is left as Windows made it (logged).
  - Each launch logs `vdisplay: ready in <n> ms after launch began, <m> mode changes` and `vdisplay: HDR set <n> ms after launch began, <h> HDR switches`: the baseline for cutting the flashing at launch.

## Tests

`tests/fork/` holds standalone programs with no Apollo dependencies, built and run by the "Fork unit tests" step of the Windows build:

- `clipbundle_test.cpp`: the bundle format and send rules (shared with LuzLunar).
- `clipboard_win_smoke.cpp`: a round trip through the real Windows clipboard; it skips when the runner has no clipboard.
- `header/*_test.cpp`: one program each for header-only code: what clients send (`client_args`), control packets, input packets, Web UI login, the virtual display's mode (`vdisplay_mode`), the client's screen (`client_display`), the driver's requests built from it (`luzvda_desc`) and the isolated layout (`display_layout`, including the random desktops above).
- Each `src/fork/*.h` is also compiled on its own with the build's include paths: the repository root and `third-party`, not `src/`, which the header tests have. A fork header includes its siblings by bare name (`"vdisplay_mode.h"`).
