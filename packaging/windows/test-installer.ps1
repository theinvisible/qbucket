<#
.SYNOPSIS
    Checks the QBucket setup on this machine: a per-user install of an older
    version, the update to this one, the uninstall.

.DESCRIPTION
    Called by build-release.ps1 -TestInstaller, or on its own:

        powershell -ExecutionPolicy Bypass -File packaging\windows\test-installer.ps1 `
            -Setup dist\qbucket-1.0.0-windows-x64-setup.exe -Version 1.0.0 `
            -Stage dist\stage\qbucket

    Steps, all per user (no admin rights, no UAC):
      1. an "older" setup (version 0.9.0) is built from the same staged folder;
      2. it is installed silently - files, the uninstall entry and the Start
         menu entry are checked; a stale plugin is planted;
      3. the new setup is installed over it - one uninstall entry with the
         new version, the stale plugin gone, every staged file installed,
         --version and --self-test of the installed exe with nothing but
         Windows on PATH;
      4. it is uninstalled - program folder and uninstall entry gone, the
         user's settings (%LOCALAPPDATA%\qbucket) untouched.

    Refuses to run while QBucket is installed (per user or for all users):
    a real installation is never touched. Whatever this script installed is
    uninstalled again when a step fails.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Setup,
    [Parameter(Mandatory)] [string]$Version,
    [Parameter(Mandatory)] [string]$Stage,
    [string]$Iscc,
    [string]$OldVersion = '0.9.0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Setup = (Resolve-Path $Setup).Path
$Stage = (Resolve-Path $Stage).Path

# installer.iss: the stable AppId; Inno Setup names the uninstall key after it.
$UninstallKeyName = '{9B11BC02-82C9-4227-9B22-EE7128FB2A0F}_is1'
$AppDir = Join-Path $env:LOCALAPPDATA 'Programs\QBucket'
$StartMenuLink = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\QBucket\QBucket.lnk'
$SettingsDir = Join-Path $env:LOCALAPPDATA 'qbucket'

function Write-Step([string]$Text) { Write-Host "`n--> $Text" -ForegroundColor Cyan }
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "installer test: $Message" }
    Write-Host "  ok: $Message"
}

# Every uninstall entry of QBucket: per user and for all users, both views.
function Get-UninstallEntries {
    $roots = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
             'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
             'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    foreach ($root in $roots) {
        $key = Join-Path $root $UninstallKeyName
        if (Test-Path $key) { Get-ItemProperty $key | Add-Member -NotePropertyName RegistryPath -NotePropertyValue $key -PassThru }
    }
}

function Find-Iscc {
    $found = foreach ($root in ${env:ProgramFiles(x86)}, $env:ProgramFiles, (Join-Path $env:LOCALAPPDATA 'Programs')) {
        if ($root -and (Test-Path $root)) {
            Get-ChildItem $root -Directory -Filter 'Inno Setup *' | ForEach-Object {
                $candidate = Join-Path $_.FullName 'ISCC.exe'
                if (Test-Path $candidate) { $candidate }
            }
        }
    }
    $found | Select-Object -First 1
}

function Invoke-Setup([string]$Exe, [string]$Log) {
    $p = Start-Process -FilePath $Exe -Wait -PassThru `
        -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/LOG=`"$Log`""
    if ($p.ExitCode -ne 0) { throw "$(Split-Path $Exe -Leaf) failed with exit code $($p.ExitCode) (log: $Log)" }
}

# The uninstaller hands over to a copy of itself in %TEMP%; wait for its work.
function Invoke-Uninstall([string]$Log) {
    $uninstaller = Join-Path $AppDir 'unins000.exe'
    if (-not (Test-Path $uninstaller)) { throw "no uninstaller at $uninstaller" }
    $p = Start-Process -FilePath $uninstaller -Wait -PassThru `
        -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$Log`""
    if ($p.ExitCode -ne 0) { throw "the uninstaller failed with exit code $($p.ExitCode) (log: $Log)" }
    $deadline = (Get-Date).AddSeconds(60)
    while (((Test-Path $AppDir) -or @(Get-UninstallEntries).Count -gt 0) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
}

# Runs the installed exe with nothing but Windows on PATH and waits for it
# (PowerShell doesn't wait for a GUI executable started with &). Returns the
# output (stdout, then stderr) and the exit code.
function Invoke-Isolated([string]$Exe, [string[]]$Arguments) {
    $saved = $env:PATH
    $outFile = [IO.Path]::GetTempFileName()
    $errFile = [IO.Path]::GetTempFileName()
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        [pscustomobject]@{
            Output   = "$(Get-Content $outFile -Raw)$(Get-Content $errFile -Raw)".Trim()
            ExitCode = $p.ExitCode
        }
    } finally {
        $env:PATH = $saved
        Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
    }
}

