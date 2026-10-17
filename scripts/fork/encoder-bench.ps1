<#
.SYNOPSIS
  Measures what NVENC's settings buy in picture quality at Apollo's stream settings, on this PC's GPU.

.DESCRIPTION
  Fork-only test tool; Apollo doesn't use it. See encoder-bench.md next to it.

  -Mode Check     Checks ffmpeg: NVENC encoders and the options this script uses, libvmaf, ddagrab.
  -Mode AddApps   Adds "Bench ..." apps to Apollo's apps.json (back it up first). They show the test
                  pages full screen; the capture apps also record a clip, and the latency apps set
                  the NVENC preset through encoder-overrides.
  -Mode Capture   Records one test page losslessly from the display being streamed. Started by the
                  "Bench capture ..." apps; the stream must be running, with its display the only one.
  -Mode Encode    Encodes the recorded clips with Apollo's rate control at each combination of codec,
                  preset, adaptive quantization and multipass, scores them against the lossless clip,
                  and writes results.csv and results.md.

  Windows PowerShell 5.1 or later.
#>
[CmdletBinding()]
param(
  [ValidateSet('Check', 'AddApps', 'Capture', 'Encode')]
  [string]$Mode = 'Check',
  [string]$Ffmpeg = 'ffmpeg',
  [string]$OutDir = 'C:\LuzVDA\r5\bench',
  [ValidateSet('text', 'motion')]
  [string]$Clip = 'text',
  [int]$Seconds = 5,
  [int]$Fps = 120,
  [int[]]$BitrateKbps = @(),
  [ValidateSet('core', 'quick')]
  [string]$Matrix = 'core',
  [int]$VmafSubsample = 10,
  [string]$AppsJson = 'C:\Program Files\Apollo\config\apps.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Write-Step([string]$text) {
  Write-Host ("[{0:HH:mm:ss}Z] {1}" -f [DateTime]::UtcNow, $text)
}

