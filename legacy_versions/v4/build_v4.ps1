param(
    [switch]$TrainModel,
    [switch]$BuildV3Parity,
    [switch]$Native
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ProjectDir
Push-Location $ProjectDir
try {
    $PythonCandidates = @()
    if ($env:CONDA_PREFIX) { $PythonCandidates += (Join-Path $env:CONDA_PREFIX 'python.exe') }
    if ($env:USERPROFILE) { $PythonCandidates += (Join-Path $env:USERPROFILE 'anaconda3\python.exe') }
    $PythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($PythonCommand) { $PythonCandidates += $PythonCommand.Source }
    $Python = $PythonCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($TrainModel) {
        if (-not $Python) { throw "Python was not found for V4 model training" }
        & $Python .\train_v4_residual.py `
            --golden ..\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
            --v3-predictions ..\plusone-srb_fast_v3\srb_fast_v3\output\v3_predictions_1m.csv `
            --arch-dir ..\plusone-srb_fast_v3\srb_fast_v3\arch `
            --atlas-file ..\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
            --fixed-splits ..\analysis\srb_fast_v4\v4_fixed_splits.npz
        if ($LASTEXITCODE -ne 0) { throw "V4 residual training failed" }
    }
    if (-not (Test-Path -LiteralPath .\v4_residual_data.hpp)) {
        throw "v4_residual_data.hpp is missing; run with -TrainModel"
    }
    if (-not $Python) { throw "Python was not found for V4 asset verification" }
    & $Python ..\tools\verify_srb_v4_assets.py `
        --manifest .\v4_model_manifest.json `
        --residual-header .\v4_residual_data.hpp `
        --arch-dir ..\plusone-srb_fast_v3\srb_fast_v3\arch `
        --atlas-file ..\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
        --v3-source-dir ..\plusone-srb_fast_v3\srb_fast_v3 `
        --fixed-splits ..\analysis\srb_fast_v4\v4_fixed_splits.npz
    if ($LASTEXITCODE -ne 0) { throw "V4 asset verification failed" }

    $CompilerCommand = Get-Command g++ -ErrorAction SilentlyContinue
    $Compiler = if ($CompilerCommand) { $CompilerCommand.Source } else { $null }
    if (-not $Compiler) {
        $CompilerRoots = @()
        if ($env:CONDA_PREFIX) { $CompilerRoots += $env:CONDA_PREFIX }
        if ($Python) { $CompilerRoots += (Split-Path -Parent $Python) }
        if ($env:USERPROFILE) { $CompilerRoots += (Join-Path $env:USERPROFILE 'anaconda3') }
        if ($env:ProgramData) { $CompilerRoots += (Join-Path $env:ProgramData 'anaconda3') }
        $Candidates = foreach ($Root in ($CompilerRoots | Select-Object -Unique)) {
            Join-Path $Root 'Library\mingw-w64\bin\x86_64-w64-mingw32-g++.exe'
            Join-Path $Root 'Library\mingw-w64\bin\g++.exe'
        }
        $Compiler = $Candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (-not $Compiler) { throw "g++ was not found" }

    $CompilerMajor = [int]((& $Compiler -dumpversion).Split('.')[0])
    $CppStandard = if ($CompilerMajor -lt 7) { '-std=c++1z' } else { '-std=c++17' }
    $Flags = @($CppStandard, '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-pedantic',
               '-static', '-static-libgcc', '-static-libstdc++')
    if ($Native) { $Flags += '-march=native' }
    $OriginalPath = $env:PATH
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
    & $Compiler @Flags .\estimate_v4.cpp -o .\estimate_v4.exe
    if ($LASTEXITCODE -ne 0) { throw "V4 compilation failed" }
    if ($BuildV3Parity) {
        & $Compiler @Flags ..\plusone-srb_fast_v3\srb_fast_v3\estimate.cpp -o .\estimate_v3_parity.exe
        if ($LASTEXITCODE -ne 0) { throw "V3 parity compilation failed" }
    }
    Write-Host "Compiler: $Compiler"
    Write-Host "Built: $ProjectDir\estimate_v4.exe"
    $env:PATH = $OriginalPath
} finally {
    Pop-Location
}
