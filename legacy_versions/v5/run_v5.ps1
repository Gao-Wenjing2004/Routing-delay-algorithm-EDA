param(
    [string]$InputCsv = ".\arch\queries.csv",
    [string]$OutputCsv = ".\output\v5_predictions.csv"
)
$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $ProjectDir
try {
    if (-not (Test-Path -LiteralPath .\estimate.exe)) {
        throw ".\estimate.exe is missing; run build_v5.ps1 first"
    }
    $parent = Split-Path -Parent $OutputCsv
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    .\estimate.exe -in $InputCsv -out $OutputCsv
    if ($LASTEXITCODE -ne 0) { throw "estimate.exe failed" }
    Write-Host "Output: $OutputCsv"
} finally { Pop-Location }
