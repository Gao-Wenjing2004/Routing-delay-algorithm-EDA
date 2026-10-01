$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $ProjectDir
try {
    $PythonCommand = Get-Command python -ErrorAction SilentlyContinue
    $Python = if ($PythonCommand) { $PythonCommand.Source } else { "C:\Users\ZC\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" }
    if (-not (Test-Path -LiteralPath $Python)) { throw "python was not found" }
    $CompilerCommand = Get-Command g++ -ErrorAction SilentlyContinue
    $Compiler = if ($CompilerCommand) { $CompilerCommand.Source } else { "C:\Program Files (x86)\MinGW\bin\g++.exe" }
    if (-not (Test-Path -LiteralPath $Compiler)) { throw "g++ was not found" }
    $Flags = @('-std=c++17','-O3','-DNDEBUG','-march=native','-Wall','-Wextra','-pedantic','-Wl,--large-address-aware')

    & $Python .\prepare_local_arch.py
    if ($LASTEXITCODE -ne 0) { throw "JSON architecture generation failed" }
    & $Compiler @Flags .\estimate.cpp -o .\estimate.exe
    if ($LASTEXITCODE -ne 0) { throw "estimate.exe compilation failed" }
    Write-Host "Built single-file runtime-preprocessing V5: $ProjectDir\estimate.exe"
} finally { Pop-Location }
