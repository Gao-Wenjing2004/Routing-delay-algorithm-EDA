param(
  [string]$Zig = "zig",
  [string]$Output = "bin\estimate_v69",
  [ValidateSet("linux", "windows")]
  [string]$Target = "linux"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root
try {
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
  $triple = if ($Target -eq "windows") { "x86_64-windows-gnu" } else { "x86_64-linux-musl" }
  & $Zig c++ -target $triple -std=c++17 -O3 -DNDEBUG `
    -ffp-contract=off source\release_v69.cpp -o $Output
  if ($LASTEXITCODE -ne 0) { throw "V69 compile failed: $LASTEXITCODE" }
  Get-Item -LiteralPath $Output | Select-Object FullName,Length,LastWriteTime
} finally {
  Pop-Location
}
