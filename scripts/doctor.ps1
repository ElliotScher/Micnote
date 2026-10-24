<#
.SYNOPSIS
  Check the dev toolchain against tools/versions.env (Windows counterpart of doctor.sh).

.DESCRIPTION
  Prints tool / required / found / status and exits non-zero if a required
  tool is missing or too old. -Quiet prints only the problems. -NoFirmware
  skips the firmware toolchain, for core/app-only setups. Optional tools
  never fail the run.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1

  Works in Windows PowerShell 5.1 and PowerShell 7+. -ExecutionPolicy Bypass
  is needed on a default Windows install, which refuses to run local scripts.
#>
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Coloured console report')]
[CmdletBinding()]
param(
    [switch]$Quiet,
    [switch]$NoFirmware
)

Set-StrictMode -Version Latest
# Not 'Stop': Windows PowerShell 5.1 turns any stderr line from a native
# command redirected with 2>&1 into a terminating error under 'Stop'.
$ErrorActionPreference = 'Continue'

$Root = Split-Path -Parent $PSScriptRoot
# $IsWindows/$IsMacOS don't exist in 5.1, and StrictMode throws on reading them.
$OnWindows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$OnMacOS = (-not $OnWindows) -and ((uname -s) -eq 'Darwin')
# toolchain-manager's --shell isn't supported on Windows.
$launchShell = if ($OnWindows) { '-- powershell' } else { '--shell' }

# --- versions.env -----------------------------------------------------------
$V = @{}
$envFile = Join-Path $Root 'tools/versions.env'
if (-not (Test-Path $envFile)) { Write-Host "error: missing $envFile" -ForegroundColor Red; exit 1 }
foreach ($line in Get-Content $envFile) {
    if ($line -match '^\s*([A-Z0-9_]+)=([^\s#]*)') { $V[$Matches[1]] = $Matches[2] }
}

# --- Helpers ----------------------------------------------------------------
$Rows = [System.Collections.Generic.List[object]]::new()
$script:Failed = $false

function Add-Row([string]$Tool, [string]$Required, [string]$Found, [string]$Status, [string]$Note = '') {
    $Rows.Add([pscustomobject]@{ Tool = $Tool; Required = $Required; Found = $Found; Status = $Status; Note = $Note })
    if ($Status -in 'MISSING', 'OLD') { $script:Failed = $true }
}

function Test-Command([string]$Name) { [bool](Get-Command $Name -ErrorAction SilentlyContinue) }

# First N.N[.N...] in the text, or ''.
function Get-VersionString([string]$Text) {
    if ($Text -match '(\d+(\.\d+)+)') { return $Matches[1] }
    return ''
}

# Dotted compare; missing parts are 0 and non-numeric suffixes are ignored.
function Test-VersionAtLeast([string]$Found, [string]$Min) {
    $a = $Found.Split('.'); $b = $Min.Split('.')
    for ($i = 0; $i -lt [Math]::Max($a.Count, $b.Count); $i++) {
        $x = if ($i -lt $a.Count -and $a[$i] -match '^(\d+)') { [long]$Matches[1] } else { 0 }
        $y = if ($i -lt $b.Count -and $b[$i] -match '^(\d+)') { [long]$Matches[1] } else { 0 }
        if ($x -gt $y) { return $true }
        if ($x -lt $y) { return $false }
    }
    return $true
}

function Get-ToolVersion([string]$Name, [string[]]$Arguments = @('--version')) {
    if (-not (Test-Command $Name)) { return '' }
    try { $out = (& $Name @Arguments 2>&1 | Select-Object -First 5) -join "`n" } catch { return '' }
    Get-VersionString $out
}

function Test-Minimum([string]$Name, [string]$Min, [string]$Found, [switch]$Optional) {
    if (-not $Found) { Add-Row $Name ">= $Min" '-' ($(if ($Optional) { 'WARN' } else { 'MISSING' })) }
    elseif (Test-VersionAtLeast $Found $Min) { Add-Row $Name ">= $Min" $Found 'OK' }
    else { Add-Row $Name ">= $Min" $Found ($(if ($Optional) { 'WARN' } else { 'OLD' })) }
}

# --- Build tools ------------------------------------------------------------
Test-Minimum 'cmake' $V.CMAKE_MIN (Get-ToolVersion 'cmake')
Test-Minimum 'ninja' $V.NINJA_MIN (Get-ToolVersion 'ninja')

