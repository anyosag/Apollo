# anyosag/Apollo

A personal fork of [ClassicOldSong/Apollo](https://github.com/ClassicOldSong/Apollo) for one Windows PC (NVIDIA GPU) streaming to an Apple Silicon Mac running [LuzLunar](https://github.com/anyosag/LuzLunar), a fork of Moonlight.

It stays a drop-in Apollo: stock Moonlight clients work as before, and every fork feature can be turned off or does nothing until it's configured.

## Builds

- `.github/workflows/build-windows.yml` builds the NSIS installer and the portable zip, following `docs/building.md`. It runs on pushes to `master` and `claude/**`, by hand, and from the sync workflow. The artifact is `Apollo-windows-<version>` with `Apollo-windows-installer.exe`, `Apollo-windows-portable.zip` and `SHA256SUMS.txt`.
  - The version is the latest upstream release tag plus the number of commits since it (e.g. `0.4.8.32`), so Apollo's update check compares correctly against upstream releases.
  - The SudoVDA virtual display driver's `SudoVDA.dll` and `nefconc.exe` aren't in the source tree (`*.dll` is gitignored). The workflow takes them from the official Apollo v0.4.6 installer, pinned by SHA256, and fails if the tree's `inf`/`cat`/`cer` no longer match that release or if a package is missing them.
  - The "Fork unit tests" step builds and runs `tests/fork/` before the main build (see [Tests](#tests)).
- `.github/workflows/sync-upstream.yml` merges upstream `master` daily (07:23 UTC) and then calls the build. If the merge conflicts, it stops with an error instead of pushing. It re-enables its own schedule so GitHub doesn't turn it off after 60 days without activity. It pushes with `GITHUB_TOKEN`, which can't push changes to `.github/workflows/`; upstream has no workflows today, but if it adds some, the sync will need a token with the Workflows permission.

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
| `cmake/compile_definitions/windows.cmake` | Links `z` and defines `CLIPBUNDLE_WITH_ZLIB` |
| `src/config.cpp` | `fork_tweaks::apply_config(vars)` before the `flags` option is read |
| `src/process.cpp` | `fork_tweaks::apply_app_encoder_overrides(app.uuid)` in `proc_t::execute()`, after the per-app gamepad override; `fork_tweaks::restore_app_encoder_overrides()` in `terminate()` |
| `src/video.cpp` | `fork_tweaks::try_reuse_encoder_probe()` in the "another probe is required" check of `probe_encoders()`; `fork_tweaks::invalidate_encoder_probe()` when `make_encode_session()` fails during a stream |
| `src/nvhttp.cpp` | `getClipboard`/`setClipboard` accept `type=bundle` and hand it to `fork_tweaks::clipboard` after upstream's permission and session checks; a missing `type` is a 400 instead of an exception; a failed text set returns after its 500 instead of also sending a 200; `max_request_streambuf_size` raised for bundles |
| `src/stream.cpp` | The `IDX_SET_CLIPBOARD` log line shows the size, not the contents |
| `tests/CMakeLists.txt` | `tests/fork/` excluded from upstream's test glob |

### Upstream fixes carried by the fork

- `src/video.cpp`: the AMF H.264 `profile` option is removed (commit `b063ddaf`). Upstream added it reading `cfg.profile`, which `video::config_t` doesn't have, so upstream `master` doesn't compile on Windows. Drop this change once upstream fixes it.

## Fork options

In `sunshine.conf`. `apply_config()` reads them and removes them from the option list, so Apollo doesn't report them as unrecognized. Both accept `enabled`/`disabled` (also `true`/`false`, `on`/`off`, `yes`/`no`, `1`/`0`).

| Option | Default | Effect |
|---|---|---|
| `fork_encoder_probe_reuse` | `enabled` | Reuse the encoder probe when only the display layout changed |
| `fork_clipboard_rich` | `enabled` | Rich clipboard sync for LuzLunar; `disabled` answers `type=bundle` with 400, like stock Apollo |

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

## Tests

`tests/fork/` holds standalone programs with no Apollo dependencies, built and run by the "Fork unit tests" step of the Windows build:

- `clipbundle_test.cpp`: the bundle format and send rules (shared with LuzLunar).
- `clipboard_win_smoke.cpp`: a round trip through the real Windows clipboard; it skips when the runner has no clipboard.