# Names, sizes and times of the settings folder - or nothing when absent.
function Get-SettingsSnapshot {
    if (-not (Test-Path $SettingsDir)) { return '<absent>' }
    (Get-ChildItem $SettingsDir -Recurse -File | Sort-Object FullName |
        ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join "`n"
}

# --- Preconditions -----------------------------------------------------------
Write-Step 'preconditions'
$existing = @(Get-UninstallEntries)
if ($existing.Count -gt 0) {
    throw "QBucket is installed ($($existing[0].RegistryPath)); the installer test never touches a real installation - uninstall it first"
}
if (Test-Path $AppDir) { throw "$AppDir exists; the installer test never touches a real installation - remove it first" }
Assert $true 'no QBucket installed'

if (-not $Iscc) { $Iscc = Find-Iscc }
if (-not $Iscc -or -not (Test-Path $Iscc)) { throw 'ISCC.exe (Inno Setup) not found; pass -Iscc' }

$work = Join-Path ([IO.Path]::GetTempPath()) ("qbucket-installer-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$installed = $false
try {
    # --- 1. An older setup ---------------------------------------------------
    Write-Step "build a $OldVersion setup from the same staged folder"
    & $Iscc /Q "/DAppVersion=$OldVersion" "/DStagedRoot=$Stage" `
        "/DAppIconFile=$(Join-Path $Repo 'resources\app.ico')" "/O$work" `
        (Join-Path $Repo 'packaging\windows\installer.iss')
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
    $oldSetup = Join-Path $work "qbucket-$OldVersion-windows-x64-setup.exe"
    Assert (Test-Path $oldSetup) "built $(Split-Path $oldSetup -Leaf)"

    # --- 2. Install it -------------------------------------------------------
    Write-Step "install $OldVersion per user"
    $installed = $true
    Invoke-Setup $oldSetup (Join-Path $work 'install-old.log')
    Assert (Test-Path (Join-Path $AppDir 'qbucket.exe')) "qbucket.exe in $AppDir"
    $entries = @(Get-UninstallEntries)
    Assert ($entries.Count -eq 1) 'one uninstall entry'
    Assert ($entries[0].RegistryPath -like 'HKCU:*') 'the entry is per user (HKCU)'
    Assert ($entries[0].DisplayVersion -eq $OldVersion) "its version is $OldVersion"
    Assert (Test-Path $StartMenuLink) 'Start menu entry'

    $stale = Join-Path $AppDir 'platforms\stale-test.dll'
    Set-Content -Path $stale -Value 'stale' -Encoding Ascii
    Assert (Test-Path $stale) 'planted a stale plugin'

    # --- 3. Update -----------------------------------------------------------
    Write-Step "update to $Version"
    Invoke-Setup $Setup (Join-Path $work 'install-new.log')
    $entries = @(Get-UninstallEntries)
    Assert ($entries.Count -eq 1) 'still one uninstall entry'
    Assert ($entries[0].DisplayVersion -eq $Version) "its version is $Version"
    Assert (-not (Test-Path $stale)) 'the stale plugin is gone'

    $missing = @(Get-ChildItem $Stage -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($Stage.Length + 1)
        if (-not (Test-Path (Join-Path $AppDir $rel))) { $rel }
    })
    Assert ($missing.Count -eq 0) "every staged file installed$(if ($missing) { ': missing ' + ($missing -join ', ') })"

    $exe = Join-Path $AppDir 'qbucket.exe'
    $reported = (Invoke-Isolated $exe @('--version')).Output
    Assert ($reported -eq "qbucket $Version") "installed exe says '$reported'"
    $selfTest = Invoke-Isolated $exe @('--self-test')
    Write-Host $selfTest.Output
    if ($selfTest.ExitCode -ne 0 -and $env:SSH_CONNECTION) {
        Write-Warning 'self-test failed in an SSH session (no logon credentials for the Credential Manager)'
    } else {
        Assert ($selfTest.ExitCode -eq 0) 'installed exe passes --self-test'
    }

    # --- 4. Uninstall --------------------------------------------------------
    Write-Step 'uninstall'
    $settingsBefore = Get-SettingsSnapshot
    Invoke-Uninstall (Join-Path $work 'uninstall.log')
    $installed = $false
    Assert (-not (Test-Path $AppDir)) 'program folder removed'
    Assert (@(Get-UninstallEntries).Count -eq 0) 'uninstall entry removed'
    Assert (-not (Test-Path $StartMenuLink)) 'Start menu entry removed'
    Assert ((Get-SettingsSnapshot) -eq $settingsBefore) "settings ($SettingsDir) untouched"

    Write-Host "`ninstaller test passed" -ForegroundColor Green
} finally {
    if ($installed -and (Test-Path (Join-Path $AppDir 'unins000.exe'))) {
        Write-Warning 'installer test failed - removing what it installed'
        try { Invoke-Uninstall (Join-Path $work 'cleanup.log') } catch { Write-Warning $_ }
    }
    Write-Host "logs: $work"
}
