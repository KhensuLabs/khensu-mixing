; packaging/khensu-installer.iss
;
; Compile with ISCC after a Release build and the two stages below; the
; local smoke test (`pwsh scripts/verify_all.ps1 -Only installer-smoke`)
; compiles it, installs it silently into a temp folder and uninstalls it.
;
; Defines a build can pass (ISCC /D<name>=<value>):
;   MyAppVersion  - defaults to the ProductVersion of the built app, so the
;                   installer can't name a different version from its binary.
;   MyAppId       - the smoke test passes its own id so it never touches a
;                   real installation's uninstall entry.
;   BuildDir      - the CMake build folder (default ..\build, beside this
;                   folder); verify_all passes its own.
;   NoRuntime     - build without the staged Python runtime (smoke test on a
;                   machine that has not run build-python-env.ps1). Never ship
;                   one: the service cannot start.
#define MyAppName "Khensu Mixing Suite"
#define MyAppPublisher "Khensu Labs"
#define MyAppExeName "Khensu Mixing.exe"

; Expected to be built (CMake Release) and staged into these paths before
; compiling this script: the Python runtime by build-python-env.ps1, the
; service by `python scripts/stage_local_services.py --check` (P0-3: from git
; through an allow-list, plus the pinned bundled models - never the dev tree,
; whose test scratch alone runs to gigabytes).
#ifndef BuildDir
  #define BuildDir "..\build"
#endif
; Preprocessor file checks get absolute paths; [Files] resolves relative
; ones against this folder itself.
#define AbsPath(str P) Pos(":", P) > 0 ? P : AddBackslash(SourcePath) + P
#define DesktopAppDir BuildDir + "\desktop-app\KhensuDesktop_artefacts\Release"
#define LocalServicesDir BuildDir + "\packaging\local-services"
#define PythonRuntimeDir BuildDir + "\packaging\python-runtime"
#define PluginDir BuildDir + "\plugin-copilot\KhensuCopilot_artefacts\Release"
#define Vst3BundleDir PluginDir + "\VST3\Khensu Copilot.vst3"
#define ClapFile PluginDir + "\CLAP\Khensu Copilot.clap"
#define AraBundleDir BuildDir + "\plugin-copilot\KhensuCopilotARA_artefacts\Release\VST3\Khensu Copilot ARA.vst3"

#ifndef MyAppVersion
  #define MyAppVersion GetStringFileInfo(AbsPath(DesktopAppDir + "\" + MyAppExeName), "ProductVersion")
#endif
#if MyAppVersion == ""
  #error The app binary has no ProductVersion: build it first, or pass /DMyAppVersion=<x.y.z>.
#endif
#ifndef MyAppId
  ; Inno-escaped exactly as every earlier release wrote it, so upgrades find them.
  ; NEVER change this string, even its odd ending: Inno turns "{{" into "{"
  ; but keeps "}}", so the installed AppId (and the uninstall key,
  ; "...F10}}_is1") ends in two braces. Every release since v1.0.0 must
  ; carry the same id or it installs beside the old version instead of
  ; upgrading it (verify_all installer-upgrade checks this).
  #define MyAppId "{{B3D0F7B4-6C1E-4E9A-9D2F-4B2C1A7E9F10}}"
