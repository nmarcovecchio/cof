# Push this repo to GitHub (Windows). From the repo root:
#   powershell -ExecutionPolicy Bypass -File scripts\push.ps1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

git status
$msg = Read-Host "Commit message"
if ([string]::IsNullOrWhiteSpace($msg)) {
    Write-Error "Empty message, abort."
    exit 1
}

git add -A
git status --short
$ok = Read-Host "Commit and push to origin? [y/N]"
if ($ok -notmatch '^[yY](es)?$') {
    Write-Host "Aborted."
    exit 0
}

git commit -m $msg
git push -u origin HEAD
Write-Host "Done."
