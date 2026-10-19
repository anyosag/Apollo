# Builds the "US - Mac" keyboard layout (src/fork/keyboard_layout/) with Visual Studio's compiler, as
# Microsoft's keyboard layout tool does: kbdluzus.dll for 64-bit Windows and its WOW64 copy for 32-bit
# programs, into src_assets/windows/keyboard/, where Apollo's installer takes them from. With -TestDir,
# it also builds the tests in tests/fork/ that check them into that folder.
#
# build-windows.yml runs it. Locally it needs Visual Studio or its Build Tools with the C++ workload:
#   powershell -File scripts/fork/build-keyboard-layout.ps1 [-TestDir <folder>]
param([string]$TestDir)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$src = "$root\src\fork\keyboard_layout"
$out = "$root\src_assets\windows\keyboard"
$work = Join-Path ([IO.Path]::GetTempPath()) "kbdluzus-build-$PID"
New-Item -ItemType Directory -Force $work, "$out\wow64" | Out-Null
if ($TestDir) {
  New-Item -ItemType Directory -Force $TestDir | Out-Null
  $TestDir = (Resolve-Path $TestDir).Path
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) {
  throw 'Building the keyboard layout needs Visual Studio or its Build Tools with the C++ workload'
}
$vcvars = "$vs\VC\Auxiliary\Build\vcvarsall.bat"

# Runs commands in Visual Studio's environment for an architecture (x64, or x64_x86 for 32-bit code),
# from the work folder, stopping at the first that fails
function Invoke-VsCommands([string]$arch, [string[]]$commands) {
  $script = "$work\$arch.cmd"
  $lines = @('@echo off', "call `"$vcvars`" $arch >nul || exit /b 1", "cd /d `"$work`"") +
    ($commands | ForEach-Object { "$_ || exit /b 1" })
  Set-Content $script $lines -Encoding ASCII
  & cmd.exe /d /c $script
  if ($LASTEXITCODE -ne 0) {
    throw "Building for $arch failed"
  }
}

# A layout DLL is only tables: no C runtime, no entry point, and everything in one read-only section,
# with the one export in kbdluzus.def
$cflags = '/nologo /c /utf-8 /W3 /WX /O1 /GS- /Gy /Zl'
$lflags = '/nologo /dll /noentry /nodefaultlib /subsystem:native /release /opt:ref /opt:icf ' +
  '/merge:.edata=.data /merge:.rdata=.data /merge:.text=.data /merge:.bss=.data /section:.data,re ' +
  "/ignore:4039,4078,4254 /def:`"$src\kbdluzus.def`" kbdluzus.res"

Invoke-VsCommands 'x64' @(
  "rc /nologo /fo kbdluzus.res `"$src\kbdluzus.rc`""
  "cl $cflags /Fokbdluzus-x64.obj `"$src\kbdluzus.c`""
  "link $lflags /implib:kbdluzus-x64.lib /out:`"$out\kbdluzus.dll`" kbdluzus-x64.obj"
)
# The WOW64 copy: 32-bit code whose tables keep 64-bit pointers
Invoke-VsCommands 'x64_x86' @(
  "cl $cflags /Gz /DBUILD_WOW6432 /Fokbdluzus-wow64.obj `"$src\kbdluzus.c`""
  "link $lflags /implib:kbdluzus-wow64.lib /out:`"$out\wow64\kbdluzus.dll`" kbdluzus-wow64.obj"
)
Get-ChildItem "$out\kbdluzus.dll", "$out\wow64\kbdluzus.dll" | Format-Table FullName, Length

if ($TestDir) {
  $tests = "$root\tests\fork"
  $cxxflags = '/nologo /utf-8 /std:c++17 /EHsc /W4 /WX'
  Invoke-VsCommands 'x64' @(
    "cl $cxxflags /Fe`"$TestDir\keyboard_layout_tables_test.exe`" `"$tests\keyboard_layout_tables_test.cpp`""
    "cl $cxxflags /Fe`"$TestDir\keyboard_layout_win_smoke.exe`" `"$tests\keyboard_layout_win_smoke.cpp`" user32.lib"
  )
  Invoke-VsCommands 'x64_x86' @(
    "cl $cxxflags /DBUILD_WOW6432 /Fe`"$TestDir\keyboard_layout_tables_test_wow64.exe`" `"$tests\keyboard_layout_tables_test.cpp`""
  )
}

Remove-Item $work -Recurse -Force
