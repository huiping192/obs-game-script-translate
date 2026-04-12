#define MyAppName "OBS Game Translator"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "obs-game-script-translate"

[Setup]
AppId={{F3A2B1C0-D4E5-4F60-9A8B-7C6D5E4F3210}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; Default to OBS system directory; GetOBSPath reads the registry at install time
DefaultDirName={code:GetOBSPath|{autopf}\obs-studio}
; Show dir page so users with non-standard OBS paths can correct it
DisableDirPage=no
DirExistsWarning=no
; OBS system dirs require admin
PrivilegesRequired=admin
OutputBaseFilename=obs-game-translator-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
SourceDir=..\pkg\obs-game-translator

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
; Plugin DLL into OBS plugin scan directory
Source: "bin\64bit\obs-game-translator.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
; Bundle libcurl explicitly — don't rely on OBS's own copy
Source: "bin\64bit\libcurl.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
; Locale files where OBS resolves module data
Source: "data\locale\*.ini"; DestDir: "{app}\data\obs-plugins\obs-game-translator\locale"; Flags: ignoreversion

[Code]
// Read OBS install path from the registry; fall back to Default if not found.
function GetOBSPath(Default: string): string;
var
  Path: string;
begin
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', 'InstallPath', Path) then
    Result := Path
  else if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', 'InstallPath', Path) then
    Result := Path
  else
    Result := Default;
end;