# Quotes one argument for a Windows command line (the rules CommandLineToArgvW reads back)
function ConvertTo-Arg([string]$arg) {
  if ($arg -notmatch '[\s"]' -and $arg.Length -gt 0) {
    return $arg
  }
  $out = '"'
  $slashes = 0
  foreach ($c in $arg.ToCharArray()) {
    if ($c -eq '\') {
      $slashes++
      continue
    }
    if ($c -eq '"') {
      $out += ('\' * ($slashes * 2 + 1)) + '"'
    } else {
      $out += ('\' * $slashes) + $c
    }
    $slashes = 0
  }
  return $out + ('\' * ($slashes * 2)) + '"'
}

# Runs ffmpeg with these arguments in $Dir; returns its exit code and stderr, and saves stderr to $Log
function Invoke-Ffmpeg([string[]]$Arguments, [string]$Dir, [string]$Log) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $script:FfmpegPath
  $psi.Arguments = ($Arguments | ForEach-Object { ConvertTo-Arg $_ }) -join ' '
  $psi.WorkingDirectory = $Dir
  $psi.UseShellExecute = $false
  $psi.RedirectStandardError = $true
  $psi.RedirectStandardOutput = $true
  $psi.CreateNoWindow = $true
  $p = [System.Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEnd()
  $p.WaitForExit()
  [void]$stdout.Result
  if ($Log) {
    Set-Content -Path $Log -Value ("ffmpeg " + $psi.Arguments + "`r`n`r`n" + $stderr) -Encoding UTF8
  }
  return [pscustomobject]@{ Code = $p.ExitCode; Err = $stderr }
}

function Resolve-Ffmpeg {
  $cmd = Get-Command $Ffmpeg -ErrorAction SilentlyContinue
  if (-not $cmd) {
    throw "ffmpeg not found: $Ffmpeg"
  }
  $script:FfmpegPath = $cmd.Source
}

# What ffmpeg prints on stdout for these arguments (its -h and list output)
function Get-FfmpegOutput([string]$arguments) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $script:FfmpegPath
  $psi.Arguments = "-hide_banner $arguments"
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $p = [System.Diagnostics.Process]::Start($psi)
  $errTask = $p.StandardError.ReadToEndAsync()
  $out = $p.StandardOutput.ReadToEnd()
  $p.WaitForExit()
  [void]$errTask.Result
  return $out
}

# The option names and pixel formats an encoder's help lists (ffmpeg -h encoder=<name>)
function Get-EncoderOptions([string]$encoder) {
  $help = Get-FfmpegOutput "-h encoder=$encoder"
  $names = @{}
  $formats = ''
  foreach ($line in $help -split "`n") {
    if ($line -match '^\s+-([A-Za-z0-9_\-]+)\s') {
      $names[$Matches[1]] = $true
    }
    if ($line -match 'Supported pixel formats:\s*(.*)$') {
      $formats = $Matches[1].Trim()
    }
  }
  return [pscustomobject]@{ Options = $names; PixelFormats = ($formats -split '\s+') }
}

# What this ffmpeg can do; the matrix leaves out what it can't
function Get-Capabilities {
  Resolve-Ffmpeg
  $encoders = Get-FfmpegOutput '-encoders'
  $filters = Get-FfmpegOutput '-filters'
  $caps = [ordered]@{
    ffmpeg      = $script:FfmpegPath
    hevc_nvenc  = $encoders -match '\shevc_nvenc\s'
    av1_nvenc   = $encoders -match '\sav1_nvenc\s'
    libvmaf     = $filters -match '\slibvmaf\s'
    ddagrab     = $filters -match '\sddagrab\s'
    psnr        = $filters -match '\spsnr\s'
    ssim        = $filters -match '\sssim\s'
    hevcOptions = $null
    av1Options  = $null
  }
  if ($caps.hevc_nvenc) {
    $caps.hevcOptions = Get-EncoderOptions 'hevc_nvenc'
  }
  if ($caps.av1_nvenc) {
    $caps.av1Options = Get-EncoderOptions 'av1_nvenc'
  }
  return $caps
}

# The NVENC options this script sets; the encode runs check each before using it
# (-bf and -g are ffmpeg's general options, which an encoder's help doesn't list)
$script:NeededOptions = @('preset', 'tune', 'rc', 'multipass', 'spatial-aq', 'temporal-aq', 'zerolatency', 'delay', 'rc-lookahead', 'ldkfs')

function Invoke-Check {
  $caps = Get-Capabilities
  $ok = $true
  Write-Host "ffmpeg: $($caps.ffmpeg)"
  foreach ($k in @('hevc_nvenc', 'av1_nvenc', 'libvmaf', 'ddagrab', 'psnr', 'ssim')) {
    Write-Host ("  {0,-11} {1}" -f $k, $(if ($caps[$k]) { 'yes' } else { 'NO' }))
  }
  foreach ($enc in @(@('hevc_nvenc', $caps.hevcOptions), @('av1_nvenc', $caps.av1Options))) {
    $name = $enc[0]
    $opts = $enc[1]
    if (-not $opts) {
      continue
    }
    $missing = @($script:NeededOptions | Where-Object { -not $opts.Options.ContainsKey($_) })
    Write-Host ("  {0} options missing: {1}" -f $name, $(if ($missing.Count) { $missing -join ', ' } else { 'none' }))
    Write-Host ("  {0} rgb_mode: {1}; yuv444p: {2}" -f $name, $opts.Options.ContainsKey('rgb_mode'), ($opts.PixelFormats -contains 'yuv444p'))
    if ($missing -contains 'preset' -or $missing -contains 'rc' -or $missing -contains 'tune') {
      $ok = $false
    }
  }
  if (-not ($caps.hevc_nvenc -and $caps.libvmaf -and $caps.ddagrab -and $caps.psnr)) {
    $ok = $false
  }
  Write-Host $(if ($ok) { 'CHECK OK' } else { 'CHECK FAILED' })
  if (-not $ok) {
    exit 1
  }
}

# ---------------------------------------------------------------------------------------------
# Test pages

$script:TextPage = @'
<!doctype html>
<html><head><meta charset="utf-8"><title>Bench text</title>
<style>
  html, body { margin: 0; background: #fff; color: #1f1f1f; font: 15px/1.55 "Segoe UI", sans-serif; }
  #bar { position: fixed; top: 0; left: 0; right: 0; height: 40px; background: #f3f3f3; border-bottom: 1px solid #d0d0d0;
         display: flex; align-items: center; gap: 10px; padding: 0 14px; z-index: 2; font-size: 13px; }
  #bar span { display: inline-block; width: 18px; height: 18px; border-radius: 3px; }
  #doc { max-width: 900px; margin: 64px auto 0; padding: 0 40px 2000px; }
  h1 { font-size: 28px; color: #1a4d8f; margin: 1.2em 0 .4em; } h2 { font-size: 20px; color: #2b579a; margin: 1.1em 0 .3em; }
  a { color: #0563c1; } mark { background: #fff2a8; } .red { color: #c00000; } .green { color: #2e7d32; }
  table { border-collapse: collapse; margin: 12px 0; font-size: 13px; } td, th { border: 1px solid #bfbfbf; padding: 3px 8px; }
  th { background: #dde8f6; } tr:nth-child(even) td { background: #f7f7f7; }
  pre { background: #1e1e1e; color: #d4d4d4; font: 13px/1.45 Consolas, monospace; padding: 10px 14px; border-radius: 4px; }
  .k { color: #569cd6; } .s { color: #ce9178; } .c { color: #6a9955; } .n { color: #b5cea8; }
  small { font-size: 11px; color: #555; }
</style></head>
<body>
<div id="bar"><b>Document1 - Word</b><span style="background:#2b579a"></span><span style="background:#c43e1c"></span>
<span style="background:#217346"></span><span style="background:#7719aa"></span><i>Calibri (Body)</i> 11 <b>B</b> <i>I</i> <u>U</u></div>
<div id="doc"></div>
<script>
// The same document every time: a seeded generator
let seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }
const words = ("the stream encoder frame display latency bitrate quality network client host packet picture color text " +
  "window budget quarter report revenue forecast meeting schedule project review summary analysis customer product " +
  "market growth team design release update version server storage memory process thread queue buffer signal").split(" ");
function sentence() {
  let n = 8 + Math.floor(rnd() * 14), s = [];
  for (let i = 0; i < n; i++) {
    let w = words[Math.floor(rnd() * words.length)], r = rnd();
    if (r < 0.03) w = '<a href="#">' + w + '</a>'; else if (r < 0.05) w = '<b>' + w + '</b>';
    else if (r < 0.06) w = '<mark>' + w + '</mark>'; else if (r < 0.07) w = '<span class="red">' + w + '</span>';
    else if (r < 0.08) w = '<span class="green">' + w + '</span>'; else if (r < 0.09) w = (Math.floor(rnd() * 9000) + 100).toString();
    s.push(w);
  }
  s[0] = s[0].charAt(0).toUpperCase() + s[0].slice(1);
  return s.join(" ") + ".";
}
let html = "";
for (let sec = 1; sec <= 40; sec++) {
  html += "<h1>" + sec + ". " + sentence().slice(0, 40) + "</h1>";
  for (let p = 0; p < 3; p++) { let t = ""; for (let i = 0; i < 4 + Math.floor(rnd() * 4); i++) t += sentence() + " "; html += "<p>" + t + "</p>"; }
  if (sec % 3 == 1) {
    html += "<table><tr><th>Region</th><th>Q1</th><th>Q2</th><th>Q3</th><th>Q4</th><th>Change</th></tr>";
    for (let r = 0; r < 6; r++) {
      html += "<tr><td>" + words[Math.floor(rnd() * words.length)] + "</td>";
      for (let c = 0; c < 4; c++) html += "<td>" + (rnd() * 10000).toFixed(2) + "</td>";
      let ch = (rnd() * 40 - 20).toFixed(1);
      html += '<td class="' + (ch < 0 ? "red" : "green") + '">' + ch + "%</td></tr>";
    }
    html += "</table>";
  }
  if (sec % 3 == 2) {
    html += '<pre><span class="c">// ' + sentence() + '</span>\n<span class="k">for</span> (<span class="k">auto</span> &amp;frame : frames) {\n' +
            '  encode(frame, <span class="n">' + Math.floor(rnd() * 999) + '</span>, <span class="s">"' + words[Math.floor(rnd() * words.length)] + '"</span>);\n}</pre>';
  }
  html += "<h2>" + sentence().slice(0, 50) + "</h2><ul>";
  for (let i = 0; i < 4; i++) html += "<li>" + sentence() + "</li>";
  html += "</ul><p><small>" + sentence() + " " + sentence() + "</small></p>";
}
document.getElementById("doc").innerHTML = html;
// Scrolls at a steady 900 CSS pixels a second, like a long wheel scroll
let start = null;
function step(t) { if (start === null) start = t; window.scrollTo(0, (t - start) * 0.9); requestAnimationFrame(step); }
requestAnimationFrame(step);
</script></body></html>
'@

$script:MotionPage = @'
<!doctype html>
<html><head><meta charset="utf-8"><title>Bench motion</title>
<style>html, body { margin: 0; height: 100%; overflow: hidden; background: #000; } canvas { width: 100vw; height: 100vh; display: block; }
#hud { position: fixed; left: 24px; bottom: 20px; color: #fff; font: bold 22px "Segoe UI", sans-serif; text-shadow: 0 0 4px #000; }
#hud2 { position: fixed; right: 24px; top: 20px; color: #ffd54a; font: 18px Consolas, monospace; text-shadow: 0 0 4px #000; }</style></head>
<body><canvas id="c"></canvas><div id="hud"></div><div id="hud2"></div>
<script>
// A fast, detailed flyover over procedural terrain: a stand-in for a game, with a HUD
const canvas = document.getElementById("c");
const gl = canvas.getContext("webgl", { antialias: false, preserveDrawingBuffer: false });
function resize() { canvas.width = innerWidth * devicePixelRatio; canvas.height = innerHeight * devicePixelRatio; gl.viewport(0, 0, canvas.width, canvas.height); }
resize(); addEventListener("resize", resize);
const vs = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
const fs = `precision highp float;
uniform vec2 res; uniform float t;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) { vec2 i = floor(p), f = fract(p); vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y); }
float fbm(vec2 p) { float v = 0.0, a = 0.5; for (int i = 0; i < 6; i++) { v += a * noise(p); p = p * 2.03 + 17.0; a *= 0.5; } return v; }
float height(vec2 p) { return fbm(p * 0.35) * 3.2; }
void main() {
  vec2 uv = (gl_FragCoord.xy - 0.5 * res) / res.y;
  float z = t * 9.0;
  vec3 ro = vec3(sin(t * 0.7) * 4.0, 3.6, z);
  vec3 ta = vec3(sin(t * 0.7 + 0.6) * 4.0, 2.4, z + 6.0);
  vec3 fw = normalize(ta - ro), rt = normalize(cross(vec3(sin(t * 0.5) * 0.25, 1, 0), fw)), up = cross(fw, rt);
  vec3 rd = normalize(uv.x * rt + uv.y * up + 1.4 * fw);
  float d = 0.0; vec3 col = vec3(0.0); bool hit = false;
  for (int i = 0; i < 90; i++) { vec3 p = ro + rd * d; float h = p.y - height(p.xz); if (h < 0.01 * d) { hit = true; break; } d += h * 0.45; if (d > 60.0) break; }
  vec3 sky = mix(vec3(0.55, 0.7, 0.95), vec3(0.15, 0.3, 0.7), clamp(rd.y * 2.0, 0.0, 1.0));
  if (hit) {
    vec3 p = ro + rd * d; vec2 e = vec2(0.02, 0.0);
    vec3 n = normalize(vec3(height(p.xz - e.xy) - height(p.xz + e.xy), 2.0 * e.x, height(p.xz - e.yx) - height(p.xz + e.yx)));
    float g = noise(p.xz * 6.0) * 0.5 + noise(p.xz * 23.0) * 0.5;
    vec3 base = mix(vec3(0.22, 0.45, 0.12), vec3(0.45, 0.38, 0.28), smoothstep(1.4, 2.2, p.y)) * (0.6 + 0.6 * g);
    base = mix(base, vec3(0.95), smoothstep(2.5, 2.9, p.y + g * 0.3));
    float l = clamp(dot(n, normalize(vec3(0.6, 0.7, 0.3))), 0.0, 1.0);
    col = mix(base * (0.25 + 0.9 * l), sky, 1.0 - exp(-0.0025 * d * d));
  } else { col = sky + vec3(1.0, 0.9, 0.7) * pow(max(dot(rd, normalize(vec3(0.6, 0.35, 0.7))), 0.0), 64.0); }
  gl_FragColor = vec4(pow(col, vec3(0.9)), 1.0);
}`;
function sh(type, src) { const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s); return s; }
const pr = gl.createProgram(); gl.attachShader(pr, sh(gl.VERTEX_SHADER, vs)); gl.attachShader(pr, sh(gl.FRAGMENT_SHADER, fs)); gl.linkProgram(pr); gl.useProgram(pr);
const buf = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buf);
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
const loc = gl.getAttribLocation(pr, "p"); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
const uRes = gl.getUniformLocation(pr, "res"), uT = gl.getUniformLocation(pr, "t");
const hud = document.getElementById("hud"), hud2 = document.getElementById("hud2");
let start = null, frames = 0;
function frame(ts) {
  if (start === null) start = ts;
  const t = (ts - start) / 1000;
  gl.uniform2f(uRes, canvas.width, canvas.height); gl.uniform1f(uT, t); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  frames++;
  hud.textContent = "HP 100   AMMO " + (30 - Math.floor(t * 3) % 31) + "/120   SCORE " + Math.floor(t * 137);
  hud2.textContent = "t=" + t.toFixed(2) + "  frame " + frames;
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script></body></html>
'@

function Write-Pages([string]$dir) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  Set-Content -Path (Join-Path $dir 'text.html') -Value $script:TextPage -Encoding UTF8
  Set-Content -Path (Join-Path $dir 'motion.html') -Value $script:MotionPage -Encoding UTF8
}

function Get-EdgePath {
  foreach ($p in @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe")) {
    if (Test-Path $p) {
      return $p
    }
  }
  throw 'Microsoft Edge not found'
}

# ---------------------------------------------------------------------------------------------
# AddApps

function Invoke-AddApps {
  $pages = Join-Path $OutDir 'pages'
  Write-Pages $pages
  $edge = Get-EdgePath
  Resolve-Ffmpeg
  $self = $PSCommandPath
  $profileDir = Join-Path $OutDir 'edge-profile'

  function Edge-Cmd([string]$page) {
    $url = 'file:///' + ((Join-Path $pages $page) -replace '\\', '/')
    return '"' + $edge + '" --user-data-dir="' + $profileDir + '" --no-first-run --no-default-browser-check --disable-features=msEdgeSidebarV2,Translate --start-fullscreen --app=' + $url
  }
  function Capture-Cmd([string]$clip) {
    return 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $self + '" -Mode Capture -Clip ' + $clip +
           ' -OutDir "' + $OutDir + '" -Ffmpeg "' + $script:FfmpegPath + '" -Seconds ' + $Seconds + ' -Fps ' + $Fps
  }

  $apps = @(
    [ordered]@{ name = 'Bench capture text'; cmd = (Edge-Cmd 'text.html'); detached = @(Capture-Cmd 'text') },
    [ordered]@{ name = 'Bench capture motion'; cmd = (Edge-Cmd 'motion.html'); detached = @(Capture-Cmd 'motion') },
    [ordered]@{ name = 'Bench P1'; cmd = (Edge-Cmd 'motion.html'); 'encoder-overrides' = [ordered]@{ nvenc_preset = 1 } },
    [ordered]@{ name = 'Bench P4'; cmd = (Edge-Cmd 'motion.html'); 'encoder-overrides' = [ordered]@{ nvenc_preset = 4 } },
    [ordered]@{ name = 'Bench P7'; cmd = (Edge-Cmd 'motion.html'); 'encoder-overrides' = [ordered]@{ nvenc_preset = 7 } },
    [ordered]@{ name = 'Bench P7 full-res'; cmd = (Edge-Cmd 'motion.html'); 'encoder-overrides' = [ordered]@{ nvenc_preset = 7; nvenc_twopass = 'full_res' } }
  )

  $json = Get-Content -Raw -Path $AppsJson | ConvertFrom-Json
  $kept = @($json.apps | Where-Object { $_.name -notlike 'Bench *' })
  foreach ($a in $apps) {
    $a['uuid'] = ([guid]::NewGuid().ToString().ToUpper())
    $a['image-path'] = ''
    $kept += [pscustomobject]$a
  }
  $json.apps = $kept
  $text = $json | ConvertTo-Json -Depth 10
  [System.IO.File]::WriteAllText($AppsJson, $text, (New-Object System.Text.UTF8Encoding $false))
  Write-Step "added $($apps.Count) Bench apps to $AppsJson"
  foreach ($a in $apps) {
    Write-Host ("  {0}: {1}" -f $a.name, $(if ($a.Contains('encoder-overrides')) { ($a['encoder-overrides'] | ConvertTo-Json -Compress) } else { 'records a clip' }))
  }
}

# ---------------------------------------------------------------------------------------------
# Capture: run by a "Bench capture ..." app while the stream shows its page

function Invoke-Capture {
  Resolve-Ffmpeg
  $clips = Join-Path $OutDir 'clips'
  New-Item -ItemType Directory -Force -Path $clips | Out-Null
  $log = Join-Path $clips "$Clip-capture.txt"
  "capture $Clip start $([DateTime]::UtcNow.ToString('HH:mm:ss.fff'))Z" | Set-Content $log

  # The page needs a moment to open and go full screen
  Start-Sleep -Seconds 5

  # The stream's display is the only one, but find its DXGI output by its size anyway
  $outputIdx = -1
  $size = ''
  for ($i = 0; $i -lt 4; $i++) {
    $r = Invoke-Ffmpeg @('-hide_banner', '-f', 'lavfi', '-i', "ddagrab=output_idx=${i}:framerate=${Fps}", '-frames:v', '1', '-f', 'null', '-') $clips $null
    if ($r.Code -eq 0 -and $r.Err -match ',\s*(\d{3,5})x(\d{3,5})') {
      $size = "$($Matches[1])x$($Matches[2])"
      $outputIdx = $i
      break
    }
  }
  "output $outputIdx size $size" | Add-Content $log
  if ($outputIdx -lt 0) {
    'no DXGI output could be captured' | Add-Content $log
    exit 1
  }

  # Lossless HEVC. NVENC converts the desktop's BGRA to 4:4:4 itself where ffmpeg has rgb_mode;
  # otherwise ffmpeg converts it, which may drop frames at this size
  $opts = Get-EncoderOptions 'hevc_nvenc'
  $out = "$Clip-lossless.mkv"
  $common = @('-hide_banner', '-y', '-f', 'lavfi', '-i', "ddagrab=output_idx=${outputIdx}:framerate=${Fps}:draw_mouse=0", '-t', "$Seconds")
  if ($opts.Options.ContainsKey('rgb_mode')) {
    $ffArgs = $common + @('-c:v', 'hevc_nvenc', '-preset', 'p1', '-tune', 'lossless', '-rgb_mode', 'yuv444', $out)
  } else {
    $ffArgs = $common + @('-vf', 'hwdownload,format=bgra,format=yuv444p', '-c:v', 'hevc_nvenc', '-preset', 'p1', '-tune', 'lossless', $out)
  }
  "capture from $([DateTime]::UtcNow.ToString('HH:mm:ss.fff'))Z" | Add-Content $log
  $r = Invoke-Ffmpeg $ffArgs $clips (Join-Path $clips "$Clip-ffmpeg.txt")
  "capture to $([DateTime]::UtcNow.ToString('HH:mm:ss.fff'))Z, exit $($r.Code)" | Add-Content $log
  $tail = ($r.Err -split "`n" | Where-Object { $_ -match 'frame=|dup=|drop=|Error|error' } | Select-Object -Last 3) -join ' | '
  "ffmpeg: $tail" | Add-Content $log
  exit $r.Code
}

# ---------------------------------------------------------------------------------------------
# Encode

function Get-FreeGB([string]$path) {
  $root = [System.IO.Path]::GetPathRoot((Resolve-Path $path).Path)
  $drive = Get-PSDrive -Name $root.Substring(0, 1)
  return [math]::Round($drive.Free / 1GB, 1)
}

function Get-ClipInfo([string]$file) {
  $r = Invoke-Ffmpeg @('-hide_banner', '-i', $file) (Split-Path $file) $null
  $line = ($r.Err -split "`n" | Where-Object { $_ -match 'Stream #.*Video:' } | Select-Object -First 1)
  if ($line -and $line -match 'Video: [^,]+, (\w+)' -and $line -match ', (\d{2,5})x(\d{2,5})') {
    $size = $Matches
    [void]($line -match 'Video: [^,]+, (\w+)')
    return [pscustomobject]@{ PixFmt = $Matches[1]; Width = [int]$size[1]; Height = [int]$size[2] }
  }
  throw "can't read $file"
}

function Get-Configs([string]$codec) {
  $list = New-Object System.Collections.Generic.List[object]
  if ($Matrix -eq 'quick') {
    foreach ($preset in @('p1', 'p7')) {
      $list.Add([pscustomobject]@{ Preset = $preset; Aq = 'off'; Multipass = 'qres' })
    }
    $list.Add([pscustomobject]@{ Preset = 'p4'; Aq = 'spatial'; Multipass = 'qres' })
    return $list
  }
  foreach ($preset in @('p1', 'p4', 'p7')) {
    foreach ($aq in @('off', 'spatial')) {
      foreach ($mp in @('qres', 'fullres')) {
        $list.Add([pscustomobject]@{ Preset = $preset; Aq = $aq; Multipass = $mp })
      }
    }
  }
  # Temporal AQ (which Apollo doesn't offer) and single pass, at one preset each
  $list.Add([pscustomobject]@{ Preset = 'p4'; Aq = 'temporal'; Multipass = 'qres' })
  $list.Add([pscustomobject]@{ Preset = 'p4'; Aq = 'both'; Multipass = 'qres' })
  $list.Add([pscustomobject]@{ Preset = 'p1'; Aq = 'off'; Multipass = 'disabled' })
  return $list
}

function Invoke-Encode {
  $caps = Get-Capabilities
  if (-not ($caps.hevc_nvenc -and $caps.libvmaf -and $caps.psnr)) {
    throw 'ffmpeg needs hevc_nvenc, libvmaf and psnr; run -Mode Check'
  }
  if ($BitrateKbps.Count -eq 0) {
    throw 'give -BitrateKbps, the bitrate the client streams at'
  }
  $clips = Join-Path $OutDir 'clips'
  $work = Join-Path $OutDir 'work'
  $runs = Join-Path $OutDir 'runs'
  New-Item -ItemType Directory -Force -Path $work, $runs | Out-Null
  $threads = [Environment]::ProcessorCount
  $script:ClipNotes = @()
  $rows = New-Object System.Collections.Generic.List[object]
  $csv = Join-Path $OutDir 'results.csv'

  foreach ($clip in @('text', 'motion')) {
    $lossless = Join-Path $clips "$clip-lossless.mkv"
    if (-not (Test-Path $lossless)) {
      Write-Step "no $clip clip, skipping it"
      continue
    }
    $info = Get-ClipInfo $lossless
    $w = $info.Width
    $h = $info.Height
    $frames = $Seconds * $Fps

    # The reference, decoded once: 4:4:4 for text (where colour detail matters), 4:2:0 for motion
    $refFmt = if ($clip -eq 'text') { 'yuv444p' } else { 'yuv420p' }
    $ref = Join-Path $work "$clip-ref.yuv"
    $needGB = [math]::Ceiling($w * $h * $(if ($refFmt -eq 'yuv444p') { 3 } else { 1.5 }) * $frames / 1GB) + 2
    if (-not (Test-Path $ref)) {
      $free = Get-FreeGB $work
      if ($free -lt $needGB) {
        throw "the $clip reference needs $needGB GB and $work has $free GB free"
      }
      Write-Step "decoding the $clip reference ($w x $h, $refFmt, $frames frames)"
      $r = Invoke-Ffmpeg @('-hide_banner', '-y', '-i', $lossless, '-frames:v', "$frames", '-pix_fmt', $refFmt, '-f', 'rawvideo', "$clip-ref.yuv") $work (Join-Path $runs "$clip-ref.txt")
      if ($r.Code -ne 0) {
        throw "decoding the $clip reference failed; see $runs\$clip-ref.txt"
      }
    }
    $refIn = @('-f', 'rawvideo', '-pix_fmt', $refFmt, '-s', "${w}x${h}", '-r', "$Fps", '-i', "$clip-ref.yuv")
    $frameBytes = [double]$w * $h * $(if ($refFmt -eq 'yuv444p') { 3 } else { 1.5 })
    $frames = [int]((Get-Item $ref).Length / $frameBytes)
    $clipSeconds = $frames / $Fps
    $script:ClipNotes += "${clip}: $w x $h, captured as $($info.PixFmt), reference $refFmt, $frames frames"

    $codecs = New-Object System.Collections.Generic.List[object]
    $codecs.Add([pscustomobject]@{ Name = 'hevc'; Encoder = 'hevc_nvenc'; PixFmt = 'yuv420p'; Opts = $caps.hevcOptions })
    if ($clip -eq 'text' -and $caps.hevcOptions.PixelFormats -contains 'yuv444p') {
      $codecs.Add([pscustomobject]@{ Name = 'hevc444'; Encoder = 'hevc_nvenc'; PixFmt = 'yuv444p'; Opts = $caps.hevcOptions })
    }
    if ($caps.av1_nvenc) {
      $codecs.Add([pscustomobject]@{ Name = 'av1'; Encoder = 'av1_nvenc'; PixFmt = 'yuv420p'; Opts = $caps.av1Options })
      if ($clip -eq 'text' -and $caps.av1Options.PixelFormats -contains 'yuv444p') {
        $codecs.Add([pscustomobject]@{ Name = 'av1_444'; Encoder = 'av1_nvenc'; PixFmt = 'yuv444p'; Opts = $caps.av1Options })
      }
    }

    foreach ($kbps in $BitrateKbps) {
      foreach ($codec in $codecs) {
        foreach ($cfg in (Get-Configs $codec.Name)) {
          $id = "$clip-$($codec.Name)-$kbps-$($cfg.Preset)-aq_$($cfg.Aq)-mp_$($cfg.Multipass)"
          $row = [ordered]@{
            clip = $clip; codec = $codec.Name; chroma = $(if ($codec.PixFmt -eq 'yuv444p') { '444' } else { '420' }); bitrate_kbps = $kbps
            preset = $cfg.Preset; aq = $cfg.Aq; multipass = $cfg.Multipass; encode_fps = ''; actual_kbps = ''
            vmaf_mean = ''; vmaf_harmonic = ''; vmaf_min = ''; psnr_y = ''; psnr_u = ''; psnr_v = ''; psnr_avg = ''; ssim_all = ''; note = ''
          }
          $opts = $codec.Opts.Options
          $skip = @()
          if ($cfg.Aq -in @('temporal', 'both') -and -not $opts.ContainsKey('temporal-aq')) { $skip += 'no temporal-aq option' }
          if (-not $opts.ContainsKey('multipass')) { $skip += 'no multipass option' }
          if ($skip.Count) {
            $row.note = $skip -join '; '
            $rows.Add([pscustomobject]$row)
            continue
          }

          # Apollo's rate control (src/nvenc/nvenc_base.cpp): lowest-delay tuning, CBR, a one-frame
          # buffer, no lookahead, no B-frames, one endless GOP
          $bufBits = [int]([double]$kbps * 1000 / $Fps)
          $enc = @('-c:v', $codec.Encoder, '-preset', $cfg.Preset, '-tune', 'ull', '-rc', 'cbr',
                   '-b:v', "${kbps}k", '-maxrate', "${kbps}k", '-bufsize', "$bufBits",
                   '-bf', '0', '-g', '1000000', '-rc-lookahead', '0', '-multipass', $cfg.Multipass,
                   '-spatial-aq', $(if ($cfg.Aq -in @('spatial', 'both')) { '1' } else { '0' }))
          if ($opts.ContainsKey('temporal-aq')) {
            $enc += @('-temporal-aq', $(if ($cfg.Aq -in @('temporal', 'both')) { '1' } else { '0' }))
          }
          foreach ($o in @(@('zerolatency', '1'), @('delay', '0'), @('ldkfs', '1'))) {
            if ($opts.ContainsKey($o[0])) { $enc += @("-$($o[0])", $o[1]) }
          }
          $ext = if ($codec.Encoder -eq 'av1_nvenc') { 'ivf' } else { 'hevc' }
          $file = "$id.$ext"

          Write-Step "encode $id"
          $sw = [Diagnostics.Stopwatch]::StartNew()
          $r = Invoke-Ffmpeg (@('-hide_banner', '-y') + $refIn + @('-pix_fmt', $codec.PixFmt) + $enc + @($file)) $work (Join-Path $runs "$id-encode.txt")
          $sw.Stop()
          if ($r.Code -ne 0) {
            $row.note = 'encode failed: ' + (($r.Err -split "`n" | Where-Object { $_ -match 'rror|nvalid|not supported' } | Select-Object -First 2) -join ' | ').Trim()
            $rows.Add([pscustomobject]$row)
            $rows | Export-Csv -NoTypeInformation -Path $csv
            continue
          }
          $row.encode_fps = [math]::Round($frames / $sw.Elapsed.TotalSeconds, 1)
          $row.actual_kbps = [math]::Round((Get-Item (Join-Path $work $file)).Length * 8 / 1000 / $clipSeconds)

          # Scores: VMAF on the brightness (every Nth frame), PSNR and SSIM per plane in the reference's
          # format, so 4:2:0's colour loss shows on text
          $vmafLog = "$id-vmaf.json"
          $graph = "[0:v]format=$refFmt,split=3[d1][d2][d3];[1:v]split=3[r1][r2][r3];" +
                   "[d1]format=yuv420p[dv];[r1]format=yuv420p[rv];[dv][rv]libvmaf=model=version=vmaf_v0.6.1:n_subsample=${VmafSubsample}:n_threads=${threads}:log_fmt=json:log_path=$vmafLog;" +
                   "[d2][r2]psnr;[d3][r3]ssim"
          $r = Invoke-Ffmpeg (@('-hide_banner', '-y', '-i', $file) + $refIn + @('-lavfi', $graph, '-f', 'null', '-')) $work (Join-Path $runs "$id-score.txt")
          if ($r.Code -ne 0) {
            $row.note = 'scoring failed'
          } else {
            if ($r.Err -match 'PSNR y:([\d.inf]+) u:([\d.inf]+) v:([\d.inf]+) average:([\d.inf]+)') {
              $row.psnr_y = $Matches[1]; $row.psnr_u = $Matches[2]; $row.psnr_v = $Matches[3]; $row.psnr_avg = $Matches[4]
            }
            if ($r.Err -match 'SSIM .*All:([\d.]+)') {
              $row.ssim_all = $Matches[1]
            }
            $vmafPath = Join-Path $work $vmafLog
            if (Test-Path $vmafPath) {
              $v = (Get-Content -Raw $vmafPath | ConvertFrom-Json).pooled_metrics.vmaf
              $row.vmaf_mean = [math]::Round($v.mean, 2)
              $row.vmaf_harmonic = [math]::Round($v.harmonic_mean, 2)
              $row.vmaf_min = [math]::Round($v.min, 2)
            }
          }
          Remove-Item (Join-Path $work $file) -ErrorAction SilentlyContinue
          $rows.Add([pscustomobject]$row)
          $rows | Export-Csv -NoTypeInformation -Path $csv
        }
      }
    }
  }

  Write-Summary $rows (Join-Path $OutDir 'results.md')
  Write-Step "done: $csv"
}

# A table per clip and codec: each setting against that codec's P1 default (Apollo's: P1, AQ off,
# quarter-resolution two-pass)
function Write-Summary($rows, [string]$path) {
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add("# Encoder benchmark, $([DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm'))Z")
  $lines.Add('')
  $lines.Add("VMAF: v0.6.1 on the brightness, every ${VmafSubsample}th frame. PSNR per plane against the lossless clip, in 4:4:4 for text. encode_fps is ffmpeg's throughput from raw frames on disk, not stream latency.")
  $lines.Add('')
  foreach ($n in $script:ClipNotes) {
    $lines.Add("- $n")
  }
  foreach ($group in ($rows | Group-Object clip, codec, bitrate_kbps)) {
    $items = @($group.Group)
    $base = $items | Where-Object { $_.preset -eq 'p1' -and $_.aq -eq 'off' -and $_.multipass -eq 'qres' } | Select-Object -First 1
    $lines.Add('')
    $lines.Add("## $($group.Name -replace ', ', ' / ')")
    $lines.Add('')
    $lines.Add('| preset | AQ | multipass | VMAF | ΔVMAF vs Apollo default | PSNR Y/U/V | SSIM | encode fps | kbps | note |')
    $lines.Add('|---|---|---|---|---|---|---|---|---|---|')
    foreach ($i in ($items | Sort-Object { if ($_.vmaf_mean -ne '') { -[double]$_.vmaf_mean } else { 0 } })) {
      $delta = ''
      if ($base -and $base.vmaf_mean -ne '' -and $i.vmaf_mean -ne '') {
        $delta = '{0:+0.00;-0.00;0.00}' -f ([double]$i.vmaf_mean - [double]$base.vmaf_mean)
      }
      $lines.Add("| $($i.preset) | $($i.aq) | $($i.multipass) | $($i.vmaf_mean) | $delta | $($i.psnr_y)/$($i.psnr_u)/$($i.psnr_v) | $($i.ssim_all) | $($i.encode_fps) | $($i.actual_kbps) | $($i.note) |")
    }
  }
  Set-Content -Path $path -Value $lines -Encoding UTF8
}

switch ($Mode) {
  'Check' { Invoke-Check }
  'AddApps' { Invoke-AddApps }
  'Capture' { Invoke-Capture }
  'Encode' { Invoke-Encode }
}
