param(
    [Parameter(Mandatory = $true)]
    [string]$ZigExe
)

$ErrorActionPreference = 'Stop'
$ZigExe = (Resolve-Path -LiteralPath $ZigExe).Path
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$Submission = Join-Path $Root 'submission_p38\bin'
New-Item -ItemType Directory -Force -Path $Build, $Submission | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $Build 'zig-p38-linux-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $Build 'zig-p38-linux-local'
New-Item -ItemType Directory -Force -Path $env:ZIG_GLOBAL_CACHE_DIR, $env:ZIG_LOCAL_CACHE_DIR | Out-Null

$CppObject = Join-Path $Build 'estimate_v9_p38_linux.o'
$AtlasObject = Join-Path $Build 'p14_atlas_data_linux.o'
$Core = Join-Path $Build 'estimate_v9_p38_core'

Push-Location $Root
try {
    & $ZigExe c++ -target x86_64-linux-musl -std=c++17 -O3 -DNDEBUG `
        -DP2_FAST_MODEL -DP18_FAST_BASE -DP18_TWO_COLUMN_IO `
        -DP18_PERFECT_PORT_HASH -DP18_TRUST_PORTS -DP20_BLOCK_SIDES `
        -DP21_TRUST_ENDPOINTS -DP24_DEFAULT_SUBMISSION_FAST_PATH `
        -DP25_PAIR_NUMBER_FORMAT -DP35_FAST_SOA -DP36_AVX2_DISPATCH `
        -DP38_FAST_SUBMISSION_PREDICT -DV9_COMPACT_SUBMISSION `
        -DV9_PACKED_ATLAS -DV9_DISABLE_LEARNED_SELECTOR -w -c `
        'src\estimate_v9.cpp' -o $CppObject
    if ($LASTEXITCODE -ne 0) { throw "Zig C++ compile failed: $LASTEXITCODE" }

    if (-not (Test-Path -LiteralPath $AtlasObject)) {
        & $ZigExe cc -target x86_64-linux-musl -c `
            'src\p14_atlas_data.S' -o $AtlasObject
        if ($LASTEXITCODE -ne 0) { throw "Zig Atlas compile failed: $LASTEXITCODE" }
    }

    & $ZigExe c++ -target x86_64-linux-musl -static -s -w `
        $CppObject $AtlasObject -o $Core
    if ($LASTEXITCODE -ne 0) { throw "Zig link failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}

Copy-Item -LiteralPath $Core -Destination (Join-Path $Submission 'estimate') -Force
$Bytes = (Get-Item -LiteralPath (Join-Path $Submission 'estimate')).Length
if ($Bytes -ge 100000000) { throw "Executable is $Bytes bytes; exceeds 100 MB" }
Write-Host "Built V9-P38 Linux submission binary: $Bytes bytes"
