<#
.SYNOPSIS
  Install Micnote's development tools on Windows (winget). Linux and macOS use bootstrap.sh.

.DESCRIPTION
  Prints what it will do and asks first. Steps that are already satisfied are
  left out, so re-running is safe. The user PATH is only changed after asking
  (or with -Yes). Finishes by running scripts/doctor.ps1.

  -Yes         don't ask before installing
  -DryRun      print the plan and exit
  -NoFirmware  skip nrfutil, the NCS toolchain and J-Link
  -Android     add a JDK and the Android SDK, NDK and build tools
  -Hardware    add KiCad and FreeCAD

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1

  Works in Windows PowerShell 5.1 and PowerShell 7+. Run it as your normal
  user: the Visual Studio Build Tools installer asks for elevation itself.
#>
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Interactive installer output')]
[CmdletBinding()]
param(
    [switch]$Yes,
    [switch]$DryRun,
    [switch]$NoFirmware,
    [switch]$Android,
    [switch]$Hardware
)

Set-StrictMode -Version Latest
$AssumeYes = [bool]$Yes
# Not 'Stop': 5.1 turns stderr from native commands into terminating errors.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is very slow with the progress bar in 5.1

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    Write-Host 'error: bootstrap.ps1 is for Windows; on Linux and macOS run scripts/bootstrap.sh' -ForegroundColor Red
    exit 1
}

$Root = Split-Path -Parent $PSScriptRoot
$V = @{}
foreach ($line in Get-Content (Join-Path $Root 'tools/versions.env')) {
    if ($line -match '^\s*([A-Z0-9_]+)=([^\s#]*)') { $V[$Matches[1]] = $Matches[2] }
}

$FlutterDir = Join-Path $env:LOCALAPPDATA 'flutter'
$NrfutilDir = Join-Path $env:LOCALAPPDATA 'Programs\nrfutil'
$AndroidSdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
# sdkmanager is deprecated in favour of the `android` CLI, and only the CLI
# itself accepts --no-metrics (it collects usage data otherwise).
$AndroidCli = Join-Path $AndroidSdk 'cmdline-tools\latest\bin\android.exe'

# --- Helpers ----------------------------------------------------------------
function Write-Info([string]$Msg) { Write-Host $Msg }
function Write-FatalError([string]$Msg) { Write-Host "error: $Msg" -ForegroundColor Red; exit 1 }
function Test-Command([string]$Name) { [bool](Get-Command $Name -ErrorAction SilentlyContinue) }
function Get-VersionString([string]$Text) { if ($Text -match '(\d+(\.\d+)+)') { $Matches[1] } else { '' } }

function Test-VersionAtLeast([string]$Found, [string]$Min) {
    if (-not $Found) { return $false }
    $a = $Found.Split('.'); $b = $Min.Split('.')
    for ($i = 0; $i -lt [Math]::Max($a.Count, $b.Count); $i++) {
        $x = if ($i -lt $a.Count -and $a[$i] -match '^(\d+)') { [long]$Matches[1] } else { 0 }
        $y = if ($i -lt $b.Count -and $b[$i] -match '^(\d+)') { [long]$Matches[1] } else { 0 }
        if ($x -gt $y) { return $true }
        if ($x -lt $y) { return $false }
    }
    return $true
}

function Get-ToolVersion([string]$Name) {
    if (-not (Test-Command $Name)) { return '' }
    Get-VersionString ((& $Name --version 2>&1 | Select-Object -First 3) -join "`n")
}

function Confirm-Step([string]$Question) {
    if ($AssumeYes) { return $true }
    if (-not [Environment]::UserInteractive) { return $false }
    (Read-Host "$Question [y/N]") -match '^(y|yes)$'
}

# Re-read PATH from the registry so tools installed by winget are visible here.
function Sync-SessionPath {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = (@($machine, $user) | Where-Object { $_ }) -join ';'
}

