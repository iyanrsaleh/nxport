# nxport installer (Windows) - run: powershell -ExecutionPolicy Bypass -File install.ps1
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'nxport-1.0.0-windows-x64.exe'
if (-not (Test-Path $src)) { $src = Join-Path $PSScriptRoot 'nxport.exe' }
if (-not (Test-Path $src)) { Write-Host "nxport.exe not found next to install.ps1"; exit 1 }

$destDir = Join-Path $env:LOCALAPPDATA 'nxport'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
Copy-Item $src (Join-Path $destDir 'nxport.exe') -Force

$p = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($p -split ';') -notcontains $destDir) {
  [Environment]::SetEnvironmentVariable('Path', ($p.TrimEnd(';') + ';' + $destDir), 'User')
  Write-Host "Added $destDir to user PATH."
}
Write-Host "Installed: $destDir\nxport.exe"
Write-Host "Open a NEW terminal, then run:  nxport"
