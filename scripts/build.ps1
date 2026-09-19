param(
    [switch]$SkipTests,
    [ValidateSet('core','drop','mail_setup','settings_cleanup','history','rtf_fit','docx_text','doc_conversion','doc_format','reader_loading','reader_bridge','reader_native_loading','instance_forwarding')][string[]]$Test
)
$ErrorActionPreference = 'Stop'
if ($SkipTests -and $Test) { throw 'Use either -SkipTests or -Test, not both.' }
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
if ($Test) {
    $selection = "^(" + (($Test | ForEach-Object { [regex]::Escape($_) }) -join "|") + ")$"
    & $ctest --test-dir "$repo/build" -C Release --output-on-failure --no-tests=error -LE interactive -R $selection
    if ($LASTEXITCODE) { throw 'Tests failed.' }
}
& $cmake --install "$repo/build" --config Release --prefix "$repo/dist"
if ($LASTEXITCODE) { throw 'Packaging failed.' }
Write-Host "Package ready: $repo/dist"
