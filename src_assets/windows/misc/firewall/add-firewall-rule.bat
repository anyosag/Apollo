@echo off

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set RULE_NAME=Apollo
set PROGRAM_BIN="%ROOT_DIR%\sunshine.exe"

rem Add the rule, for private and domain networks only: on a network Windows calls public (a cafe's,
rem a hotel's), nobody else should reach the Web UI, pairing or streams. If streaming stops working at
rem home, set your home network to Private in Windows' network settings.
rem Rules from earlier versions allowed every profile; replace them.
netsh advfirewall firewall delete rule name=%RULE_NAME% >nul 2>&1
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=tcp program=%PROGRAM_BIN% enable=yes profile=private,domain
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=udp program=%PROGRAM_BIN% enable=yes profile=private,domain
