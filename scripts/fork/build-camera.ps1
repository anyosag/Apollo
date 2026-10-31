# Builds Mac Camera (src/fork/camera.h; FORK.md, "Mac camera for LuzLunar") with Visual Studio's compiler
# and the Windows SDK, which has the virtual camera API (mfvirtualcamera.h): the media source Windows'
# Frame Server loads (tools/apollo-camera-source/) and the tool that registers it (tools/apollo-camera.cpp),
# both with the C runtime linked in, into src_assets/windows/camera/, where Apollo's installer takes them
# from into tools\. The source DLL's name has the version, as Frame Server may keep an older one loaded
# while an update installs.
#
# build-windows.yml runs it. Locally it needs Visual Studio or its Build Tools with the C++ workload and a
# Windows 11 SDK; a build without them has no Mac Camera:
#   powershell -File scripts/fork/build-camera.ps1 [-Version <version>]
param([string]$Version = 'dev')

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$src = "$root\tools\apollo-camera-source"
$out = "$root\src_assets\windows\camera"
$work = Join-Path ([IO.Path]::GetTempPath()) "apollo-camera-build-$PID"
New-Item -ItemType Directory -Force $work, $out | Out-Null
# Only this build's files
Get-ChildItem $out -Filter 'apollo-camera*' | Remove-Item -Force

$safe = $Version -replace '[^A-Za-z0-9.-]', '-'
$dll = "apollo-camera-source-$safe.dll"
Set-Content "$work\camera_build.h" @('#pragma once', "#define APOLLO_CAMERA_SOURCE_DLL L`"$dll`"") -Encoding ASCII

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) {
  throw 'Building Mac Camera needs Visual Studio or its Build Tools with the C++ workload'
}
$vcvars = "$vs\VC\Auxiliary\Build\vcvarsall.bat"

# Runs commands in Visual Studio's x64 environment, from the work folder, stopping at the first that fails
function Invoke-VsCommands([string[]]$commands) {
  $script = "$work\build.cmd"
  $lines = @('@echo off', "call `"$vcvars`" x64 >nul || exit /b 1", "cd /d `"$work`"") +
    ($commands | ForEach-Object { "$_ || exit /b 1" })
  Set-Content $script $lines -Encoding ASCII
  & cmd.exe /d /c $script
  if ($LASTEXITCODE -ne 0) {
    throw 'Building Mac Camera failed'
  }
}

# C++20 for src/fork/camera_policy.h; NOMINMAX, as it uses std::min and std::max
$cxxflags = "/nologo /utf-8 /std:c++20 /EHsc /W3 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /I`"$root`" /I`"$work`""
$libs = 'mfplat.lib mfuuid.lib ole32.lib oleaut32.lib advapi32.lib'
Invoke-VsCommands @(
  "cl $cxxflags /c `"$src\camera_source.cpp`" `"$src\camera_stream.cpp`""
  "link /nologo /dll /release /opt:ref /opt:icf /def:`"$src\apollo-camera-source.def`" /implib:apollo-camera-source.lib /out:`"$out\$dll`" camera_source.obj camera_stream.obj $libs"
  "cl $cxxflags /Fe`"$out\apollo-camera.exe`" `"$root\tools\apollo-camera.cpp`" $libs cfgmgr32.lib /link /release /subsystem:console"
)
Get-ChildItem $out -Filter 'apollo-camera*' | Format-Table Name, Length

Remove-Item $work -Recurse -Force
