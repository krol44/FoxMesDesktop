#define MyAppName "FoxMes Desktop"
#define MyAppShortName "FoxMes"
#define MyAppPublisher "Foxtail"
#define MyAppURL "https://fxl.ru"
#define MyAppVersion "1.4.4"
#define MyAppExeName "FoxMes.exe"
#define MyAppId "F65B4EBE-8E1B-58C8-AED1-B3E8E207EA5C"

#ifndef ReleasePath
  #error ReleasePath must point to the Release build directory.
#endif

#ifndef OutputPath
  #error OutputPath must point to the artifact directory.
#endif

[Setup]
AppId={{{#MyAppId}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL=https://github.com/krol44/FoxMesDesktop/issues
AppUpdatesURL=https://github.com/krol44/FoxMesDesktop/releases/latest
DefaultDirName={localappdata}\FoxMes
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#OutputPath}
OutputBaseFilename=FoxMes-1.4.4-windows-x64-setup
SetupIconFile={#SourcePath}\..\..\Resources\art\icon256.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}.0
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#ReleasePath}\FoxMes.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppShortName}}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\foxmes"; ValueType: string; ValueName: ""; ValueData: "URL:FoxMes Link"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\foxmes"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\foxmes\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKCU; Subkey: "Software\Classes\foxmes\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" -- ""%1"""

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppShortName}}"; Flags: nowait postinstall skipifsilent

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
    Exec(ExpandConstant('{app}\{#MyAppExeName}'), '-cleanup', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;
