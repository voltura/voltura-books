$ErrorActionPreference = 'Stop'
$executable = Join-Path $PSScriptRoot 'VolturaBooks.exe'
if (!(Test-Path -LiteralPath $executable)) { $executable = Join-Path $PSScriptRoot '../dist/VolturaBooks.exe' }
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the release package first with scripts/build.ps1.' }
$process = Start-Process -FilePath $executable -ArgumentList '--install' -Wait -PassThru
if ($process.ExitCode) { throw 'Installation failed.' }
Write-Host 'Installed. Right-click an EPUB and choose Send to Kindle (Show more options on Windows 11).'
