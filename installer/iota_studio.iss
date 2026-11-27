; Iota Studio - Inno Setup 6 Installer Script
; Compiles build/installer/IotaStudio-Setup-<AppVersion>.exe

#ifndef AppVersion
  #define AppVersion "1.13.2"
#endif

#ifndef IncludeMcp
  #define IncludeMcp 0
#endif

#if Int(IncludeMcp) != 0
  #define McpEnabled 1
#else
  #define McpEnabled 0
#endif

#ifndef BuildDir
  #if McpEnabled
    #define BuildDir "..\build\release-mcp"
  #else
    #define BuildDir "..\build\release"
  #endif
#endif

#define AppName "Iota Studio"
#define AppPublisher "Iota Studio"
#define AppExeName "iota_studio.exe"
#define McpExeName "iota_mcp.exe"
#define AppRepoUrl "https://github.com/vorlie/iota-studio"
#define FfmpegUrl "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.7z"
#define FfmpegArchiveName "ffmpeg-release-essentials.7z"

[Setup]
AppId={{8F6D3C91-4B27-4E9E-B84A-72F0D5A81C42}
AppName={#AppName}
AppVersion={#AppVersion}
#if McpEnabled
AppVerName={#AppName} {#AppVersion} (MCP)
#else
AppVerName={#AppName} {#AppVersion}
#endif
AppPublisher={#AppPublisher}
AppPublisherURL={#AppRepoUrl}
AppSupportURL={#AppRepoUrl}/issues
AppUpdatesURL={#AppRepoUrl}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern dynamic
SetupIconFile=..\assets\iota_studio.ico
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
ArchiveExtraction=enhanced/nopassword
OutputDir=..\build\installer
#if McpEnabled
OutputBaseFilename=IotaStudio-MCP-Setup-{#AppVersion}
#else
OutputBaseFilename=IotaStudio-Setup-{#AppVersion}
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BuildDir}\apps\studio\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\apps\studio\discord_partner_sdk.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
#if McpEnabled
Source: "{#BuildDir}\apps\mcp\{#McpExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\MCP.md"; DestDir: "{app}"; DestName: "MCP.md"; Flags: ignoreversion
#endif
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{tmp}\{#FfmpegArchiveName}"; DestDir: "{tmp}\ffmpeg_extract"; Flags: external extractarchive recursesubdirs ignoreversion deleteafterinstall; Check: ShouldExtractFfmpeg

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
#if McpEnabled
Name: "{group}\MCP Setup Guide"; Filename: "{app}\MCP.md"
#endif
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\ffmpeg.exe"
Type: files; Name: "{app}\ffprobe.exe"

#if McpEnabled == 0
[InstallDelete]
Type: files; Name: "{app}\{#McpExeName}"
Type: files; Name: "{app}\MCP.md"
#endif

[Code]
var
  FfmpegPage: TInputOptionWizardPage;
  DownloadPage: TDownloadWizardPage;
  HasFfmpegDetected: Boolean;
  HasFfprobeDetected: Boolean;
  FfmpegFoundLocation: String;
  FfmpegCheckedDir: String;
  FfmpegUserInteracted: Boolean;
  FfmpegForceCliOn: Boolean;
  FfmpegForceCliOff: Boolean;
  FfmpegDownloadSucceeded: Boolean;
  FfmpegDownloadAttempted: Boolean;

function WinExpandEnvironmentStrings(lpSrc: String; lpDst: String; nSize: DWORD): DWORD;
  external 'ExpandEnvironmentStringsW@kernel32.dll stdcall';

function ExpandEnvVars(const InputStr: String): String;
var
  Buf: String;
  Needed: DWORD;
  NullPos: Integer;
begin
  Result := InputStr;
  if Pos('%', InputStr) = 0 then
    Exit;

  SetLength(Buf, 4096);
  Needed := WinExpandEnvironmentStrings(InputStr, Buf, Length(Buf));
  if Needed > DWORD(Length(Buf)) then begin
    SetLength(Buf, Needed + 1);
    Needed := WinExpandEnvironmentStrings(InputStr, Buf, Length(Buf));
  end;
  if Needed > 0 then begin
    NullPos := Pos(#0, Buf);
    if NullPos > 0 then
      Result := Copy(Buf, 1, NullPos - 1)
    else
      Result := Copy(Buf, 1, Needed - 1);
  end;
end;

function NormalizeDirPath(const RawDir: String): String;
begin
  Result := Trim(RawDir);
  if (Length(Result) >= 2) and (Result[1] = '"') and (Result[Length(Result)] = '"') then
    Result := Copy(Result, 2, Length(Result) - 2);
  Result := ExpandEnvVars(Trim(Result));
  if (Result <> '') and (Result[Length(Result)] = '\') and (Length(Result) > 3) then
    Delete(Result, Length(Result), 1);
end;

function FindExeInPathString(const PathList, ExeName: String; var FoundDir: String): Boolean;
var
  Remaining, Entry, CleanDir: String;
  SemiPos: Integer;
begin
  Result := False;
  Remaining := PathList;
  while Remaining <> '' do begin
    SemiPos := Pos(';', Remaining);
    if SemiPos > 0 then begin
      Entry := Copy(Remaining, 1, SemiPos - 1);
      Delete(Remaining, 1, SemiPos);
    end else begin
      Entry := Remaining;
      Remaining := '';
    end;

    CleanDir := NormalizeDirPath(Entry);
    if (CleanDir <> '') and FileExists(AddBackslash(CleanDir) + ExeName) then begin
      FoundDir := CleanDir;
      Result := True;
      Exit;
    end;
  end;
end;

function DetectExecutable(const AppDir, ExeName: String; var FoundWhere: String): Boolean;
var
  CleanAppDir, RegPath: String;
begin
  Result := False;
  FoundWhere := '';

  CleanAppDir := NormalizeDirPath(AppDir);
  if (CleanAppDir <> '') and FileExists(AddBackslash(CleanAppDir) + ExeName) then begin
    FoundWhere := CleanAppDir;
    Result := True;
    Exit;
  end;

  if RegQueryStringValue(HKEY_LOCAL_MACHINE,
       'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
       'Path', RegPath) then begin
    if FindExeInPathString(RegPath, ExeName, FoundWhere) then begin
      Result := True;
      Exit;
    end;
  end;

  if RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', RegPath) then begin
    if FindExeInPathString(RegPath, ExeName, FoundWhere) then begin
      Result := True;
      Exit;
    end;
  end;

  if FindExeInPathString(GetEnv('PATH'), ExeName, FoundWhere) then begin
    Result := True;
    Exit;
  end;
end;

function DescribeFfmpegSummary: String;
var
  MissingText: String;
begin
  if HasFfmpegDetected and HasFfprobeDetected then begin
    Result := 'Detected both ffmpeg.exe and ffprobe.exe (' + FfmpegFoundLocation + ').';
  end else begin
    MissingText := '';
    if not HasFfmpegDetected then
      MissingText := 'ffmpeg.exe';
    if not HasFfprobeDetected then begin
      if MissingText <> '' then
        MissingText := MissingText + ' and ';
      MissingText := MissingText + 'ffprobe.exe';
    end;
    Result := 'Not found: ' + MissingText + '.';
  end;
end;

procedure RefreshFfmpegDetection(const TargetAppDir: String; const UpdateCheckboxDefault: Boolean);
var
  WhereFfmpeg, WhereFfprobe: String;
  BothPresent, WantInstall: Boolean;
begin
  HasFfmpegDetected := DetectExecutable(TargetAppDir, 'ffmpeg.exe', WhereFfmpeg);
  HasFfprobeDetected := DetectExecutable(TargetAppDir, 'ffprobe.exe', WhereFfprobe);

  if HasFfmpegDetected then
    FfmpegFoundLocation := WhereFfmpeg
  else if HasFfprobeDetected then
    FfmpegFoundLocation := WhereFfprobe
  else
    FfmpegFoundLocation := '';

  BothPresent := HasFfmpegDetected and HasFfprobeDetected;
  FfmpegCheckedDir := TargetAppDir;

  if BothPresent then begin
    FfmpegPage.SubCaptionLabel.Caption :=
      'Status: ' + DescribeFfmpegSummary + #13#10 +
      'Iota Studio already has access to FFmpeg & FFprobe. Installing a local copy is optional.';
  end else begin
    FfmpegPage.SubCaptionLabel.Caption :=
      'Status: ' + DescribeFfmpegSummary + #13#10 +
      'FFmpeg & FFprobe are recommended for broad audio decoding (MP3, FLAC, AAC, OGG) and MP4 video export. ' +
      'Toggle below to download and install them automatically alongside Iota Studio.';
  end;

  if UpdateCheckboxDefault and (not FfmpegUserInteracted) then begin
    WantInstall := not BothPresent;
    if FfmpegForceCliOn then
      WantInstall := True;
    if FfmpegForceCliOff then
      WantInstall := False;
    FfmpegPage.Values[0] := WantInstall;
  end;
end;

procedure InitializeWizard;
var
  I: Integer;
  Param: String;
begin
  FfmpegUserInteracted := False;
  FfmpegForceCliOn := False;
  FfmpegForceCliOff := False;
  FfmpegDownloadSucceeded := False;
  FfmpegDownloadAttempted := False;

  for I := 1 to ParamCount do begin
    Param := Uppercase(ParamStr(I));
    if Param = '/INSTALLFFMPEG' then
      FfmpegForceCliOn := True
    else if Param = '/NOFFMPEG' then
      FfmpegForceCliOff := True;
  end;

  FfmpegPage := CreateInputOptionPage(
    wpSelectTasks,
    'Audio & Video Dependencies (FFmpeg / FFprobe)',
    'Checking your system for FFmpeg and FFprobe...',
    'Optional third-party download (gyan.dev essentials build):',
    False,
    False
  );
  FfmpegPage.Add('Download and install FFmpeg & FFprobe into the Iota Studio folder (~35 MB)');

  RefreshFfmpegDetection(WizardDirValue, True);

  DownloadPage := CreateDownloadPage(
    SetupMessage(msgWizardPreparing),
    SetupMessage(msgPreparingDesc),
    nil
  );
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = FfmpegPage.ID then begin
    if FfmpegCheckedDir <> WizardDirValue then
      RefreshFfmpegDetection(WizardDirValue, True)
    else
      RefreshFfmpegDetection(WizardDirValue, False);
  end;
end;

function ShouldExtractFfmpeg: Boolean;
begin
  Result := FfmpegDownloadSucceeded and
            FileExists(ExpandConstant('{tmp}\{#FfmpegArchiveName}'));
end;

function RunFfmpegDownload(const ShowUi: Boolean): Boolean;
var
  ErrorMsg: String;
begin
  Result := True;
  FfmpegDownloadAttempted := True;
  FfmpegDownloadSucceeded := False;

  DownloadPage.Clear;
  DownloadPage.Add('{#FfmpegUrl}', '{#FfmpegArchiveName}', '');

  if ShowUi then
    DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      FfmpegDownloadSucceeded := True;
      Result := True;
    except
      if DownloadPage.AbortedByUser then begin
        Log('FFmpeg download cancelled by user.');
        Result := False;
      end else begin
        ErrorMsg := Format('%s: %s', [DownloadPage.LastBaseNameOrUrl, GetExceptionMessage]);
        Log('FFmpeg download failed: ' + ErrorMsg);
        if ShowUi then begin
          if SuppressibleMsgBox(
               'Failed to download FFmpeg & FFprobe:' + #13#10 + ErrorMsg + #13#10#13#10 +
               'Do you want to continue installing Iota Studio without FFmpeg?',
               mbError, MB_YESNO, IDYES) = IDYES then begin
            FfmpegDownloadSucceeded := False;
            Result := True;
          end else begin
            Result := False;
          end;
        end else begin
          FfmpegDownloadSucceeded := False;
          Result := True;
        end;
      end;
    end;
  finally
    if ShowUi then
      DownloadPage.Hide;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = FfmpegPage.ID then begin
    FfmpegUserInteracted := True;
  end else if CurPageID = wpReady then begin
    if FfmpegPage.Values[0] then
      Result := RunFfmpegDownload(True);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if WizardSilent and FfmpegPage.Values[0] and (not FfmpegDownloadAttempted) then begin
    RefreshFfmpegDetection(WizardDirValue, True);
    if FfmpegPage.Values[0] then
      RunFfmpegDownload(False);
  end;
end;

function FindAndCopyFileRecursive(const RootDir, TargetFileName, DestPath: String): Boolean;
var
  FindRec: TFindRec;
  SearchDir, ChildPath: String;
begin
  Result := False;
  SearchDir := RemoveBackslash(RootDir);
  if FindFirst(SearchDir + '\*', FindRec) then begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then begin
          ChildPath := SearchDir + '\' + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then begin
            if FindAndCopyFileRecursive(ChildPath, TargetFileName, DestPath) then begin
              Result := True;
              Exit;
            end;
          end else if CompareText(FindRec.Name, TargetFileName) = 0 then begin
            Result := CopyFile(ChildPath, DestPath, False);
            Exit;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ExtractRoot, AppDir: String;
  CopiedFfmpeg, CopiedFfprobe: Boolean;
begin
  if (CurStep = ssPostInstall) and ShouldExtractFfmpeg then begin
    ExtractRoot := ExpandConstant('{tmp}\ffmpeg_extract');
    AppDir := ExpandConstant('{app}');
    CopiedFfmpeg := FindAndCopyFileRecursive(ExtractRoot, 'ffmpeg.exe', AddBackslash(AppDir) + 'ffmpeg.exe');
    CopiedFfprobe := FindAndCopyFileRecursive(ExtractRoot, 'ffprobe.exe', AddBackslash(AppDir) + 'ffprobe.exe');
    DelTree(ExtractRoot, True, True, True);

    if (not CopiedFfmpeg) or (not CopiedFfprobe) then begin
      SuppressibleMsgBox(
        'FFmpeg archive was downloaded, but ffmpeg.exe or ffprobe.exe could not be copied to ' + AppDir + '.',
        mbError, MB_OK, IDOK);
    end;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  DepMemo: String;
begin
  Result := '';
  if MemoUserInfoInfo <> '' then
    Result := Result + MemoUserInfoInfo + NewLine + NewLine;
  if MemoDirInfo <> '' then
    Result := Result + MemoDirInfo + NewLine + NewLine;
  if MemoTypeInfo <> '' then
    Result := Result + MemoTypeInfo + NewLine + NewLine;
  if MemoComponentsInfo <> '' then
    Result := Result + MemoComponentsInfo + NewLine + NewLine;
  if MemoGroupInfo <> '' then
    Result := Result + MemoGroupInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo + NewLine + NewLine;

  DepMemo := 'Audio & Video Dependencies:' + NewLine +
             Space + DescribeFfmpegSummary + NewLine;
  if FfmpegPage.Values[0] then
    DepMemo := DepMemo + Space + 'Action: Download and install FFmpeg & FFprobe into destination folder'
  else
    DepMemo := DepMemo + Space + 'Action: Skip FFmpeg & FFprobe installation';

#if McpEnabled
  DepMemo := DepMemo + NewLine + NewLine +
             'MCP Support (Enabled):' + NewLine +
             Space + 'Installs MCP-enabled ' + '{#AppExeName}' + ', ' + '{#McpExeName}' + ', and MCP.md' + NewLine +
             Space + 'MCP Server Path: ' + AddBackslash(WizardDirValue) + '{#McpExeName}';
#endif

  Result := Result + DepMemo;
end;
