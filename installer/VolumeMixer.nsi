Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID zlib

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!define AppName "Volume Mixer"
!define AppExe "VolumeMixer.exe"
!define UninstallKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\SelfProject.VolumeMixer"
!define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"
!define AppMutex "Local\SelfProject.VolumeMixer.SingleInstance"

Name "${AppName}"
OutFile "${SetupFile}"
InstallDir "$LOCALAPPDATA\Programs\VolumeMixer"
VIProductVersion "${Version}"
VIAddVersionKey /LANG=1033 "ProductName" "${AppName}"
VIAddVersionKey /LANG=1033 "FileDescription" "${AppName} Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${Version}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${Version}"
VIAddVersionKey /LANG=1033 "LegalCopyright" ""

!define MUI_ICON "..\Assets\VolumeMixer.ico"
!define MUI_UNICON "..\Assets\VolumeMixer.ico"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${AppExe}"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\THIRD_PARTY_NOTICES.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "View third-party credits"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!macro RequireMixerClosed
  System::Call 'kernel32::OpenMutexW(i 0x100000, i 0, w "${AppMutex}") p.r0'
  ${If} $0 P<> 0
    System::Call 'kernel32::CloseHandle(p r0)'
    MessageBox MB_OK|MB_ICONEXCLAMATION "Close Volume Mixer from its taskbar or tray menu and try again." /SD IDOK
    Abort
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${IsNativeAMD64}
  ${OrIfNot} ${AtLeastWin11}
    MessageBox MB_OK|MB_ICONSTOP "Volume Mixer requires Windows 11 x64." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  !insertmacro RequireMixerClosed
FunctionEnd

Section "Volume Mixer"
  !insertmacro RequireMixerClosed
  SetOverwrite on
  !include "${InstallFiles}"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${AppName}.lnk" "$INSTDIR\${AppExe}"
  WriteRegStr HKCU "${UninstallKey}" "DisplayName" "${AppName}"
  WriteRegStr HKCU "${UninstallKey}" "DisplayVersion" "${Version}"
  WriteRegStr HKCU "${UninstallKey}" "DisplayIcon" "$INSTDIR\${AppExe}"
  WriteRegStr HKCU "${UninstallKey}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UninstallKey}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegDWORD HKCU "${UninstallKey}" "EstimatedSize" "${EstimatedSize}"
  WriteRegDWORD HKCU "${UninstallKey}" "NoModify" 1
  WriteRegDWORD HKCU "${UninstallKey}" "NoRepair" 1
SectionEnd

Function un.onInit
  SetRegView 64
  !insertmacro RequireMixerClosed
FunctionEnd

Section "Uninstall"
  !insertmacro RequireMixerClosed
  ReadRegStr $0 HKCU "${RunKey}" "VolumeMixer"
  ${If} $0 == '$\"$INSTDIR\${AppExe}$\"'
    DeleteRegValue HKCU "${RunKey}" "VolumeMixer"
  ${EndIf}
  DeleteRegKey HKCU "${UninstallKey}"
  Delete "$SMPROGRAMS\${AppName}.lnk"
  !include "${UninstallFiles}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
