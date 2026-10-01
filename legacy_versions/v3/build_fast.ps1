param(
    [switch]$SkipTrain,
    [switch]$RebuildAtlas
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $ProjectDir
try {
    if (-not $SkipTrain) {
        python .\train_fast_model.py
        if ($LASTEXITCODE -ne 0) { throw "fast model training failed" }
    }

    python .\prepare_local_arch.py
    if ($LASTEXITCODE -ne 0) { throw "local architecture generation failed" }

    $CompilerCommand = Get-Command g++ -ErrorAction SilentlyContinue
    if ($CompilerCommand) {
        $Compiler = $CompilerCommand.Source
    } else {
        $Compiler = "C:\Program Files (x86)\MinGW\bin\g++.exe"
    }
    if (-not (Test-Path -LiteralPath $Compiler)) {
        throw "g++ was not found. Install MinGW or edit build_fast.ps1 with its path."
    }

    & $Compiler -std=c++17 -O3 -DNDEBUG -march=native -Wall -Wextra -pedantic .\generate_local_atlas.cpp -o .\generate_local_atlas.exe
    if ($LASTEXITCODE -ne 0) { throw "local Atlas generator compilation failed" }

    $AtlasNeedsBuild = $RebuildAtlas -or -not (Test-Path -LiteralPath .\local_atlas.bin)
    if (-not $AtlasNeedsBuild) {
        & .\generate_local_atlas.exe --check .\local_atlas.bin
        $AtlasNeedsBuild = $LASTEXITCODE -ne 0
    }
    if ($AtlasNeedsBuild) {
        .\generate_local_atlas.exe .\local_atlas.bin
        if ($LASTEXITCODE -ne 0) { throw "local Atlas generation failed" }
    }

    & $Compiler -std=c++17 -O3 -DNDEBUG -march=native -Wall -Wextra -pedantic .\estimate.cpp -o .\estimate.exe
    if ($LASTEXITCODE -ne 0) { throw "estimate.exe compilation failed" }
    Write-Host "Built: $ProjectDir\estimate.exe"
} finally {
    Pop-Location
}
