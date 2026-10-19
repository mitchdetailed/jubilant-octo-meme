; SPDX-FileCopyrightText: 2026 the Esses contributors
; SPDX-License-Identifier: MIT
;
; The Windows installer of Esses (08 section 7.1 step 3; 07 section 7.1 "Packaging"; AC-M4-04, AC-M4-09; D-212).
; Compiled by tools/packaging/package.py (Inno Setup 6.3 or later):
;   ISCC /DAppVersion=<version> /DFileVersion=<a.b.c.d> /DStageDir=<staged tree> /DRepoRoot=<repository>
;        /DOutputDir=<dist> [/DVcRedist=<vc_redist.x64.exe> /DVcRedistVersion=<14.xx.yyyyy>] [/DCrtDir=<CRT folder>]
;        [/DVcRequired=<a.b.c> /DOutputSuffix=<-suffix>] esses.iss
; AppVersion is the version stamped into the build (cmake/version.cmake); a release build is configured with
; ESSES_RELEASE_TAG, which CMake accepts only when it is "v" + that version, so the installer's version equals the
; tag. Installs the staged tree (esses, esses-cli, esses-render, Qt deployed by windeployqt, the docking library,
; data, NOTICES.md, THIRD_PARTY_LICENSES.md, LICENSE, licenses\) into Program Files (or, with "install for me
; only", the user's programs folder).
; The C++ runtime (08 section 7.1 step 2): binaries of the VS 2022 17.10 (14.40) or later toolsets crash in
; std::mutex with an older msvcp140.dll, and an old redistributable counts as installed. [Code] reads the installed
; runtime's version (HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64 Major, Minor, Bld) and compares it
; with VcRequired (default: the version of the bundled redistributable, at least 14.40.0):
;   * administrative install, runtime absent or older: the bundled vc_redist.x64.exe runs (/install /quiet
;     /norestart) before anything is copied; when it fails, or the runtime is still older afterwards, Setup refuses
;     ("cannot proceed", exit code 7 when silent) and installs nothing;
;   * per-user install, runtime absent or older: the runtime's DLLs are installed beside esses.exe (app-local
;     deployment, as in the portable zip; the Visual Studio redistribution terms allow it).
; VcRequired is also how CI exercises both paths (a test build that requires a runtime no machine has).
; Tasks: assocld associates .ld log files (ProgId Esses.LogFile, "esses.exe" -l "%1"; 06 section 19 item 5); it is
; preselected in the wizard unless another program owns .ld (an existing installation of the reference product
; keeps its association unless the user opts in); a silent install associates only with /MERGETASKS="assocld".
; An upgrade does not take the earlier install's tasks again (UsePreviousTasks=no), so a silent upgrade never takes
; .ld back from a program that took it meanwhile; the uninstaller removes .ld's default only while it is
; Esses.LogFile (D-217). .ldx is not associated; .csv only offers Esses in "Open with" (never the default).
; Upgrades: [InstallDelete] first removes what Setup puts into {app} and an earlier package may have had that this
; one has not (app-local runtime DLLs of an earlier per-user install on an old runtime, Qt modules and plugin
; folders), so the files of the version installed are the ones it ships (D-217).

#ifndef AppVersion
  #error Define AppVersion (/DAppVersion=<version>)
#endif
#ifndef StageDir
  #error Define StageDir (/DStageDir=<the staged tree>)
#endif
#ifndef RepoRoot
  #error Define RepoRoot (/DRepoRoot=<the repository>)
#endif
#ifndef FileVersion
  #define FileVersion "0.0.0.0"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef OutputSuffix
  #define OutputSuffix ""
#endif
#ifndef VcRequired
  #ifdef VcRedistVersion
    #define VcRequired VcRedistVersion
  #else
    #define VcRequired "14.40.0"
  #endif
#endif

