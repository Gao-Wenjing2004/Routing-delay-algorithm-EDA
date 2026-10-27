param([string]$Zig = 'zig')
$ErrorActionPreference = 'Stop'
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot '.zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $PSScriptRoot '.zig-cache'
& $Zig c++ -std=c++17 -O3 -DNDEBUG -ffp-contract=off -pthread `
  (Join-Path $PSScriptRoot 'source\release_v71.cpp') `
  -o (Join-Path $PSScriptRoot 'bin\estimate_v71.exe')
if ($LASTEXITCODE -ne 0) { throw "V71 build failed: $LASTEXITCODE" }
