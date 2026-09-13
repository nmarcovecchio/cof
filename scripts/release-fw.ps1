# Compile ESP32 firmware, update actual_version/, commit and push.
# From the repo root:
#   powershell -ExecutionPolicy Bypass -File scripts\release-fw.ps1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$py = Get-Command python, python3, py -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $py) {
    Write-Error "Python not found. Install Python 3 or add it to PATH."
    exit 1
}
if ($py.Name -eq "py.exe" -or $py.Name -eq "py") {
    & $py.Source -3 scripts\release-fw.py @args
} else {
    & $py.Source scripts\release-fw.py @args
}
exit $LASTEXITCODE
