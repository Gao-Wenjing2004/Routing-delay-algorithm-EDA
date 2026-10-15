param(
  [Parameter(Mandatory=$true)][string]$Executable,
  [Parameter(Mandatory=$true)][string]$Zip
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$exePath=[IO.Path]::GetFullPath($Executable)
$zipPath=[IO.Path]::GetFullPath($Zip)
if([IO.File]::Exists($zipPath)){[IO.File]::Delete($zipPath)}
$archive=[IO.Compression.ZipFile]::Open($zipPath,[IO.Compression.ZipArchiveMode]::Create)
try{
  $entry=$archive.CreateEntry('bin/estimate',[IO.Compression.CompressionLevel]::Optimal)
  $attr=[Convert]::ToUInt32('81ED0000',16)
  $entry.ExternalAttributes=[BitConverter]::ToInt32([BitConverter]::GetBytes($attr),0)
  $src=[IO.File]::OpenRead($exePath);$dst=$entry.Open()
  try{$src.CopyTo($dst,8MB)}finally{$src.Dispose();$dst.Dispose()}
}finally{$archive.Dispose()}
$archive=[IO.Compression.ZipFile]::OpenRead($zipPath)
try{
  if($archive.Entries.Count-ne 1){throw 'bad entry count'}
  $entry=$archive.Entries[0]
  $external=[BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$entry.ExternalAttributes),0)
  if($entry.FullName-ne 'bin/estimate'-or (($external-shr 16)-band 0x1FF)-ne 493){throw 'bad entry or mode'}
  $s=$entry.Open();try{$b=New-Object byte[] 4;[void]$s.Read($b,0,4);if($b[0]-ne 0x7f-or$b[1]-ne 0x45-or$b[2]-ne 0x4c-or$b[3]-ne 0x46){throw 'not ELF'}}finally{$s.Dispose()}
}finally{$archive.Dispose()}
$h=(Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
Write-Output "zip_bytes=$((Get-Item -LiteralPath $zipPath).Length)"
Write-Output "sha256=$h"
