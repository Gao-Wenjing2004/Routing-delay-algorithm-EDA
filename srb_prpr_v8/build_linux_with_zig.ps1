param(
    [Parameter(Mandatory = $true)]
    [string]$ZigExe
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$Submission = Join-Path $Root 'submission\bin'
New-Item -ItemType Directory -Force -Path $Build, $Submission | Out-Null

py -3.14 (Join-Path $Root 'tools\export_cpp_model.py')
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $Build 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $Build 'zig-local-cache'
New-Item -ItemType Directory -Force -Path $env:ZIG_GLOBAL_CACHE_DIR, $env:ZIG_LOCAL_CACHE_DIR | Out-Null

& $ZigExe c++ -target x86_64-linux-musl -std=c++17 -O3 -DNDEBUG -static `
    (Join-Path $Root 'src\estimate.cpp') -o (Join-Path $Submission 'estimate')
if ($LASTEXITCODE -ne 0) { throw "Zig C++ build failed: $LASTEXITCODE" }

$Bytes = (Get-Item -LiteralPath (Join-Path $Submission 'estimate')).Length
if ($Bytes -ge 90000000) { throw "Executable is $Bytes bytes; exceeds the 90 MB engineering limit" }
Write-Host "Built Linux submission binary: $Bytes bytes"
