param(
  [string]$Binary = "bin\estimate_v69",
  [string]$Output = "submission_v69_no_extension.zip"
)

& (Join-Path $PSScriptRoot "package_v49.ps1") -Binary $Binary -Output $Output
