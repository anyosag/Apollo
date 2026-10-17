# NSIS Packaging
# see options at: https://cmake.org/cmake/help/latest/cpack_gen/nsis.html

set(CPACK_NSIS_INSTALLED_ICON_NAME "${PROJECT__DIR}\\\\${PROJECT_EXE}")

# Extra commands before the files are installed
# The service runs as SYSTEM from the install directory, and the install commands below run scripts
# from it as an administrator, so nobody else may write to it from the start. Under Program Files that
# comes from the parent; in a folder like C:\Apollo, other users could otherwise write, or have made
# the folder themselves. Administrators take ownership (the creator loses control), every ACL is reset
# to inherited (dropping explicit grants), then the folder gets its own: SYSTEM and Administrators full
# control, Users read and run. SIDs, since group names are localized.
SET(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS}
        CreateDirectory \\\"$INSTDIR\\\"
        nsExec::ExecToLog 'icacls \\\"$INSTDIR\\\" /setowner *S-1-5-32-544 /T /C /Q'
        nsExec::ExecToLog 'icacls \\\"$INSTDIR\\\" /reset /T /C /Q'
        nsExec::ExecToLog 'icacls \\\"$INSTDIR\\\" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX /C /Q'
        ")

# Extra install commands
# Migrates config files from the root into the new config folder
# Keeps config (pairings, Web UI credentials, apps) to SYSTEM and Administrators
# Install service
SET(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\update-path.bat\\\" add'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\drivers\\\\luzvda\\\\install.bat\\\"'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\migrate-config.bat\\\"'
        nsExec::ExecToLog 'icacls \\\"$INSTDIR\\\\config\\\" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F /C /Q'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\add-firewall-rule.bat\\\"'
        nsExec::ExecToLog \
          'powershell.exe -NoProfile -ExecutionPolicy Bypass -File \\\"$INSTDIR\\\\scripts\\\\install-gamepad.ps1\\\"'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\install-service.bat\\\"'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\autostart-service.bat\\\"'
        NoController:
        ")

# Extra uninstall commands
# Uninstall service
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\delete-firewall-rule.bat\\\"'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\uninstall-service.bat\\\"'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\sunshine.exe\\\" --restore-nvprefs-undo'
        MessageBox MB_YESNO|MB_ICONQUESTION \
            'Do you want to remove Virtual Gamepad?' \
            /SD IDNO IDNO NoGamepad
            nsExec::ExecToLog \
              'powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
                \\\"$INSTDIR\\\\scripts\\\\uninstall-gamepad.ps1\\\"'; \
              skipped if no
        NoGamepad:
        MessageBox MB_YESNO|MB_ICONQUESTION \
            'Do you want to remove the LuzVDA virtual display driver?' \
            /SD IDNO IDNO NoLuzVDA
            nsExec::ExecToLog '\\\"$INSTDIR\\\\drivers\\\\luzvda\\\\uninstall.bat\\\"'; skipped if no
        NoLuzVDA:
        MessageBox MB_YESNO|MB_ICONQUESTION \
            'Do you want to remove $INSTDIR (this includes the configuration, cover images, and settings)?' \
            /SD IDNO IDNO NoDelete
            RMDir /r \\\"$INSTDIR\\\"; skipped if no
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\update-path.bat\\\" remove'
        NoDelete:
        ")

# Adding an option for the start menu
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
# This will be shown on the installed apps Windows settings
set(CPACK_NSIS_INSTALLED_ICON_NAME "sunshine.exe")
set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "${CPACK_NSIS_CREATE_ICONS_EXTRA}
        SetOutPath '\$INSTDIR'
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\${CMAKE_PROJECT_NAME}.lnk' \
            '\$INSTDIR\\\\sunshine.exe' '--shortcut'
        ")
set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "${CPACK_NSIS_DELETE_ICONS_EXTRA}
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\${CMAKE_PROJECT_NAME}.lnk'
        ")

# Checking for previous installed versions
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL "ON")

# set(CPACK_NSIS_HELP_LINK "https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html")
# set(CPACK_NSIS_URL_INFO_ABOUT "${CMAKE_PROJECT_HOMEPAGE_URL}")
# set(CPACK_NSIS_CONTACT "${CMAKE_PROJECT_HOMEPAGE_URL}/support")

# set(CPACK_NSIS_MENU_LINKS
#         "https://docs.lizardbyte.dev/projects/sunshine" "Sunshine documentation"
#         "https://app.lizardbyte.dev" "LizardByte Web Site"
#         "https://app.lizardbyte.dev/support" "LizardByte Support")
set(CPACK_NSIS_MANIFEST_DPI_AWARE true)
