param(
    [string]$InputCsv = ".\arch\queries.csv",
    [string]$OutputCsv = ".\output\fast_predictions.csv"
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $ProjectDir
try {
    if (-not (Test-Path -LiteralPath .\estimate.exe)) {
        throw "estimate.exe does not exist. Run .\build_fast.ps1 first."
    }
    if (-not (Test-Path -LiteralPath .\local_atlas.bin)) {
        throw "local_atlas.bin does not exist. Run .\build_fast.ps1 first."
    }
    $OutputParent = Split-Path -Parent $OutputCsv
    if ($OutputParent) { New-Item -ItemType Directory -Force -Path $OutputParent | Out-Null }
    .\estimate.exe -in $InputCsv -out $OutputCsv
    if ($LASTEXITCODE -ne 0) { throw "estimate.exe failed" }
    Write-Host "Output: $OutputCsv"
} finally {
    Pop-Location
}
