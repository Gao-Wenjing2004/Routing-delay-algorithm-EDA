param(
    [Parameter(Mandatory = $true)]
    [string]$ZigExe
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$Submission = Join-Path $Root 'submission\bin'
New-Item -ItemType Directory -Force -Path $Build, $Submission | Out-Null

$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $Build 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $Build 'zig-local-cache'
New-Item -ItemType Directory -Force -Path $env:ZIG_GLOBAL_CACHE_DIR, $env:ZIG_LOCAL_CACHE_DIR | Out-Null

$Core = Join-Path $Build 'estimate_v9_core'
& $ZigExe c++ -target x86_64-linux-musl -std=c++17 -O3 -DNDEBUG `
    -DP2_FAST_MODEL -DP2_POST_MEMORY -DV9_EXACT_MEMORY -DV9_COMPACT_LANDMARKS -DV9_COMPACT_SUBMISSION -static -w `
    (Join-Path $Root 'src\estimate_v9.cpp') -o $Core
if ($LASTEXITCODE -ne 0) { throw "Zig C++ build failed: $LASTEXITCODE" }

Copy-Item -LiteralPath $Core -Destination (Join-Path $Submission 'estimate') -Force

$Bytes = (Get-Item -LiteralPath (Join-Path $Submission 'estimate')).Length
if ($Bytes -ge 100000000) { throw "Executable is $Bytes bytes; exceeds 100 MB" }
Write-Host "Built V9 Linux submission binary: $Bytes bytes"
