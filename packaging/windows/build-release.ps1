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
    Studio), and Inno Setup 6 or newer for the installer
    (winget install JRSoftware.InnoSetup). The MSVC environment is set up
    through vswhere when cl.exe is not on PATH already.

    The staged folder is deployed by windeployqt into an empty directory and
    gets the MSVC runtime app-local (no vc_redist, no admin rights). A
    dependency check then reads every staged exe / dll with dumpbin: each
    import must be staged or a Windows system DLL - a development machine
    has the MSVC runtime in System32, so the copy would start there and fail
    on a clean Windows.

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

.PARAMETER TestInstaller
    Afterwards run test-installer.ps1 on the new setup: install an older
    version per user, update it, uninstall (needs no admin rights; refuses
    to run while QBucket is installed).
#>
[CmdletBinding()]
param(
    [string]$QtDir,
    [string]$BuildDir = 'build-windows-release',
    [string]$OutDir = 'dist',
    [switch]$SkipTests,
    [switch]$SkipInstaller,
    [switch]$TestInstaller
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

# A path relative to the repository, or an absolute one as it is.
function Get-RepoPath([string]$Path) { [IO.Path]::GetFullPath([IO.Path]::Combine($Repo, $Path)) }

# The DLLs a PE file imports (normal and delay-loaded), as dumpbin lists them.
function Get-Imports([string]$File) {
    $lines = & dumpbin.exe /nologo /dependents $File
    if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents $File failed with exit code $LASTEXITCODE" }
    $dlls = foreach ($line in $lines) { if ($line -match '^\s{4}(\S+\.dll)\s*$') { $Matches[1] } }
    $dlls | Sort-Object -Unique
}

# The MSVC runtime: a dev machine has it in System32, a clean Windows doesn't.
$CrtPattern = '^(msvcp|vcruntime|concrt|vccorlib)140'

# Every import of every staged exe / dll must be next to qbucket.exe or a
# Windows system DLL (the MSVC runtime doesn't count as one). Plugins in
# subfolders resolve their imports from the application directory too.
function Test-StageDependencies([string]$Stage) {
    $staged = @{}
    Get-ChildItem $Stage -File -Filter *.dll | ForEach-Object { $staged[$_.Name.ToLowerInvariant()] = $true }
    $system32 = Join-Path $env:SystemRoot 'System32'
    $problems = @()
    $binaries = Get-ChildItem $Stage -File -Recurse | Where-Object { $_.Extension -in '.exe', '.dll' }
    foreach ($bin in $binaries) {
        foreach ($dll in Get-Imports $bin.FullName) {
            $name = $dll.ToLowerInvariant()
            if ($staged.ContainsKey($name)) { continue }
            if ($name -like 'api-ms-win-*' -or $name -like 'ext-ms-*') { continue }
            if ($name -notmatch $CrtPattern -and (Test-Path (Join-Path $system32 $dll))) { continue }
            $problems += "$($bin.FullName.Substring($Stage.Length + 1)) -> $dll"
        }
    }
    if ($problems) {
        throw "The staged folder isn't self-contained; not staged, not a system DLL:`n  $($problems -join "`n  ")"
    }
    Write-Host "$(@($binaries).Count) binaries, every import staged or a system DLL"
}

# Runs qbucket.exe with nothing but Windows on PATH (no Qt, no Visual
# Studio) and waits for it: PowerShell doesn't wait for a GUI executable it
# starts with &, and doesn't always capture its output. Returns the output
# (stdout, then stderr) and the exit code.
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
$windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
if (-not (Test-Path $windeployqt)) { throw "No windeployqt.exe in $QtDir\bin" }
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
foreach ($tool in 'cl.exe', 'cmake.exe', 'ninja.exe', 'dumpbin.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not on PATH" }
}
if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is not set - not an MSVC developer environment?' }
$crtDir = Join-Path $env:VCToolsRedistDir 'x64\Microsoft.VC143.CRT'
if (-not (Test-Path (Join-Path $crtDir 'vcruntime140.dll'))) { throw "No MSVC runtime in $crtDir" }

# --- Build and test ----------------------------------------------------------
$built = Get-RepoPath $BuildDir
$tests = if ($SkipTests) { 'OFF' } else { 'ON' }
Write-Step "configure ($built)"
Invoke-Native cmake @('-S', '.', '-B', $built, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                      '-DQBUCKET_WARNINGS_AS_ERRORS=ON', "-DQBUCKET_BUILD_TESTS=$tests",
                      "-DCMAKE_PREFIX_PATH=$QtDir")
Write-Step 'build'
Invoke-Native cmake @('--build', $built, '-j')
if (-not $SkipTests) {
    Write-Step 'test'
    Invoke-Native ctest @('--test-dir', $built, '--output-on-failure')
}

# --- Stage -------------------------------------------------------------------
# Deployed afresh into an empty folder: nothing a former Qt or former flags
# left in the build directory gets in.
Write-Step 'stage'
$out = Get-RepoPath $OutDir
$stage = Join-Path $out 'stage\qbucket'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$exe = Join-Path $stage 'qbucket.exe'
Copy-Item (Join-Path $built 'qbucket.exe') $exe
$deployArgs = @('--release', '--dir', $stage, '--translations', 'de,en',
                '--no-system-d3d-compiler', '--no-opengl-sw', '--no-compiler-runtime')
# The DXC shader compiler (~20 MB) is for the D3D12 RHI only; older
# windeployqt neither deploys it nor knows the option.
if ((& $windeployqt --help | Out-String) -match '--no-system-dxc-compiler') {
    $deployArgs += '--no-system-dxc-compiler'
}
Invoke-Native $windeployqt ($deployArgs + $exe)
# Non-Qt DLLs qbucket.exe links (a shared QtKeychain, say) - windeployqt
# deploys Qt's own only.
foreach ($dll in Get-Imports $exe) {
    if (Test-Path (Join-Path $stage $dll)) { continue }
    foreach ($dir in $built, (Join-Path $QtDir 'bin')) {
        $candidate = Join-Path $dir $dll
        if (Test-Path $candidate) { Copy-Item $candidate $stage; break }
    }
}
# The MSVC runtime, app-local: runs without vc_redist and admin rights.
Copy-Item (Join-Path $crtDir '*.dll') $stage
Copy-Item (Join-Path $Repo 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $Repo 'CHANGELOG.md') $stage

Write-Step 'dependency check (staged copy)'
Test-StageDependencies $stage

# The staged copy must run on its own: version and the keychain round trip
# (Windows Credential Manager).
Write-Step 'smoke test (staged copy, nothing but Windows on PATH)'
$reported = (Invoke-Isolated $exe @('--version')).Output
if ($reported -ne "qbucket $Version") { throw "qbucket --version says '$reported', expected 'qbucket $Version'" }
Write-Host $reported
$selfTest = Invoke-Isolated $exe @('--self-test')
Write-Host $selfTest.Output
if ($selfTest.ExitCode -ne 0) {
    # An SSH session with key authentication has no logon credentials: the
    # Credential Manager refuses every write there (ERROR_NO_SUCH_LOGON_SESSION).
    if ($env:SSH_CONNECTION) {
        Write-Warning 'qbucket --self-test failed in an SSH session - the Credential Manager needs an interactive or password logon; run it once on the desktop.'
    } else {
        throw "qbucket --self-test failed ($($selfTest.ExitCode))"
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
        # Inno Setup 6, 7, ... - per machine or per user; the newest wins.
        $iscc = foreach ($root in ${env:ProgramFiles(x86)}, $env:ProgramFiles, (Join-Path $env:LOCALAPPDATA 'Programs')) {
            if ($root -and (Test-Path $root)) {
                Get-ChildItem $root -Directory -Filter 'Inno Setup *' | ForEach-Object {
                    $candidate = Join-Path $_.FullName 'ISCC.exe'
                    if (Test-Path $candidate) { $candidate }
                }
            }
        }
        $iscc = $iscc | Sort-Object {
            $v = (Get-Item $_).VersionInfo
            [version]::new($v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart)
        } -Descending | Select-Object -First 1
    }
    if (-not $iscc) {
        throw 'ISCC.exe (Inno Setup) not found; winget install JRSoftware.InnoSetup, or pass -SkipInstaller'
    }
    Write-Step "installer ($iscc)"
    Invoke-Native $iscc @("/DAppVersion=$Version", "/DStagedRoot=$stage",
                          "/DAppIconFile=$(Join-Path $Repo 'resources\app.ico')", "/O$out",
                          (Join-Path $Repo 'packaging\windows\installer.iss'))
    $setup = Join-Path $out "qbucket-$Version-windows-x64-setup.exe"
    $artifacts += $setup

    if ($TestInstaller) {
        Write-Step 'installer test'
        & (Join-Path $PSScriptRoot 'test-installer.ps1') -Setup $setup -Version $Version -Stage $stage -Iscc $iscc
    }
}

Write-Step 'done'
foreach ($a in $artifacts) {
    $hash = (Get-FileHash $a -Algorithm SHA256).Hash.ToLower()
    $mib = '{0:N1} MiB' -f ((Get-Item $a).Length / 1MB)
    Write-Host "$hash  $(Split-Path $a -Leaf)  ($mib)"
}