# Add a directory to the user PATH (after asking) and to this session.
$Notes = [System.Collections.Generic.List[string]]::new()
function Add-UserPath([string]$Dir) {
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @()
    if ($user) { $parts = $user.Split(';') | Where-Object { $_ } }
    if ($parts -notcontains $Dir) {
        if (Confirm-Step "Add $Dir to your user PATH?") {
            [Environment]::SetEnvironmentVariable('Path', (@($parts) + $Dir) -join ';', 'User')
            $Notes.Add("Added $Dir to your user PATH; open a new terminal to pick it up.")
        }
        else { $Notes.Add("Add $Dir to your PATH to use it from a terminal.") }
    }
    if (($env:Path.Split(';')) -notcontains $Dir) { $env:Path = "$Dir;$env:Path" }
}

function Invoke-Winget([string]$Id, [string[]]$Extra = @()) {
    if (-not (Test-Command 'winget')) {
        Write-FatalError "winget is needed to install $Id. Install 'App Installer' from the Microsoft Store, then re-run."
    }
    & winget install --id $Id --exact --silent --accept-source-agreements --accept-package-agreements @Extra
    # -1978335189: already installed and no newer version.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) { Write-FatalError "winget install $Id failed ($LASTEXITCODE)" }
    Sync-SessionPath
}

function Get-PythonVersion {
    # `python3`/`python` may be the Microsoft Store stub, which prints no version.
    foreach ($py in 'py', 'python', 'python3') {
        $v = Get-ToolVersion $py
        if ($v) { return $v }
    }
    ''
}

function Get-VsInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return '' }
    (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) -as [string]
}

function Get-FlutterVersion([string]$Exe) {
    if (-not $Exe) { return '' }
    Get-VersionString ((& $Exe --version 2>$null | Where-Object { $_ -match '^Flutter' } | Select-Object -First 1) -as [string])
}

function Test-NcsInstalled {
    $nrf = Get-Command 'nrfutil' -ErrorAction SilentlyContinue
    if (-not $nrf) { return $false }
    $list = (& $nrf.Source toolchain-manager list 2>&1) -join "`n"
    [bool]([regex]::Matches($list, 'v\d+\.\d+\.\d+') | Where-Object Value -EQ $V.NCS_VERSION)
}

# --- Plan -------------------------------------------------------------------
$Plan = [System.Collections.Generic.List[object]]::new()
function Add-Step([string]$Desc, [scriptblock]$Action) { $Plan.Add([pscustomobject]@{ Desc = $Desc; Action = $Action }) }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($isAdmin) { Write-Host 'warning: running elevated; per-user installs (Flutter, PATH) will go to this admin account.' -ForegroundColor Yellow }

if (-not (Test-Command 'git')) { Add-Step 'Install Git (winget Git.Git)' { Invoke-Winget 'Git.Git' } }
if (-not (Test-VersionAtLeast (Get-ToolVersion 'cmake') $V.CMAKE_MIN)) { Add-Step "Install CMake >= $($V.CMAKE_MIN) (winget Kitware.CMake)" { Invoke-Winget 'Kitware.CMake' } }
if (-not (Test-VersionAtLeast (Get-ToolVersion 'ninja') $V.NINJA_MIN)) { Add-Step "Install Ninja >= $($V.NINJA_MIN) (winget Ninja-build.Ninja)" { Invoke-Winget 'Ninja-build.Ninja' } }
if (-not (Test-Command 'clang-format')) { Add-Step 'Install LLVM: clang, clang-format, clang-tidy (winget LLVM.LLVM)' { Invoke-Winget 'LLVM.LLVM' } }
if (-not (Test-VersionAtLeast (Get-PythonVersion) $V.PYTHON_MIN)) { Add-Step 'Install Python 3.13 (winget Python.Python.3.13)' { Invoke-Winget 'Python.Python.3.13' } }
if (-not (Get-VsInstallPath)) {
    Add-Step 'Install Visual Studio Build Tools with the C++ workload (winget; asks for elevation)' {
        Invoke-Winget 'Microsoft.VisualStudio.BuildTools' @('--override', '--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait')
    }
}

