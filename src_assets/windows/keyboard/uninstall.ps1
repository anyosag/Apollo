# Removes the "US - Mac" keyboard layout install.ps1 registered: from the keyboards of the user running
# it, from Windows' layouts, and its files. Other users who added it are left with a layout Windows
# no longer has, and get their language's default keyboard. Apollo's uninstaller runs it when asked
# to remove the layout.
$ErrorActionPreference = 'Stop'

# The uninstaller is 32-bit, so it may start 32-bit PowerShell, which sees SysWOW64 as System32
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
  & "$env:SystemRoot\Sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath
  exit $LASTEXITCODE
}

$file = 'kbdluzus.dll'
$layouts = 'HKLM:\SYSTEM\CurrentControlSet\Control\Keyboard Layouts'

Add-Type -Namespace LuzKeyboard -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string existing, string replacement, int flags);
'@

$klids = @(Get-ChildItem $layouts | Where-Object { $_.GetValue('Layout File') -eq $file } |
  ForEach-Object { $_.PSChildName })

# SYSTEM has no keyboards of its own to take it off
if ($klids.Count -and -not [Security.Principal.WindowsIdentity]::GetCurrent().IsSystem) {
  try {
    $list = Get-WinUserLanguageList
    $changed = $false
    foreach ($language in $list) {
      foreach ($tip in @($language.InputMethodTips)) {
        if ($klids | Where-Object { $tip -like "*:$_" }) {
          $language.InputMethodTips.Remove($tip) | Out-Null
          $changed = $true
        }
      }
    }
    if ($changed) {
      Set-WinUserLanguageList $list -Force
      Write-Host 'Took US - Mac off your keyboards'
    }
  } catch {
    Write-Warning "Couldn't take US - Mac off your keyboards: $_"
  }
}

foreach ($klid in $klids) {
  Remove-Item "$layouts\$klid" -Recurse
  Write-Host "Removed keyboard layout $klid"
}

# A layout Windows has loaded can't be deleted, only renamed; it goes at the next restart
foreach ($folder in "$env:SystemRoot\System32", "$env:SystemRoot\SysWOW64") {
  $path = "$folder\$file"
  if (-not (Test-Path $path)) {
    continue
  }
  try {
    Remove-Item $path -Force
  } catch {
    $aside = "$path.$([guid]::NewGuid().ToString('N')).old"
    Move-Item $path $aside
    [LuzKeyboard.Native]::MoveFileEx($aside, $null, 4) | Out-Null  # MOVEFILE_DELAY_UNTIL_REBOOT
    Write-Host "$path is in use; it goes at the next restart"
  }
}
