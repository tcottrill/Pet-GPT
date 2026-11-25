; Inno Setup script for the Windows installer, Pet-GPT-<version>-windows-x64-setup.exe.
; Build it with tools\build-installer.ps1 after tools\build-windows.ps1. It is written to
; build\installer; tools\collect-release.ps1 copies it to dist\release with the other downloads.
;
; Installs for the current user (no administrator rights) into %LOCALAPPDATA%\Programs\Pet-GPT.
; No ROMs and no pet.ini are installed: without a pet.ini beside the exe, Pet-GPT keeps its
; settings, ROMs, programs and screenshots in %APPDATA%\Pet-GPT, and its ROM setup dialog
; offers to download the ROMs there (download-roms.ps1). Uninstalling leaves that folder.

#define Root AddBackslash(SourcePath) + "..\.."
#define AppExe Root + "\dist\pet-gpt.exe"
#if !FileExists(AppExe)
  #error dist\pet-gpt.exe is missing: run tools\build-windows.ps1 first
#endif
#define WxLicence Root + "\build\windows\_deps\wx-src\docs\licence.txt"
#if !FileExists(WxLicence)
  #error build\windows\_deps\wx-src\docs\licence.txt is missing: run tools\build-windows.ps1 first
#endif
#define AppVersion GetStringFileInfo(AppExe, "ProductVersion")

[Setup]
AppId={{B79053E7-6002-446E-A96A-3F9F660D4B28}
AppName=Pet-GPT
AppVersion={#AppVersion}
AppVerName=Pet-GPT {#AppVersion}
AppPublisher=Tim Cottrill
AppPublisherURL=https://github.com/tcottrill/Pet-GPT
AppCopyright=Copyright (C) 2026 Tim Cottrill
AppComments=Commodore PET and CBM emulator
DefaultDirName={autopf}\Pet-GPT
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile={#Root}\petemu\petemu.ico
UninstallDisplayIcon={app}\pet-gpt.exe
UninstallDisplayName=Pet-GPT
VersionInfoVersion={#AppVersion}
OutputDir={#Root}\build\installer
OutputBaseFilename=Pet-GPT-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=force
RestartApplications=no

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\dist\download-roms.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#Root}\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#WxLicence}"; DestDir: "{app}"; DestName: "wxWidgets-licence.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Pet-GPT"; Filename: "{app}\pet-gpt.exe"
Name: "{autodesktop}\Pet-GPT"; Filename: "{app}\pet-gpt.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\pet-gpt.exe"; Description: "Start Pet-GPT"; Flags: nowait postinstall skipifsilent

[Code]
// Inno Setup 6.7 leaves too little room left of the task check boxes above 100% DPI,
// so their left edge is cut off; this margin shows them whole.
procedure InitializeWizard;
begin
  WizardForm.TasksList.Offset := ScaleX(4);
end;