#endif

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
; The app folder page is always offered (not only on a first install), and the
; plugin folder page below lets the VST3 / CLAP folders be chosen too. A silent
; update (the app's "restart to install") reuses both previous choices.
DisableDirPage=no
UsePreviousAppDir=yes
DefaultGroupName={#MyAppName}
OutputDir=..\.build-artifacts\installer
OutputBaseFilename=khensu-setup-{#MyAppVersion}
; The app icon (scripts/make_app_icon.py) on the installer, and on its entry
; in Windows Settings > Apps.
SetupIconFile=..\resources\icons\khensu.ico
; AGPL-3.0-or-later: the installer shows it, and installs it with the
; third-party notices (which name where the source is).
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\{#MyAppExeName}
AppPublisherURL=https://github.com/KhensuLabs/khensu-mixing
AppSupportURL=https://github.com/KhensuLabs/khensu-mixing/issues
AppUpdatesURL=https://github.com/KhensuLabs/khensu-mixing/releases/latest
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Installs per user by default; an elevated install puts the app under
; Program Files and the plugins in the system-wide plugin folders.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline dialog
; The Python service runs from {app}\local-services\runtime and holds its
; DLLs and .pyd files open, so files could not be replaced while the app
; (or a service it left behind) runs. Restart Manager closes whatever holds
; them before copying (an upgrade over a running app); nothing is restarted.
CloseApplications=force
CloseApplicationsFilter=*.exe,*.dll,*.pyd
RestartApplications=no
; Code signing (CE-ENG-2). Compile with a sign tool defined to sign the
; installer and uninstaller, e.g.:
;   ISCC /DSignToolName=khensu "/Skhensu=signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /f $qcert.pfx$q /p $qpassword$q $f"
; Sign the app binaries first with `packaging/sign-artifacts.ps1 -Require`
; (-Require fails when there is no certificate; without it the script only
; previews). Without SignToolName the installer is built unsigned, as before.
#ifdef SignToolName
SignTool={#SignToolName}
SignedUninstaller=yes
#endif

[Components]
Name: "app"; Description: "Khensu Mixing Suite (Desktop App)"; Types: full compact custom; Flags: fixed
Name: "vst3"; Description: "Khensu Copilot VST3 plugin"; Types: full
#ifexist AbsPath(ClapFile)
Name: "clap"; Description: "Khensu Copilot CLAP plugin"; Types: full
#endif
#ifexist AbsPath(AraBundleDir) + "\Contents\x86_64-win\Khensu Copilot ARA.vst3"
  #define HaveAra
Name: "ara"; Description: "Khensu Copilot ARA plugin (for ARA hosts)"; Types: full
#endif

[Files]
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion; Components: app
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion; Components: app
; The app: an explicit list, so a build folder's leftovers (PDBs, a renamed
; running copy, linker output) can't ship. Binaries link the C runtime
; statically (cmake/CompilerFlags.cmake), so no VC++ redistributable is needed.
Source: "{#DesktopAppDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion; Components: app
Source: "{#DesktopAppDir}\resources\*"; DestDir: "{app}\resources"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: app; Excludes: "desktop.ini"
; The staged service is already clean (stage_local_services.py --check fails
; on test scratch, caches, venvs and dev lock files); the excludes are a second
; line in case the stage is built by hand.
Source: "{#LocalServicesDir}\*"; DestDir: "{app}\local-services"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: app; Excludes: ".pytest_tmp*,.venv*,__pycache__,*.pyc,tests,desktop.ini"
; The separately staged, hash-verified Python runtime. The launcher resolves
; runtime\python.exe before considering a development .venv or system Python.
#ifndef NoRuntime
Source: "{#PythonRuntimeDir}\*"; DestDir: "{app}\local-services\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: app
#endif
; Plugins: the system-wide folders for an elevated install, the per-user
; ones (VST3 3.7 / CLAP) otherwise - hosts scan both.
Source: "{#Vst3BundleDir}\*"; DestDir: "{code:PluginRoot|VST3}\Khensu Copilot.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: vst3; Excludes: "*.pdb,*.ilk,*.exp,*.lib"
#ifexist AbsPath(ClapFile)
Source: "{#ClapFile}"; DestDir: "{code:PluginRoot|CLAP}"; Flags: ignoreversion; Components: clap
#endif
#ifdef HaveAra
Source: "{#AraBundleDir}\*"; DestDir: "{code:PluginRoot|VST3}\Khensu Copilot ARA.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: ara; Excludes: "*.pdb,*.ilk,*.exp,*.lib"
#endif

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
; The app's "Restart now" runs a silent update with /RELAUNCH=1: open the new version.
Filename: "{app}\{#MyAppExeName}"; Flags: nowait; Check: RelaunchAfterSilentUpdate

[Code]
var
  PluginDirPage: TInputDirWizardPage;

// The standard plugin folders: {commoncf64}\<Format> when elevated,
// %LOCALAPPDATA%\Programs\Common\<Format> otherwise (the per-user plugin
// folders of VST3 3.7 and CLAP), which every current DAW scans.
function DefaultPluginRoot(Format: String): String;
begin
  if IsAdminInstallMode then
    Result := ExpandConstant('{commoncf64}\') + Format
  else
    Result := ExpandConstant('{localappdata}\Programs\Common\') + Format;
end;

// v1.0.0: the user may install the plugins elsewhere (a DAW's custom scan
// folder). The page offers the standard folders, or the previous install's.
procedure InitializeWizard;
begin
  PluginDirPage := CreateInputDirPage(wpSelectComponents,
    'Plugin location', 'Where should the Khensu Copilot plugins be installed?',
    'The standard folders below are the ones DAWs scan by default. Choose another folder only if your ' +
    'DAW is set up to scan it.', False, '');
  PluginDirPage.Add('VST3 plugins folder (also holds the ARA plugin):');
  PluginDirPage.Add('CLAP plugins folder:');
  PluginDirPage.Values[0] := GetPreviousData('VST3Dir', DefaultPluginRoot('VST3'));
  PluginDirPage.Values[1] := GetPreviousData('CLAPDir', DefaultPluginRoot('CLAP'));
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'VST3Dir', PluginDirPage.Values[0]);
  SetPreviousData(PreviousDataKey, 'CLAPDir', PluginDirPage.Values[1]);
end;

// No plugin page when only the app is being installed.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = PluginDirPage.ID then begin
    Result := not WizardIsComponentSelected('vst3');
#ifexist AbsPath(ClapFile)
    Result := Result and not WizardIsComponentSelected('clap');
#endif
#ifdef HaveAra
    Result := Result and not WizardIsComponentSelected('ara');
#endif
  end;
end;

// The folder each plugin format goes to: the page's choice.
function PluginRoot(Format: String): String;
begin
  if Format = 'CLAP' then
    Result := PluginDirPage.Values[1]
  else
    Result := PluginDirPage.Values[0];
  if Result = '' then
    Result := DefaultPluginRoot(Format);
end;

// The app's "Restart now": a silent update started with /RELAUNCH=1.
function RelaunchAfterSilentUpdate: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:RELAUNCH|0}') = '1');
end;