# --- C++ compiler and C++23 probe -------------------------------------------
$probeSrc = "#include <expected>`n#include <print>`nint main(){std::expected<int,int> e{1}; std::println(""{}"", *e);}`n"
$probeDir = Join-Path ([IO.Path]::GetTempPath()) ("micnote-doctor-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $probeDir -ErrorAction Stop | Out-Null
try {
    $probe = Join-Path $probeDir 'probe.cpp'
    Set-Content -Path $probe -Value $probeSrc -NoNewline

    if ($OnWindows) {
        # MSVC is the baseline compiler on Windows; vswhere finds it without a dev prompt.
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vsPath = if (Test-Path $vswhere) {
            & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        }
        $vcvars = if ($vsPath) { Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat' }
        if ($vcvars -and (Test-Path $vcvars)) {
            $clOut = cmd /c "call `"$vcvars`" >nul && cl 2>&1"
            Test-Minimum 'MSVC (cl)' $V.MSVC_MIN (Get-VersionString ($clOut -join "`n"))
            $log = cmd /c "call `"$vcvars`" >nul && cd /d `"$probeDir`" && cl /nologo /std:c++latest /EHsc probe.cpp 2>&1"
            if ($LASTEXITCODE -eq 0) { Add-Row 'C++23 probe (MSVC)' '<expected>, <print>' 'compiles' 'OK' }
            else { Add-Row 'C++23 probe (MSVC)' '<expected>, <print>' 'fails' 'OLD' (($log | Select-String 'error' | Select-Object -First 1) -as [string]) }
        }
        else {
            Add-Row 'MSVC (cl)' ">= $($V.MSVC_MIN)" '-' 'MISSING' 'VS 2022 Build Tools with the C++ workload (bootstrap.ps1)'
            Add-Row 'C++23 probe (MSVC)' '<expected>, <print>' '-' 'SKIP' 'no MSVC'
        }
        if (Test-Command 'clang++') {
            Test-Minimum 'clang++ (optional)' $V.CLANG_MIN (Get-ToolVersion 'clang++') -Optional
        }
    }
    else {
        # pwsh on Linux/macOS: same compiler logic as doctor.sh.
        # Same choice CMake makes: $CXX, else c++, g++, clang++ in that order.
        $cxx = if ($env:CXX) { $env:CXX } else { @('c++', 'g++', 'clang++') | Where-Object { Test-Command $_ } | Select-Object -First 1 }
        if (-not $cxx -or -not (Test-Command $cxx)) {
            Add-Row 'C++ compiler' "clang >= $($V.CLANG_MIN) or gcc >= $($V.GCC_MIN)" '-' 'MISSING'
            Add-Row 'C++23 probe' '<expected>, <print>' '-' 'SKIP' 'no compiler'
        }
        else {
            $banner = (& $cxx --version 2>&1 | Select-Object -First 1) -as [string]
            $ver = Get-VersionString $banner
            if (-not $ver) { Add-Row "C++ compiler ($cxx)" "clang >= $($V.CLANG_MIN) or gcc >= $($V.GCC_MIN)" '?' 'MISSING' '--version printed nothing' }
            elseif ($banner -match 'Apple clang') { Add-Row "C++ compiler ($cxx)" 'Xcode clang' $ver 'OK' 'Apple clang: checked by the probe' }
            elseif ($banner -match 'clang') { Test-Minimum "C++ compiler ($cxx, clang)" $V.CLANG_MIN $ver }
            else { Test-Minimum "C++ compiler ($cxx, gcc)" $V.GCC_MIN $ver }
            if ($ver) {
                $probeExe = Join-Path $probeDir 'probe'
                $log = & $cxx -std=c++23 $probe -o $probeExe 2>&1
                if ($LASTEXITCODE -ne 0) { Add-Row 'C++23 probe' '<expected>, <print>' 'fails' 'OLD' (($log | Select-Object -First 1) -as [string]) }
                else {
                    # Run it too: an inherited LD_LIBRARY_PATH can load an older libstdc++ than the compiler's.
                    $runOut = & $probeExe 2>&1
                    if ($LASTEXITCODE -eq 0) { Add-Row 'C++23 probe' '<expected>, <print>' 'builds, runs' 'OK' }
                    else {
                        $note = (($runOut | Select-Object -First 1) -as [string])
                        if ($note -match "version ``[^']*' not found") { $note = $Matches[0] }
                        if ($env:LD_LIBRARY_PATH) { $note += ' (LD_LIBRARY_PATH is set; unset it)' }
                        Add-Row 'C++23 probe' '<expected>, <print>' "won't run" 'OLD' $note
                    }
                }
            }
        }
    }
}
finally {
    Remove-Item -Recurse -Force $probeDir -ErrorAction SilentlyContinue
}

# --- Firmware ---------------------------------------------------------------
# On Windows `python3` is often the Microsoft Store stub, so try the real names in order.
$pyVer = ''
foreach ($py in 'python3', 'python', 'py') {
    $pyVer = Get-ToolVersion $py
    if ($pyVer) { break }
}
Test-Minimum 'python' $V.PYTHON_MIN $pyVer

if ($NoFirmware) { Add-Row 'Firmware toolchain' '-' '-' 'SKIP' '-NoFirmware' }
else {
    if ($env:ZEPHYR_SDK_INSTALL_DIR) {
        $sdkFile = Join-Path $env:ZEPHYR_SDK_INSTALL_DIR 'sdk_version'
        $sdk = if (Test-Path $sdkFile) { (Get-Content $sdkFile -Raw).Trim() } else { '' }
        if (-not $sdk) { Add-Row 'Zephyr SDK' $V.ZEPHYR_SDK_VERSION '-' 'MISSING' 'no sdk_version in $ZEPHYR_SDK_INSTALL_DIR' }
        elseif ($sdk -eq $V.ZEPHYR_SDK_VERSION) { Add-Row 'Zephyr SDK' $V.ZEPHYR_SDK_VERSION $sdk 'OK' }
        else { Add-Row 'Zephyr SDK' $V.ZEPHYR_SDK_VERSION $sdk 'OLD' "NCS $($V.NCS_VERSION) documents SDK $($V.ZEPHYR_SDK_VERSION)" }
        Test-Minimum 'west' '1.0' (Get-ToolVersion 'west')
        Test-Minimum 'dtc' $V.DTC_MIN (Get-ToolVersion 'dtc')
    }
    else {
        # Nordic's toolchain-manager bundles west, dtc and the SDK per NCS version.
        $ncsFound = $false
        if (Test-Command 'nrfutil') {
            $list = (& nrfutil toolchain-manager list 2>&1) -join "`n"
            $ncsFound = $LASTEXITCODE -eq 0 -and ([regex]::Matches($list, 'v\d+\.\d+\.\d+') | Where-Object Value -EQ $V.NCS_VERSION)
        }
        if ($ncsFound) { Add-Row 'NCS toolchain' $V.NCS_VERSION $V.NCS_VERSION 'OK' 'nrfutil toolchain-manager' }
        else { Add-Row 'NCS toolchain' $V.NCS_VERSION '-' 'MISSING' "nrfutil toolchain-manager install --ncs-version $($V.NCS_VERSION)" }
        foreach ($t in 'west', 'dtc') {
            if (Test-Command $t) { Add-Row $t '-' (Get-ToolVersion $t) 'OK' }
            elseif ($ncsFound) { Add-Row $t '-' 'in NCS env' 'OK' "nrfutil toolchain-manager launch --ncs-version $($V.NCS_VERSION) $launchShell" }
            else { Add-Row $t '-' '-' 'MISSING' }
        }
    }

    if (Test-Command 'nrfutil') { Add-Row 'nrfutil' '-' (Get-ToolVersion 'nrfutil') 'OK' }
    else { Add-Row 'nrfutil' '-' '-' 'MISSING' 'see docs/dev-setup.md' }

    $jlink = if ($OnWindows) {
        $p = Join-Path $env:ProgramFiles 'SEGGER\JLink\JLink.exe'
        if (Test-Command 'JLink.exe') { 'JLink.exe' } elseif (Test-Path $p) { $p }
    }
    elseif (Test-Command 'JLinkExe') { 'JLinkExe' }
    if ($jlink) { Add-Row 'J-Link' '-' 'present' 'OK' }
    elseif (Test-Command 'pyocd') { Add-Row 'J-Link' '-' '-' 'WARN' 'no J-Link; flash with pyOCD or nrfutil instead' }
    else { Add-Row 'J-Link' '-' '-' 'WARN' 'no J-Link or pyOCD; needed only for flashing' }
}

# --- App --------------------------------------------------------------------
if (Test-Command 'flutter') {
    $flOut = (& flutter --version 2>$null | Where-Object { $_ -match '^Flutter' } | Select-Object -First 1) -as [string]
    $fl = Get-VersionString $flOut
    $want = $V.FLUTTER_VERSION
    $minor = { param($s) ($s.Split('.') | Select-Object -First 2) -join '.' }
    if (-not $fl) { Add-Row 'flutter' $want '?' 'MISSING' "couldn't parse flutter --version" }
    elseif ($fl -eq $want) { Add-Row 'flutter' $want $fl 'OK' }
    elseif ((& $minor $fl) -eq (& $minor $want)) { Add-Row 'flutter' $want $fl 'WARN' 'same minor, different patch' }
    else { Add-Row 'flutter' $want $fl 'OLD' }
}
else { Add-Row 'flutter' $V.FLUTTER_VERSION '-' 'MISSING' }

# --- Optional ---------------------------------------------------------------
$sdkCandidates = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT)
if ($env:LOCALAPPDATA) { $sdkCandidates += Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if ($HOME) { $sdkCandidates += (Join-Path $HOME 'Android/Sdk'), (Join-Path $HOME 'Library/Android/sdk') }
$androidSdk = $sdkCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
$plat = "platform $($V.ANDROID_PLATFORM)"
if (-not $androidSdk) { Add-Row 'Android SDK (optional)' $plat '-' 'SKIP' 'bootstrap.ps1 -Android' }
else {
    if (Test-Path (Join-Path $androidSdk "platforms/android-$($V.ANDROID_PLATFORM)")) { Add-Row 'Android SDK (optional)' $plat $androidSdk 'OK' }
    else { Add-Row 'Android SDK (optional)' $plat $androidSdk 'WARN' "android-$($V.ANDROID_PLATFORM) not installed" }
    if (Test-Path (Join-Path $androidSdk "ndk/$($V.ANDROID_NDK_VERSION)")) { Add-Row 'Android NDK (optional)' $V.ANDROID_NDK_VERSION $V.ANDROID_NDK_VERSION 'OK' }
    else { Add-Row 'Android NDK (optional)' $V.ANDROID_NDK_VERSION '-' 'WARN' }
}

if (Test-Command 'kicad-cli') {
    # stdout only, and only on success: a crashing kicad-cli prints its store/install path, which contains a version.
    $kicadOut = (& kicad-cli --version 2>$null) -join "`n"
    $kicad = if ($LASTEXITCODE -eq 0) { Get-VersionString $kicadOut } else { '' }
    if ($kicad) { Test-Minimum 'KiCad (optional)' $V.KICAD_MIN $kicad -Optional }
    else { Add-Row 'KiCad (optional)' ">= $($V.KICAD_MIN)" '?' 'WARN' 'kicad-cli --version failed' }
}
else { Add-Row 'KiCad (optional)' ">= $($V.KICAD_MIN)" '-' 'SKIP' 'bootstrap.ps1 -Hardware' }

if ($env:VULKAN_SDK -or (Test-Command 'vulkaninfo')) { Add-Row 'Vulkan (optional)' '-' 'present' 'OK' }
elseif (-not $OnMacOS) { Add-Row 'Vulkan (optional)' '-' '-' 'SKIP' 'GPU builds of whisper.cpp only' }

foreach ($t in 'clang-format', 'clang-tidy') {
    if (Test-Command $t) { Add-Row "$t (optional)" '-' (Get-ToolVersion $t) 'OK' }
    else { Add-Row "$t (optional)" '-' '-' 'SKIP' }
}

# --- Report -----------------------------------------------------------------
$shown = if ($Quiet) { @($Rows | Where-Object Status -NotIn 'OK', 'SKIP') } else { @($Rows) }
if ($shown.Count -gt 0) {
    $w1 = [Math]::Max(4, ($Rows.Tool | Measure-Object Length -Maximum).Maximum)
    $w2 = [Math]::Max(8, ($Rows.Required | Measure-Object Length -Maximum).Maximum)
    $w3 = [Math]::Max(5, ($Rows.Found | Measure-Object Length -Maximum).Maximum)
    Write-Host ("{0,-$w1}  {1,-$w2}  {2,-$w3}  {3}" -f 'Tool', 'Required', 'Found', 'Status')
    foreach ($r in $shown) {
        $colour = switch ($r.Status) { 'OK' { 'Green' } 'WARN' { 'Yellow' } { $_ -in 'MISSING', 'OLD' } { 'Red' } default { 'DarkGray' } }
        Write-Host ("{0,-$w1}  {1,-$w2}  {2,-$w3}  " -f $r.Tool, $r.Required, $r.Found) -NoNewline
        Write-Host ("{0,-7}" -f $r.Status) -ForegroundColor $colour -NoNewline
        Write-Host (" " + $r.Note) -ForegroundColor DarkGray
    }
}

if ($script:Failed) {
    Write-Host 'error: required tools are missing or too old (pins: tools/versions.env; help: docs/dev-setup.md)' -ForegroundColor Red
    exit 1
}
if (-not $Quiet) { Write-Host 'All required tools found.' -ForegroundColor Green }
exit 0
