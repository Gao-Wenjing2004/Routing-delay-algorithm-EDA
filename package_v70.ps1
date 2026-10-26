param(
  [string]$Binary = "bin\estimate_v70",
  [string]$Output = "submission_v70_selective_extension.zip"
)

& (Join-Path $PSScriptRoot "package_v49.ps1") -Binary $Binary -Output $Output
