; quorum studio installer. Per-user: needs no administrator rights, writes nothing outside the user's profile.
; Built by build.sh with makensis (NSIS) in Docker.  makensis -DVERSION=x.y.z installer.nsi

Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!define APP "quorum studio"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\quorum-studio"

Name "${APP}"
OutFile "dist\quorum-studio-setup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\quorum studio"
InstallDirRegKey HKCU "Software\quorum-studio" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "CompanyName" "BioDream LLC"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright BioDream LLC. PolyForm Noncommercial 1.0.0; commercial licences from BioDream LLC."

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "quorum studio ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "quorum studio turns a machine's rulebook, a safety worksheet or a table of past records into a checked program image for a Raspberry Pi Pico 2 or an Arduino Uno R4.$\r$\n$\r$\nNo administrator rights are needed."
!define MUI_FINISHPAGE_RUN "$INSTDIR\quorum-studio.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open quorum studio now"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  File "dist\quorum-studio.exe"
  File "README.txt"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\quorum studio"
  CreateShortcut "$SMPROGRAMS\quorum studio\quorum studio.lnk" "$INSTDIR\quorum-studio.exe"
  CreateShortcut "$SMPROGRAMS\quorum studio\Uninstall quorum studio.lnk" "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\quorum-studio" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "BioDream LLC"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\quorum-studio.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\quorum-studio.exe"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\quorum studio\quorum studio.lnk"
  Delete "$SMPROGRAMS\quorum studio\Uninstall quorum studio.lnk"
  RMDir "$SMPROGRAMS\quorum studio"
  ; The tools the studio unpacks on first run, under %LOCALAPPDATA%\quorum-studio.
  RMDir /r "$LOCALAPPDATA\quorum-studio"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\quorum-studio"
SectionEnd
