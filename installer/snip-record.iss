; Snip Record ("OBS for Dummies") installer.
; Installs the Snip Record plugin plus the patched obs64.exe (tray click
; stops recording) into an existing OBS Studio 33.0.1 64-bit install.
; The stock obs64.exe is backed up to obs64.exe.snip-backup and restored
; on uninstall.
;
; Build:  ISCC.exe snip-record.iss      (payload paths are relative)

#define AppName "Snip Record for OBS"
#define AppVersion "1.2.0"
#define Rundir "..\build_vs2026\rundir\RelWithDebInfo"

[Setup]
AppId={{7E3A1C24-9B5D-4E6A-8F2B-51D0A4C8E901}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Snip Record
DefaultDirName={code:GetObsInstallDir}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..
OutputBaseFilename=SnipRecorder-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}

[Files]
; Patched frontend. uninsneveruninstall: uninstall restores the backup
; instead of deleting the exe (see code below).
Source: "{#Rundir}\bin\64bit\obs64.exe"; DestDir: "{app}\bin\64bit"; \
  Flags: ignoreversion uninsneveruninstall
Source: "{#Rundir}\core\snip-record\snip-record.dll"; \
  DestDir: "{commonappdata}\obs-studio\plugins\snip-record"; Flags: ignoreversion
Source: "{#Rundir}\core\snip-record\data\locale\en-US.ini"; \
  DestDir: "{commonappdata}\obs-studio\plugins\snip-record\data\locale"; Flags: ignoreversion

[Run]
Filename: "{app}\bin\64bit\obs64.exe"; WorkingDir: "{app}\bin\64bit"; \
  Description: "Launch OBS (opens in the simple OBS for Dummies window)"; \
  Flags: postinstall nowait skipifsilent

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

function GetObsInstallDir(Param: String): String;
var
  UninstallExe: String;
begin
  // OBS's uninstall key does not set InstallLocation, so derive the
  // install root from the uninstaller path (<root>\uninstall.exe).
  if RegQueryStringValue(HKLM64,
       'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
       'UninstallString', UninstallExe) then
  begin
    UninstallExe := RemoveQuotes(UninstallExe);
    Result := ExtractFileDir(UninstallExe);
    if FileExists(AddBackslash(Result) + 'bin\64bit\obs64.exe') then
      Exit;
  end;
  Result := ExpandConstant('{pf}\obs-studio');
end;

function ObsExePath: String;
begin
  Result := ExpandConstant('{app}\bin\64bit\obs64.exe');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Version: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if not FileExists(ObsExePath) then
    begin
      MsgBox('No 64-bit obs64.exe found in that folder.' + #13#10 +
             'Pick the OBS Studio installation folder (the one containing bin\64bit).',
             mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if GetVersionNumbersString(ObsExePath, Version) then
    begin
      if Copy(Version, 1, 6) <> '33.0.1' then
      begin
        if MsgBox('This installer is built for OBS Studio 33.0.1, but the selected ' +
                  'obs64.exe is version ' + Version + '.' + #13#10#13#10 +
                  'The patched executable may not work with a different OBS version. ' +
                  'Continue anyway?', mbConfirmation, MB_YESNO) = IDNO then
          Result := False;
      end;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Backup, Exe: String;
begin
  Result := '';
  Exe := ObsExePath;
  // Fail early (with a clear message) if OBS is running: the exe is
  // locked for writing while it runs.
  if FileExists(Exe) and ExeIsLocked(Exe) then
  begin
    Result := 'OBS Studio appears to be running. Close OBS completely ' +
              '(including the tray icon) and run this installer again.';
    Exit;
  end;

  // Keep the stock exe once, so uninstall can put it back.
  Backup := Exe + '.snip-backup';
  if FileExists(Exe) and not FileExists(Backup) then
  begin
    if not CopyFile(Exe, Backup, True) then
      Result := 'Could not back up the existing obs64.exe. Installation aborted.';
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Backup, Exe: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Exe := ObsExePath;
    Backup := Exe + '.snip-backup';
    Log('Uninstall restore: exe=' + Exe + ' backupExists=' + IntToStr(Ord(FileExists(Backup))));
    if FileExists(Backup) then
    begin
      // Put the stock executable back (the patched one was installed
      // with uninsneveruninstall, so it is still in place here).
      if CopyFile(Backup, Exe, False) then
      begin
        DeleteFile(Backup);
        Log('Uninstall restore: stock obs64.exe restored from backup.');
      end
      else
        Log('Uninstall restore: CopyFile failed.');
    end;
  end;
end;

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\snip-record"
