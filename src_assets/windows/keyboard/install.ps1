# Registers the "US - Mac" keyboard layout (src/fork/keyboard_layout/): Windows' US layout with the
# Mac's Option characters on AltGr, the Right Alt key LuzLunar sends for the Mac's right Option key.
# Apollo's installer runs it as an administrator, or as SYSTEM for an over-the-air update. It only
# makes the layout available; add it to your keyboards in Settings > Time & language > Language &
# region (FORK.md, "Keyboard layout for LuzLunar"). Running it again updates the files and keeps the
# same layout.
$ErrorActionPreference = 'Stop'

# The installer is 32-bit, so it may start 32-bit PowerShell, which sees SysWOW64 as System32
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
  & "$env:SystemRoot\Sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath
  exit $LASTEXITCODE
}

$file = 'kbdluzus.dll'
$layouts = 'HKLM:\SYSTEM\CurrentControlSet\Control\Keyboard Layouts'

if (-not (Test-Path "$PSScriptRoot\$file")) {
  Write-Host "This build has no $file (CI builds it with Visual Studio), so the US - Mac layout isn't installed"
  exit 0
}

Add-Type -Namespace LuzKeyboard -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string existing, string replacement, int flags);
'@

# Copies a layout DLL into a system folder. Windows keeps a layout it has loaded open, but the file
# can be renamed: the old copy moves aside and goes at the next restart.
function Install-LayoutFile([string]$from, [string]$folder) {
  $to = "$folder\$file"
  if ((Test-Path $to) -and (Get-FileHash $to).Hash -eq (Get-FileHash $from).Hash) {
    return
  }
  try {
    Copy-Item $from $to -Force
  } catch {
    $aside = "$to.$([guid]::NewGuid().ToString('N')).old"
    Move-Item $to $aside
    [LuzKeyboard.Native]::MoveFileEx($aside, $null, 4) | Out-Null  # MOVEFILE_DELAY_UNTIL_REBOOT
    Copy-Item $from $to -Force
    Write-Host "Replaced $to, which Windows had loaded; apps get the new one after you sign in again"
  }
}

Install-LayoutFile "$PSScriptRoot\$file" "$env:SystemRoot\System32"
# 32-bit programs read the WOW64 copy
if ((Test-Path "$PSScriptRoot\wow64\$file") -and (Test-Path "$env:SystemRoot\SysWOW64")) {
  Install-LayoutFile "$PSScriptRoot\wow64\$file" "$env:SystemRoot\SysWOW64"
}

# Its key: the one an earlier install made, or the first free one of the form a###0409 (custom
# layouts start with a; 0409 is English (United States))
$klid = Get-ChildItem $layouts | Where-Object { $_.GetValue('Layout File') -eq $file } |
  Select-Object -First 1 -ExpandProperty PSChildName
if (-not $klid) {
  $klid = 0..0xFFF | ForEach-Object { 'a{0:x3}0409' -f $_ } | Where-Object { -not (Test-Path "$layouts\$_") } |
    Select-Object -First 1
}
$key = "$layouts\$klid"

# Its Layout Id, which no other layout may have: Windows tells layouts of the same language apart by it
$id = $null
if (Test-Path $key) {
  $id = (Get-Item $key).GetValue('Layout Id')
} else {
  New-Item $key | Out-Null
}
if (-not $id) {
  $used = Get-ChildItem $layouts | ForEach-Object { $_.GetValue('Layout Id') } |
    Where-Object { $_ -match '^[0-9a-fA-F]{1,4}$' } | ForEach-Object { [Convert]::ToInt32($_, 16) }
  $id = '{0:x4}' -f (($used | Measure-Object -Maximum).Maximum + 1)
}

Set-ItemProperty $key 'Layout File' $file
Set-ItemProperty $key 'Layout Text' 'US - Mac'
Set-ItemProperty $key 'Layout Display Name' "@%SystemRoot%\system32\$file,-1000"
Set-ItemProperty $key 'Layout Id' $id

Write-Host "US - Mac is keyboard layout $klid (Layout Id $id). Add it in Settings > Time & language > Language & region."
