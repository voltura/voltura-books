param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
if (!$SkipBuild) { & "$PSScriptRoot/build.ps1" }
$compiler = Get-Command makensis.exe -ErrorAction SilentlyContinue
if (!$compiler) { throw 'Install NSIS 3 and add makensis.exe to PATH to build the setup executable.' }
$source = Get-Content (Join-Path $PSScriptRoot '../CMakeLists.txt') -Raw
if ($source -notmatch 'project\(VolturaBooks VERSION (\d+\.\d+\.\d+)') { throw 'Project version missing.' }
$releaseVersion = $Matches[1]
& $compiler.Source /WX "/DVERSION=$releaseVersion" "$PSScriptRoot/installer.nsi"
if ($LASTEXITCODE) { throw 'Installer build failed.' }
Write-Host "Installer ready: $(Join-Path $PSScriptRoot '../dist/VolturaBooks-Setup.exe')"
