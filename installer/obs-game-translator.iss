#define MyAppName "OBS Game Translator"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "obs-game-script-translate"

[Setup]
AppId={{F3A2B1C0-D4E5-4F60-9A8B-7C6D5E4F3210}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; Install to per-user OBS plugin directory — no admin/UAC required
DefaultDirName={userappdata}\obs-studio\plugins\obs-game-translator
DisableDirPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=obs-game-translator-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
; Source files are in pkg\obs-game-translator\ built by the CI Package step
SourceDir=..\pkg\obs-game-translator

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "bin\64bit\obs-game-translator.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
; libcurl.dll bundled explicitly to avoid relying on OBS's copy
Source: "bin\64bit\libcurl.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "data\locale\*.ini"; DestDir: "{app}\data\locale"; Flags: ignoreversion

[Run]
Filename: "{win}\explorer.exe"; Parameters: "{app}"; \
  Description: "Open install folder"; Flags: postinstall skipifsilent unchecked