# Flutter, cloned at the pinned tag.
$flutterExe = Join-Path $FlutterDir 'bin\flutter.bat'
$haveFlutter = ((Test-Path $flutterExe) -and (Get-FlutterVersion $flutterExe) -eq $V.FLUTTER_VERSION)
if (-not $haveFlutter -and (Test-Command 'flutter') -and (Get-FlutterVersion 'flutter') -eq $V.FLUTTER_VERSION) {
    $haveFlutter = $true
    $FlutterDir = Split-Path -Parent (Split-Path -Parent (Get-Command 'flutter').Source)
}
if (-not $haveFlutter) {
    Add-Step "Install Flutter $($V.FLUTTER_VERSION) into $FlutterDir" {
        if (Test-Path (Join-Path $FlutterDir '.git')) {
            & git -C $FlutterDir fetch --depth 1 origin tag $V.FLUTTER_VERSION
            & git -C $FlutterDir checkout -q $V.FLUTTER_VERSION
        }
        else {
            & git clone --depth 1 --branch $V.FLUTTER_VERSION https://github.com/flutter/flutter.git $FlutterDir
        }
        if ($LASTEXITCODE -ne 0) { Write-FatalError 'cloning Flutter failed' }
        & (Join-Path $FlutterDir 'bin\flutter.bat') --version # first run downloads the Dart SDK
    }
}

