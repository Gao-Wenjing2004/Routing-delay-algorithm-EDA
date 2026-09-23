param([switch]$LoadImage)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$assetDir = Join-Path $projectRoot 'srb_fast_v7'
$imageTar = Join-Path $projectRoot 'reference_submission\fpga-ziguang-latest.tar'
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'Docker is not installed. The supplied ZIP is already built; rebuilding is optional.'
}
if ($LoadImage) {
    & docker load --input $imageTar
    if ($LASTEXITCODE -ne 0) { throw 'docker load failed' }
}
& docker run --rm --network none `
    --mount "type=bind,source=$PSScriptRoot,target=/workspace" `
    --mount "type=bind,source=$assetDir,target=/srb_fast_v7,readonly" `
    --workdir /workspace fpga-ziguang:latest sh reproduce.sh
if ($LASTEXITCODE -ne 0) { throw 'Build, validation or packaging failed' }
Write-Host "Verified archive: $(Join-Path $PSScriptRoot 'submission.zip')"
