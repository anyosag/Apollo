# Encoder benchmark

`encoder-bench.ps1` measures what NVENC's settings buy at Apollo's stream settings, on the PC's own GPU. Two numbers decide whether a setting is worth it:
- **the picture**, from encoding a lossless clip of the real stream display and scoring the result;
- **the delay**, from live streams with the setting, as the client's statistics report it.

It's a fork test tool. Apollo doesn't use it, and nothing here changes Apollo's defaults.

## What it measures

**Clips.** Two pages that run by themselves, recorded losslessly (HEVC 4:4:4) from the display being streamed, at the stream's size and rate:
- `text`: a document scrolling at a steady speed, with coloured links, tables and code;
- `motion`: a fast flyover of detailed terrain with a HUD, a stand-in for a game. Nobody needs to play.

**Encodes.** Each clip is encoded with Apollo's rate control (`src/nvenc/nvenc_base.cpp`):
- lowest-delay tuning, constant bitrate and a one-frame buffer;
- no lookahead, no B-frames, one endless GOP.

The `core` matrix varies the settings below, plus three extras: temporal AQ, temporal and spatial AQ together, and single pass.

| Setting | Values |
|---|---|
| Codec | HEVC 4:2:0; HEVC 4:4:4 (text only); AV1 4:2:0; AV1 4:4:4 (text only, if the GPU takes it) |
| Preset | P1, P4, P7 |
| Adaptive quantization | off, spatial |
| Multipass | quarter, full resolution |

Apollo's default is P1, AQ off, quarter-resolution two-pass. Each table compares the other settings with it.

**Scores:**
- VMAF (v0.6.1) on every 10th frame. VMAF only looks at brightness.
- PSNR per plane, and SSIM, against the lossless clip. For text this is in 4:4:4, so the colour detail 4:2:0 loses shows in the U and V columns.
- The encode's real bitrate.
- ffmpeg's encode throughput. This is not stream delay; the live runs measure that.

## Running it

On the PC, from an elevated PowerShell:

1. **ffmpeg** with `hevc_nvenc`, `libvmaf` and `ddagrab`, for example a Windows "full" or "gpl" build of ffmpeg 7. Check it:
   ```powershell
   .\encoder-bench.ps1 -Mode Check -Ffmpeg C:\path\ffmpeg.exe
   ```
2. **Folders and apps.** Back up `apps.json` first. `-OutDir` must be writable by the signed-in user, since Apollo starts the apps as that user.
   ```powershell
   .\encoder-bench.ps1 -Mode AddApps -Ffmpeg C:\path\ffmpeg.exe -OutDir C:\LuzVDA\r5\bench
   ```
   Then restart Apollo. This adds:

   | App | What it does |
   |---|---|
   | `Bench capture text`, `Bench capture motion` | Shows the page full screen; after 5 s records 5 s of it to `<OutDir>\clips` |
   | `Bench P1`, `Bench P4`, `Bench P7`, `Bench P7 full-res` | Shows the motion page with that preset, through `encoder-overrides` |
3. **From the client,** stream each capture app at the usual size and rate (3024x1964 at 120 fps) for about 20 s. Use SDR: the clips are 8-bit. Then stream each latency app for about 40 s. Note the client's end-of-stream statistics, chiefly its host processing latency.
4. **Encode and score**, at the bitrate the client uses. This takes about an hour for `core`.
   ```powershell
   .\encoder-bench.ps1 -Mode Encode -Ffmpeg C:\path\ffmpeg.exe -OutDir C:\LuzVDA\r5\bench -BitrateKbps 80000
   ```
   The results go to `<OutDir>\results.csv` and `results.md`; every ffmpeg run's log goes to `<OutDir>\runs`.

**Disk:** the decoded references take about `width × height × 3 × frames` bytes for text and half that for motion. At 3024x1964 for 5 s at 120 fps, that's about 16 GB in all, in `<OutDir>\work`.

## Reading the results

- **A slower preset is worth it** when its VMAF and PSNR gains are clear (as a rough guide, VMAF +1 or more), and the live run's host processing latency stays within a frame (8.3 ms at 120 fps).
- **Spatial or temporal AQ** helps where the text clip's scores rise without the motion clip's falling.
- **4:4:4** pays off on text when the U and V PSNR gain is large. VMAF won't show it.
- The live runs give the delay. ffmpeg's throughput doesn't: it includes reading and converting frames on the CPU.

## Limits

- The motion page is synthetic, so a real game may encode differently.
- Everything is 8-bit SDR. HDR streams (10-bit) aren't covered.
- One bitrate unless `-BitrateKbps` lists several.
