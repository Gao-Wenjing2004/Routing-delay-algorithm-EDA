$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Target = Join-Path $ProjectDir "submission_windows"
if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target -Recurse -Force }
New-Item -ItemType Directory -Path $Target | Out-Null
$Source = Join-Path $ProjectDir "estimate.exe"
if (-not (Test-Path -LiteralPath $Source)) { throw "estimate.exe is missing; run build_v5.ps1 first" }
Copy-Item -LiteralPath $Source -Destination $Target
Write-Host "Created single-executable package: $Target"
