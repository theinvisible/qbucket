<#
.SYNOPSIS
    Builds the Windows release of QBucket: MSVC Release with warnings as
    errors, the test suite, the windeployqt folder, a ZIP and the Inno Setup
    installer.

.DESCRIPTION
    What the Windows CI job did, for a machine without CI. Run it from any
    directory, in Windows PowerShell 5.1 or PowerShell 7:

        powershell -ExecutionPolicy Bypass -File packaging\windows\build-release.ps1

    Needs Visual Studio 2022 (or its Build Tools) with the C++ workload, Qt
    (msvc2022_64, 6.5 or newer), CMake and Ninja (both come with Visual
    Studio), and Inno Setup 6 for the installer. The MSVC environment is set
    up through vswhere when cl.exe is not on PATH already.

    Results in OutDir:
        qbucket-<version>-windows-x64.zip
        qbucket-<version>-windows-x64-setup.exe
    plus their SHA-256 sums. The version comes from CMakeLists.txt.

.PARAMETER QtDir
    The Qt kit, e.g. H:\Qt\6.10.2\msvc2022_64. Default: the newest
    msvc2022_64 kit under H:\Qt or C:\Qt.

.PARAMETER SkipTests
    Build without the test suite (faster; not for a release).

.PARAMETER SkipInstaller
    Only the ZIP, no Inno Setup.
#>
[CmdletBinding()]
param(
    [string]$QtDir,
    [string]$BuildDir = 'build-windows-release',
    [string]$OutDir = 'dist',
    [switch]$SkipTests,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $Repo

function Write-Step([string]$Text) { Write-Host "`n==> $Text" -ForegroundColor Cyan }

# Runs a program and stops on a non-zero exit code (native programs don't
# throw on their own).
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

# --- Version -----------------------------------------------------------------
$cmake = Get-Content (Join-Path $Repo 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(qbucket\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'No VERSION in project(qbucket ...) of CMakeLists.txt'
}
$Version = $Matches[1]
Write-Step "QBucket $Version"

# --- Qt ----------------------------------------------------------------------
if (-not $QtDir) {
    $kits = foreach ($root in 'H:\Qt', 'C:\Qt') {
        if (Test-Path $root) {
            Get-ChildItem $root -Directory | Where-Object { $_.Name -match '^6\.\d+\.\d+$' } | ForEach-Object {
                $kit = Join-Path $_.FullName 'msvc2022_64'
                if (Test-Path (Join-Path $kit 'lib\cmake\Qt6')) {
                    [pscustomobject]@{ Version = [version]$_.Name; Path = $kit }
                }
            }
        }
    }
    $best = $kits | Where-Object { $_.Version -ge [version]'6.5.0' } | Sort-Object Version -Descending | Select-Object -First 1
    if (-not $best) { throw 'No Qt 6.5+ msvc2022_64 kit under H:\Qt or C:\Qt; pass -QtDir' }
    $QtDir = $best.Path
}
if (-not (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6'))) { throw "Not a Qt kit: $QtDir" }
Write-Host "Qt: $QtDir"

# --- MSVC --------------------------------------------------------------------
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found - is Visual Studio 2022 installed?' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio with the C++ x64 tools found' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    Set-Location $Repo
}
foreach ($tool in 'cl.exe', 'cmake.exe', 'ninja.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not on PATH" }
}

# --- Build and test ----------------------------------------------------------
$tests = if ($SkipTests) { 'OFF' } else { 'ON' }
Write-Step "configure ($BuildDir)"
Invoke-Native cmake @('-S', '.', '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                      '-DQBUCKET_WARNINGS_AS_ERRORS=ON', "-DQBUCKET_BUILD_TESTS=$tests",
                      "-DCMAKE_PREFIX_PATH=$QtDir")
Write-Step 'build'
Invoke-Native cmake @('--build', $BuildDir, '-j')
if (-not $SkipTests) {
    Write-Step 'test'
    Invoke-Native ctest @('--test-dir', $BuildDir, '--output-on-failure')
}

# --- Stage (windeployqt already ran as a POST_BUILD step) ----------------------
Write-Step 'stage'
$out = Join-Path $Repo $OutDir
$stage = Join-Path $out "stage\qbucket"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$built = Join-Path $Repo $BuildDir
Copy-Item (Join-Path $built 'qbucket.exe') $stage
Get-ChildItem (Join-Path $built '*.dll') -File | Copy-Item -Destination $stage
foreach ($d in 'platforms', 'imageformats', 'iconengines', 'styles', 'tls', 'generic',
               'networkinformation', 'translations') {
    $p = Join-Path $built $d
    if (Test-Path $p) { Copy-Item $p $stage -Recurse -Force }
}
Copy-Item (Join-Path $Repo 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $Repo 'CHANGELOG.md') $stage

# The staged copy must run on its own: version and the keychain round trip
# (Windows Credential Manager).
Write-Step 'smoke test (staged copy)'
$exe = Join-Path $stage 'qbucket.exe'
$reported = (& $exe --version | Out-String).Trim()
if ($reported -ne "qbucket $Version") { throw "qbucket --version says '$reported', expected 'qbucket $Version'" }
Write-Host $reported
& $exe --self-test | Out-Host
if ($LASTEXITCODE -ne 0) {
    # An SSH session with key authentication has no logon credentials: the
    # Credential Manager refuses every write there (ERROR_NO_SUCH_LOGON_SESSION).
    if ($env:SSH_CONNECTION) {
        Write-Warning 'qbucket --self-test failed in an SSH session - the Credential Manager needs an interactive or password logon; run it once on the desktop.'
    } else {
        throw "qbucket --self-test failed ($LASTEXITCODE)"
    }
}

# --- ZIP ---------------------------------------------------------------------
$zip = Join-Path $out "qbucket-$Version-windows-x64.zip"
Write-Step "zip $zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip
$artifacts = @($zip)

# --- Installer ---------------------------------------------------------------
if (-not $SkipInstaller) {
    $iscc = Get-Command iscc.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
    if (-not $iscc) {
        $iscc = @((Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
                  (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
                  (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')) |
            Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $iscc) { throw 'ISCC.exe (Inno Setup 6) not found; install it or pass -SkipInstaller' }
    Write-Step 'installer'
    Invoke-Native $iscc @("/DAppVersion=$Version", "/DStagedRoot=$stage",
                          "/DAppIconFile=$(Join-Path $Repo 'resources\app.ico')", "/O$out",
                          (Join-Path $Repo 'packaging\windows\installer.iss'))
    $artifacts += Join-Path $out "qbucket-$Version-windows-x64-setup.exe"
}

Write-Step 'done'
foreach ($a in $artifacts) {
    $hash = (Get-FileHash $a -Algorithm SHA256).Hash.ToLower()
    Write-Host "$hash  $(Split-Path $a -Leaf)"
}
