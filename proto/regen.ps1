<#
.SYNOPSIS
  Regenerate proto/gen/ from micnote.proto (Windows counterpart of regen.sh).

.DESCRIPTION
  Runs the nanopb generator pinned in tools/versions.env from a throwaway venv
  under build/.nanopb-venv; nothing is installed system-wide. -Check also fails
  if proto/gen/ differs from what is committed.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File proto\regen.ps1 -Check

  Works in Windows PowerShell 5.1 and PowerShell 7+.
#>
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Console script')]
[CmdletBinding()]
param(
    [switch]$Check
)

Set-StrictMode -Version Latest
# Not 'Stop': see doctor.ps1 (5.1 turns redirected native stderr into errors).
$ErrorActionPreference = 'Continue'

$Root = Split-Path -Parent $PSScriptRoot
$OnWindows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT

function Exit-WithError([string]$Message) {
    Write-Host "error: $Message" -ForegroundColor Red
    exit 1
}

$nanopbVersion = ''
foreach ($line in Get-Content (Join-Path $Root 'tools/versions.env')) {
    if ($line -match '^\s*NANOPB_VERSION=([^\s#]+)') { $nanopbVersion = $Matches[1] }
}
if (-not $nanopbVersion) { Exit-WithError 'NANOPB_VERSION missing from tools/versions.env' }

$venv = Join-Path $Root 'build/.nanopb-venv'
$binDir = if ($OnWindows) { Join-Path $venv 'Scripts' } else { Join-Path $venv 'bin' }
$venvPython = Join-Path $binDir $(if ($OnWindows) { 'python.exe' } else { 'python' })
$stamp = Join-Path $venv ".nanopb-$nanopbVersion"

if (-not (Test-Path $stamp)) {
    Write-Host "Creating $venv with nanopb $nanopbVersion"
    if (Test-Path $venv) { Remove-Item -Recurse -Force $venv }
    if (Get-Command uv -ErrorAction SilentlyContinue) {
        & uv venv --quiet $venv
        if ($LASTEXITCODE -ne 0) { Exit-WithError 'uv venv failed' }
        & uv pip install --quiet --python $venvPython "nanopb==$nanopbVersion"
    } else {
        # The py launcher is the usual python on Windows; python3 elsewhere.
        $py = @('py', 'python3', 'python') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
        if (-not $py) { Exit-WithError 'no Python found: run scripts\bootstrap.ps1' }
        & $py -m venv $venv
        if ($LASTEXITCODE -ne 0) { Exit-WithError 'python -m venv failed' }
        & $venvPython -m pip install --quiet --disable-pip-version-check "nanopb==$nanopbVersion"
    }
    if ($LASTEXITCODE -ne 0) { Exit-WithError "installing nanopb $nanopbVersion failed" }
    New-Item -ItemType File -Path $stamp | Out-Null
}

$protoDir = Join-Path $Root 'proto'
Get-ChildItem (Join-Path $protoDir 'gen') -Filter '*.pb.*' | Remove-Item
# Run from proto/ so the paths nanopb writes into the generated files are
# relative and identical on every machine.
Push-Location $protoDir
try {
    & $venvPython -m nanopb.generator.nanopb_generator -q -I . -D gen micnote.proto
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) { Exit-WithError "nanopb_generator exited $code" }
# nanopb writes LF; .gitattributes keeps proto/gen LF in the checkout too, so
# -Check compares like with like under core.autocrlf.

Write-Host "Regenerated proto/gen with nanopb $nanopbVersion"

if ($Check) {
    & git -C $Root diff --exit-code -- proto/gen
    $dirty = $LASTEXITCODE -ne 0
    $untracked = & git -C $Root ls-files --others --exclude-standard -- proto/gen
    if ($dirty -or $untracked) { Exit-WithError 'proto/gen is out of date: run proto\regen.ps1 and commit the result' }
    Write-Host 'proto/gen is up to date'
}
exit 0
