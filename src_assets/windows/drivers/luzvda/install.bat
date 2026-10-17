@echo off
rem Installs the LuzVDA virtual display driver in place of SudoVDA. Apollo's installer runs it as an
rem administrator, before it reinstalls and starts the Apollo service.
setlocal
pushd %~dp0
set "CTL=%~dp0luzvda-ctl.exe"

set "CERTUTIL=certutil"
where certutil >nul 2>&1 || set "CERTUTIL=%SystemRoot%\System32\certutil.exe"

rem A running Apollo holds the virtual display driver open, and Windows then puts off removing or
rem replacing a display driver until a restart
echo ================
echo Stopping Apollo while its display driver changes...
net stop ApolloService >nul 2>&1

echo ================
echo Removing SudoVDA, which this Apollo no longer uses...
"%CTL%" remove-legacy

echo ================
echo Removing SudoMaker's certificate (CN=sudovda@su.mk) from the trusted stores...
rem Its SHA-1 thumbprint, from the sudovda.cer earlier Apollo installers trusted
powershell -NoProfile -ExecutionPolicy Bypass -Command "foreach ($s in 'Root', 'TrustedPublisher') { $p = 'Cert:\LocalMachine\' + $s + '\3C918FC73525AD8B1521B6DB26B71F694277CC49'; if (Test-Path $p) { Remove-Item $p; Write-Host ('Removed from ' + $s) } }"

echo ================
echo Trusting the certificate LuzVDA is signed with...
%CERTUTIL% -addstore -f Root "luzvda.cer"
%CERTUTIL% -addstore -f TrustedPublisher "luzvda.cer"

echo ================
echo Installing LuzVDA...
"%CTL%" install LuzVDA.inf
if errorlevel 1 (
  echo Installing LuzVDA failed. It needs Windows 11 24H2 or later.
  popd
  exit /b 1
)

rem The driver reads its settings when it starts, and one already installed keeps running
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-PnpDevice | Where-Object { $_.HardwareID -contains 'Root\LuzVDA' } | ForEach-Object { Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false; Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false }"

echo ================
echo Done!
popd
