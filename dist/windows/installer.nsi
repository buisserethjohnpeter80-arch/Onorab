; Onorab Windows installer (NSIS).
;
; Built by GitHub Actions AFTER the "Package Windows ZIP" step, from
; the same staged files (the VST3 bundle + the Hub exe). Run from the
; repo root with:
;   makensis /DSRC_DIR=staging dist/windows/installer.nsi
; (NSIS / makensis is preinstalled on the GitHub windows-2022 runner;
; if it is ever missing, add a `choco install nsis -y` step first.)
; The output, Onorab-Installer.exe, lands in the working directory
; and is uploaded as the onorab-installer artifact.
;
; What it installs (machine-wide, so every Windows user on the PC
; finds the same files — the plugin looks in these exact places):
;   Onorab.vst3       -> C:\Program Files\Common Files\VST3\
;   onorab-hub.exe    -> C:\Program Files\Onorab\
; The installer does NOT start the Hub and does NOT ask for any
; account, secret, or setting: the plugin auto-launches the Hub in
; Drive mode (no console window) the first time it opens, and the
; musician signs in with their own Gmail from FL Studio.
; Uninstall removes the plugin + Hub exe but deliberately keeps the
; Hub cache and the Credential Manager token: reinstalling then
; reconnects automatically instead of forcing another sign-in.
;
; Requires admin (RequestExecutionLevel admin) because both target
; folders live under Program Files. The welcome page tells the user
; to close FL Studio first, so locked plugin files can be replaced.

!ifndef SRC_DIR
  !define SRC_DIR "staging"
!endif

!include "MUI2.nsh"

Name "Onorab"
OutFile "Onorab-Installer.exe"
InstallDir "$PROGRAMFILES64\Onorab"
RequestExecutionLevel admin

!define MUI_WELCOMEPAGE_TEXT "This installs the Onorab VST3 plugin and the Onorab Hub background service.$\r$\n$\r$\nClose FL Studio before installing, so the old plugin files can be replaced.$\r$\n$\r$\nNo account or setup is needed: open FL Studio after installing, and the plugin starts the Hub and guides the Google Drive sign-in itself."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$COMMONFILES64\VST3"
  File /r "${SRC_DIR}\Onorab.vst3"
  SetOutPath "$PROGRAMFILES64\Onorab"
  File "${SRC_DIR}\onorab-hub.exe"
  WriteUninstaller "$PROGRAMFILES64\Onorab\uninstall.exe"
  SetRegView 64
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Onorab" "DisplayName" "Onorab"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Onorab" "UninstallString" "$PROGRAMFILES64\Onorab\uninstall.exe"
SectionEnd

Section "Uninstall"
  RMDir /r "$COMMONFILES64\VST3\Onorab.vst3"
  Delete "$PROGRAMFILES64\Onorab\onorab-hub.exe"
  Delete "$PROGRAMFILES64\Onorab\uninstall.exe"
  RMDir "$PROGRAMFILES64\Onorab"
  SetRegView 64
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Onorab"
SectionEnd
