param([switch]$Native)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ProjectDir
$Python = Join-Path $env:USERPROFILE 'anaconda3\python.exe'
if (-not (Test-Path -LiteralPath $Python)) { throw "Python was not found: $Python" }
$Compiler = Join-Path $env:USERPROFILE 'anaconda3\Library\mingw-w64\bin\x86_64-w64-mingw32-g++.exe'
if (-not (Test-Path -LiteralPath $Compiler)) { throw "g++ was not found: $Compiler" }

Push-Location $RepoRoot
try {
    & $Python .\srb_fast_v6\verify_v6_assets.py `
        --manifest .\srb_fast_v6\v6_model_manifest.json `
        --header .\srb_fast_v6\v6_macro10_data.hpp `
        --arch-dir .\plusone-srb_fast_v3\srb_fast_v3\arch `
        --atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin
    if ($LASTEXITCODE -ne 0) { throw "V6 asset verification failed" }
    $Flags = @('-std=c++1z', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-pedantic',
               '-static', '-static-libgcc', '-static-libstdc++')
    if ($Native) { $Flags += '-march=native' }
    $OriginalPath = $env:PATH
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
    & $Compiler @Flags .\srb_fast_v6\estimate_v6.cpp -o .\srb_fast_v6\estimate_v6.exe
    $env:PATH = $OriginalPath
    if ($LASTEXITCODE -ne 0) { throw "V6 compilation failed" }
    Write-Host "Built: $ProjectDir\estimate_v6.exe"
} finally {
    Pop-Location
}
