; OBS for Dummies - full bundle installer.
; Ships the complete OBS build from the repo rundir (patched obs64.exe,
; all modules, Snip Record plugin) as a standalone app. No existing OBS
; installation is required or touched. The app runs with --portable, so
; its config lives in <install>\config and uninstalling removes
; everything.
;
; Build:  ISCC.exe obs-for-dummies.iss   (payload paths are relative)

#define AppName "OBS for Dummies"
#define AppVersion "1.2.0"
#define Rundir "..\build_vs2026\rundir\RelWithDebInfo"
; MSVC runtime, deployed app-local. Path is specific to the build
; machine's Visual Studio 2022 install; adjust if ISCC complains.
#define CrtDir "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT"

[Setup]
AppId={{B42F7D91-3C6E-4A58-9D17-2E8B5F0A6C33}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Snip Record
AppComments=Snip-style screen recording, powered by OBS Studio (GPLv2)
; Per-user install: --portable keeps config in <install>\config, which
; must be user-writable. Under Program Files it is not (OBS aborts with
; "Failed to create required user directories"), so install to
; %LocalAppData%\Programs instead and require no elevation.
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
LicenseFile=..\frontend\data\license\gplv2.txt
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..
OutputBaseFilename=OBS-for-Dummies-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#Rundir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; \
  Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#CrtDir}\*.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\bin\64bit\obs64.exe"; \
  Parameters: "--portable"; WorkingDir: "{app}\bin\64bit"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\bin\64bit\obs64.exe"; \
  Parameters: "--portable"; WorkingDir: "{app}\bin\64bit"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\64bit\obs64.exe"; Parameters: "--portable"; \
  WorkingDir: "{app}\bin\64bit"; Description: "Launch OBS for Dummies"; \
  Flags: postinstall nowait skipifsilent

[UninstallDelete]
; Portable-mode config and logs created at runtime.
Type: filesandordirs; Name: "{app}\config"

[Code]
function ExeIsLocked(Path: String): Boolean;
var
  Stream: TFileStream;
begin
  Result := False;
  try
    Stream := TFileStream.Create(Path, fmOpenReadWrite or fmShareExclusive);
    Stream.Free;
  except
    Result := True;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Exe: String;
begin
  Result := '';
  Exe := ExpandConstant('{app}\bin\64bit\obs64.exe');
  // Reinstall/upgrade over a running instance cannot replace the exe.
  if FileExists(Exe) and ExeIsLocked(Exe) then
    Result := 'OBS for Dummies is currently running. Close it ' +
              '(including the tray icon) and run this installer again.';
end;
