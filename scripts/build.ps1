param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio with Desktop development with C++.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$version = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$generator = switch (([version]$version).Major) {
    17 { 'Visual Studio 17 2022' }
    18 { 'Visual Studio 18 2026' }
    default { throw 'Use Visual Studio 2022 or 2026.' }
}
& $cmake -S $repo -B "$repo/build" -G $generator -A x64
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& $cmake --build "$repo/build" --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
if (!$SkipTests) {
    & $ctest --test-dir "$repo/build" -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
}
& $cmake --install "$repo/build" --config Release --prefix "$repo/dist"
if ($LASTEXITCODE) { throw 'Packaging failed.' }
Write-Host "Package ready: $repo/dist"