[Setup]
; Fixed for every version (08 section 7.1): an upgrade finds the installed version by it. Never change it.
AppId={{66CC7557-E26B-4E57-9DB2-CA0B18ADA06E}
AppName=Esses
AppVersion={#AppVersion}
AppVerName=Esses {#AppVersion}
AppPublisher=the Esses contributors
AppPublisherURL=https://github.com/mitchdetailed/jubilant-octo-meme
AppSupportURL=https://github.com/mitchdetailed/jubilant-octo-meme/issues
AppUpdatesURL=https://github.com/mitchdetailed/jubilant-octo-meme/releases
VersionInfoVersion={#FileVersion}
VersionInfoProductVersion={#FileVersion}
VersionInfoProductTextVersion={#AppVersion}
VersionInfoTextVersion={#AppVersion}
VersionInfoDescription=Esses setup
VersionInfoCompany=the Esses contributors
VersionInfoCopyright=Copyright (C) 2026 the Esses contributors
DefaultDirName={autopf}\Esses
DefaultGroupName=Esses
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\esses.exe
UninstallDisplayName=Esses {#AppVersion}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Qt 6 needs Windows 10 1809 or later.
MinVersion=10.0.17763
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
; The project's licence (08 section 9 item 6); Qt's notice is in NOTICES.md and the About dialog.
LicenseFile={#RepoRoot}\LICENSE
SetupIconFile={#RepoRoot}\tools\packaging\icons\esses.ico
OutputDir={#OutputDir}
OutputBaseFilename=esses-{#AppVersion}-win64-setup{#OutputSuffix}
WizardStyle=modern
ChangesAssociations=yes
UsePreviousTasks=no
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "assocld"; Description: "Associate .ld log files with Esses"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; Before [Files] (D-217): what this or an earlier package put into {app}, so that an upgrade keeps nothing the new
; package does not ship. The runtime DLLs come back only where NeedAppLocalRuntime says so.
Type: files; Name: "{app}\msvcp140*.dll"
Type: files; Name: "{app}\vcruntime140*.dll"
Type: files; Name: "{app}\concrt140.dll"
Type: files; Name: "{app}\vccorlib140.dll"
Type: files; Name: "{app}\Qt6*.dll"
Type: files; Name: "{app}\qtadvanceddocking*.dll"
Type: files; Name: "{app}\d3dcompiler_*.dll"
Type: files; Name: "{app}\opengl32sw.dll"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\platforminputcontexts"
Type: filesandordirs; Name: "{app}\translations"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
#ifdef VcRedist
Source: "{#VcRedist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: dontcopy
#endif
#ifdef CrtDir
Source: "{#CrtDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion; Check: NeedAppLocalRuntime
#endif

[Icons]
Name: "{autoprograms}\Esses"; Filename: "{app}\esses.exe"
Name: "{autodesktop}\Esses"; Filename: "{app}\esses.exe"; Tasks: desktopicon

[Registry]
; The log-file type (06 section 19 item 5): offered in "Open with" always, the default with the task assocld.
Root: HKA; Subkey: "Software\Classes\Esses.LogFile"; ValueType: string; ValueName: ""; ValueData: "Esses log file"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Esses.LogFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\esses.exe"",0"
Root: HKA; Subkey: "Software\Classes\Esses.LogFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\esses.exe"" -l ""%1"""
Root: HKA; Subkey: "Software\Classes\.ld\OpenWithProgids"; ValueType: string; ValueName: "Esses.LogFile"; ValueData: ""; Flags: uninsdeletevalue
; No uninsdeletevalue: the uninstaller removes .ld's default only while it is still Esses.LogFile (CurUninstallStepChanged).
Root: HKA; Subkey: "Software\Classes\.ld"; ValueType: string; ValueName: ""; ValueData: "Esses.LogFile"; Tasks: assocld
; CSV files: a secondary handler only (in "Open with"), never the default.
Root: HKA; Subkey: "Software\Classes\Esses.CsvFile"; ValueType: string; ValueName: ""; ValueData: "CSV file (Esses)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Esses.CsvFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\esses.exe"",0"
Root: HKA; Subkey: "Software\Classes\Esses.CsvFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\esses.exe"" -l ""%1"""
Root: HKA; Subkey: "Software\Classes\.csv\OpenWithProgids"; ValueType: string; ValueName: "Esses.CsvFile"; ValueData: ""; Flags: uninsdeletevalue
; The program in "Open with" lists, and `esses` from Run (Win+R) or `start esses`.
Root: HKA; Subkey: "Software\Classes\Applications\esses.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Esses"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\esses.exe\SupportedTypes"; ValueType: string; ValueName: ".ld"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\esses.exe\SupportedTypes"; ValueType: string; ValueName: ".csv"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\esses.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\esses.exe"" -l ""%1"""
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\esses.exe"; ValueType: string; ValueName: ""; ValueData: "{app}\esses.exe"; Flags: uninsdeletekey

[Run]
Filename: "{app}\esses.exe"; Description: "{cm:LaunchProgram,Esses}"; Flags: nowait postinstall skipifsilent

[Code]
const
  RuntimeKey = 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  RequiredRuntime = '{#VcRequired}';
  RuntimePage = 'https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist';

var
  RuntimeTooOld: Boolean;
  TasksPrepared: Boolean;

{ The Index-th (from 0) number of a dotted version, 0 when it has fewer parts. }
function VersionPart(const Version: String; Index: Integer): Integer;
var
  Rest, Item: String;
  P, I: Integer;
begin
  Result := 0;
  Rest := Version;
  for I := 0 to Index do
  begin
    P := Pos('.', Rest);
    if P = 0 then
    begin
      Item := Rest;
      Rest := '';
    end
    else
    begin
      Item := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end;
    if I = Index then
      Result := StrToIntDef(Item, 0);
  end;
end;

{ True when the x64 runtime is absent or older than RequiredRuntime (major, minor, build). }
function InstalledRuntimeTooOld(): Boolean;
var
  Major, Minor, Bld: Cardinal;
  Have, Need, I: Integer;
begin
  Result := True;
  if not RegQueryDWordValue(HKLM64, RuntimeKey, 'Major', Major) then
    exit;
  if not RegQueryDWordValue(HKLM64, RuntimeKey, 'Minor', Minor) then
    exit;
  if not RegQueryDWordValue(HKLM64, RuntimeKey, 'Bld', Bld) then
    exit;
  Result := False;
  for I := 0 to 2 do
  begin
    case I of
      0: Have := Integer(Major);
      1: Have := Integer(Minor);
    else
      Have := Integer(Bld);
    end;
    Need := VersionPart(RequiredRuntime, I);
    if Have < Need then
    begin
      Result := True;
      exit;
    end;
    if Have > Need then
      exit;
  end;
end;

{ [Files] Check: the runtime's DLLs beside esses.exe for a per-user install on a machine with an older runtime. }
function NeedAppLocalRuntime(): Boolean;
begin
  Result := RuntimeTooOld and not IsAdminInstallMode();
end;

function InitializeSetup(): Boolean;
begin
  RuntimeTooOld := InstalledRuntimeTooOld();
  Log(Format('C++ runtime: required %s, installed one too old or absent: %d', [RequiredRuntime, Ord(RuntimeTooOld)]));
  Result := True;
end;

{ 08 section 7.1 step 3: an administrative install installs the bundled runtime when needed and refuses when that
  does not give a recent enough runtime; nothing has been copied yet. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  if (not RuntimeTooOld) or (not IsAdminInstallMode()) then
    exit;
#ifdef VcRedist
  ExtractTemporaryFile('vc_redist.x64.exe');
  if not Exec(ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /quiet /norestart', '', SW_HIDE,
              ewWaitUntilTerminated, Code) then
  begin
    Result := 'The Microsoft Visual C++ Redistributable (x64) could not be started: ' + SysErrorMessage(Code);
    exit;
  end;
  Log(Format('vc_redist.x64.exe exit code %d', [Code]));
  if Code = 3010 then
    NeedsRestart := True
  else if (Code <> 0) and (Code <> 1638) then
  begin
    Result := Format('The Microsoft Visual C++ Redistributable (x64) failed to install (exit code %d). ' +
                     'Install it from %s and run this setup again.', [Code, RuntimePage]);
    exit;
  end;
  if InstalledRuntimeTooOld() then
    Result := 'Esses needs the Microsoft Visual C++ Redistributable (x64) ' + RequiredRuntime +
              ' or newer, and this machine still has an older one. Install the latest from ' + RuntimePage +
              ' and run this setup again.';
#else
  Result := 'Esses needs the Microsoft Visual C++ Redistributable (x64) ' + RequiredRuntime +
            ' or newer. Install it from ' + RuntimePage + ' and run this setup again.';
#endif
end;

{ Another program owns .ld (for example the reference product's own installation). }
function LdOwnedElsewhere(): Boolean;
var
  ProgId: String;
begin
  Result := RegQueryStringValue(HKCR, '.ld', '', ProgId) and (ProgId <> '') and (ProgId <> 'Esses.LogFile');
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpSelectTasks) and (not TasksPrepared) then
  begin
    TasksPrepared := True;
    if not LdOwnedElsewhere() then
      WizardSelectTasks('assocld');
  end;
end;

{ Uninstall (D-217): .ld's default handler is removed only while it is Esses's own; another program that took .ld
  after Esses was installed keeps it. HKA is HKLM for an administrative install, HKCU for a per-user one. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Root: Integer;
  ProgId: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  if IsAdminInstallMode() then
    Root := HKLM64
  else
    Root := HKCU;
  if RegQueryStringValue(Root, 'Software\Classes\.ld', '', ProgId) and (ProgId = 'Esses.LogFile') then
    RegDeleteValue(Root, 'Software\Classes\.ld', '');
end;