if (-not $NoFirmware) {
    if (-not (Test-Command 'nrfutil') -and -not (Test-Path (Join-Path $NrfutilDir 'nrfutil.exe'))) {
        Add-Step "Download nrfutil into $NrfutilDir" {
            New-Item -ItemType Directory -Force -Path $NrfutilDir | Out-Null
            Invoke-WebRequest -UseBasicParsing -OutFile (Join-Path $NrfutilDir 'nrfutil.exe') `
                -Uri 'https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/x86_64-pc-windows-msvc/nrfutil.exe'
        }
    }
    if (-not (Test-NcsInstalled)) {
        Add-Step "Install the NCS $($V.NCS_VERSION) toolchain (nrfutil toolchain-manager; a large download)" {
            Add-UserPath $NrfutilDir
            & nrfutil toolchain-manager --version *> $null
            if ($LASTEXITCODE -ne 0) { & nrfutil install toolchain-manager }
            & nrfutil device --version *> $null
            if ($LASTEXITCODE -ne 0) { & nrfutil install device }
            & nrfutil toolchain-manager install --ncs-version $V.NCS_VERSION
            if ($LASTEXITCODE -ne 0) { Write-FatalError 'installing the NCS toolchain failed' }
        }
    }
    $jlink = (Test-Command 'JLink.exe') -or (Test-Path (Join-Path $env:ProgramFiles 'SEGGER\JLink\JLink.exe'))
    if (-not $jlink) {
        Add-Step 'SEGGER J-Link (manual download)' {
            Write-Info "SEGGER's licence doesn't allow redistributing J-Link, and it isn't on winget."
            Write-Info 'Download the Windows installer from https://www.segger.com/downloads/jlink/ and run it.'
            if (-not $AssumeYes -and [Environment]::UserInteractive) { $null = Read-Host "Press Enter once it's installed, or to skip for now" }
        }
    }
}

if ($Android) {
    if (-not (Test-Command 'javac')) {
        Add-Step "Install a JDK $($V.JAVA_VERSION) (winget EclipseAdoptium.Temurin.$($V.JAVA_VERSION).JDK)" { Invoke-Winget "EclipseAdoptium.Temurin.$($V.JAVA_VERSION).JDK" }
    }
    if (-not (Test-Path $AndroidCli)) {
        Add-Step "Download Android command-line tools into $AndroidSdk" {
            $tmp = Join-Path ([IO.Path]::GetTempPath()) ("micnote-android-" + [guid]::NewGuid())
            New-Item -ItemType Directory -Path $tmp | Out-Null
            $zip = Join-Path $tmp 'tools.zip'
            Invoke-WebRequest -UseBasicParsing -OutFile $zip `
                -Uri "https://dl.google.com/android/repository/commandlinetools-win-$($V.ANDROID_CMDLINE_TOOLS)_latest.zip"
            Expand-Archive -Path $zip -DestinationPath $tmp
            New-Item -ItemType Directory -Force -Path (Join-Path $AndroidSdk 'cmdline-tools') | Out-Null
            $dest = Join-Path $AndroidSdk 'cmdline-tools\latest'
            if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
            Move-Item (Join-Path $tmp 'cmdline-tools') $dest
            Remove-Item -Recurse -Force $tmp
        }
    }
    $needSdk = -not ((Test-Path (Join-Path $AndroidSdk "platforms\android-$($V.ANDROID_PLATFORM)")) -and
        (Test-Path (Join-Path $AndroidSdk "ndk\$($V.ANDROID_NDK_VERSION)")) -and
        (Test-Path (Join-Path $AndroidSdk "build-tools\$($V.ANDROID_BUILD_TOOLS)")))
    if ($needSdk) {
        Add-Step "Install Android SDK packages (platform $($V.ANDROID_PLATFORM), NDK $($V.ANDROID_NDK_VERSION); under Google's SDK terms)" {
            & $AndroidCli --no-metrics "--sdk=$AndroidSdk" sdk install 'platform-tools' "platforms;android-$($V.ANDROID_PLATFORM)" `
                "build-tools;$($V.ANDROID_BUILD_TOOLS)" "ndk;$($V.ANDROID_NDK_VERSION)" "cmake;$($V.ANDROID_CMAKE)"
            if ($LASTEXITCODE -ne 0) { Write-FatalError 'installing Android SDK packages failed' }
        }
    }
}

if ($Hardware) {
    if (-not (Test-VersionAtLeast (Get-ToolVersion 'kicad-cli') $V.KICAD_MIN)) { Add-Step "Install KiCad >= $($V.KICAD_MIN) (winget KiCad.KiCad)" { Invoke-Winget 'KiCad.KiCad' } }
    if (-not (Test-Command 'freecad') -and -not (Test-Command 'FreeCAD')) { Add-Step 'Install FreeCAD (winget FreeCAD.FreeCAD)' { Invoke-Winget 'FreeCAD.FreeCAD' } }
}

# --- Show, confirm, run -----------------------------------------------------
if ($Plan.Count -eq 0) {
    Write-Info 'Nothing to install: everything bootstrap manages is already here.'
}
else {
    Write-Info 'bootstrap will:'
    for ($i = 0; $i -lt $Plan.Count; $i++) { Write-Info ("  {0}. {1}" -f ($i + 1), $Plan[$i].Desc) }
    if ($DryRun) { Write-Info '(dry run: nothing was changed)'; exit 0 }
    if (-not (Confirm-Step 'Continue?')) { Write-FatalError 'nothing changed. Re-run with -Yes to skip this question.' }
    foreach ($s in $Plan) {
        Write-Info ''
        Write-Host "==> $($s.Desc)" -ForegroundColor Green
        & $s.Action
    }
}
if ($DryRun) { exit 0 }

# --- Follow-ups and doctor --------------------------------------------------
if (Test-Path (Join-Path $FlutterDir 'bin\flutter.bat')) { Add-UserPath (Join-Path $FlutterDir 'bin') }
if (Test-Path (Join-Path $NrfutilDir 'nrfutil.exe')) { Add-UserPath $NrfutilDir }
if ($Android) {
    if ([Environment]::GetEnvironmentVariable('ANDROID_HOME', 'User') -ne $AndroidSdk) {
        if (Confirm-Step "Set ANDROID_HOME=$AndroidSdk for your user?") {
            [Environment]::SetEnvironmentVariable('ANDROID_HOME', $AndroidSdk, 'User')
        }
        else { $Notes.Add("Set ANDROID_HOME=$AndroidSdk") }
    }
    $env:ANDROID_HOME = $AndroidSdk
}
if (-not $NoFirmware -and (Test-NcsInstalled)) {
    # --shell isn't supported on Windows; launch a shell as the command instead.
    $Notes.Add("Firmware builds run inside the NCS toolchain: nrfutil toolchain-manager launch --ncs-version $($V.NCS_VERSION) -- powershell")
}
if ($Notes.Count -gt 0) {
    Write-Info ''
    Write-Info 'Next steps:'
    foreach ($n in $Notes) { Write-Info "  - $n" }
}

Write-Info ''
Write-Info 'Running scripts/doctor.ps1:'
$doctorArgs = @()
if ($NoFirmware) { $doctorArgs += '-NoFirmware' }
& (Join-Path $PSScriptRoot 'doctor.ps1') @doctorArgs
exit $LASTEXITCODE
