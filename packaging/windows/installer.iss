; Inno Setup 6 script for QBucket (x64). Built by build-release.ps1:
;   ISCC /DAppVersion=x.y.z /DStagedRoot=<dir> /DAppIconFile=<path-to-ico> /O<out-dir> installer.iss
; StagedRoot is the windeployqt folder (qbucket.exe, Qt's DLLs and plugins).
;
; Per-user install by default (no admin rights); the first page offers "for
; all users" instead. Uninstalling removes the program only: settings,
; accounts, journal and transfer state (%LOCALAPPDATA%\qbucket) and the
; secrets in the Windows Credential Manager stay - they belong to the
; user's accounts, and a reinstall picks them up again.
; ASCII only: without a BOM, ISCC reads the file in the ANSI code page.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StagedRoot
  #error "StagedRoot must be provided via /D"
#endif
#ifndef AppIconFile
  #error "AppIconFile must be provided via /D"
#endif

#define AppName         "QBucket"
#define AppPublisher    "Rene Hadler"
#define AppPublisherURL "https://github.com/theinvisible/qbucket"
#define AppExeName      "qbucket.exe"
; Stable AppId - never change it: upgrades replace the previous install
; instead of adding a second entry under Apps & Features.
#define AppId           "{{9B11BC02-82C9-4227-9B22-EE7128FB2A0F}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppPublisherURL}
AppSupportURL={#AppPublisherURL}/issues
AppUpdatesURL={#AppPublisherURL}/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputBaseFilename=qbucket-{#AppVersion}-windows-x64-setup
OutputDir=.
SetupIconFile={#AppIconFile}
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
; Relative to this file (packaging/windows/).
LicenseFile=..\..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0.17763
; A running QBucket is asked to close (its files are in use).
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german";  MessagesFile: "compiler:Languages\German.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; The previous version's Qt plugins: a plugin dropped by a newer Qt must not
; linger and be loaded by the new one.
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\translations"

[Files]
Source: "{#StagedRoot}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}";                       Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";                 Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
