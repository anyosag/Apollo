@echo off
rem Removes the LuzVDA device, its driver and its settings. The certificate LuzVDA is signed with stays
rem trusted; remove it with certmgr.msc ("LuzVDA driver signing" in Trusted Root Certification
rem Authorities and in Trusted Publishers) if nothing else you use is signed with it.
pushd %~dp0
"%~dp0luzvda-ctl.exe" uninstall
reg delete HKLM\SOFTWARE\LuzVDA /f >nul 2>&1
popd
