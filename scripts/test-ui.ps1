param(
    [Parameter(Mandatory)][ValidateSet('browser','queue','edit_layout','shell_selection','reader')][string]$Test,
    [ValidateSet('doc','docx','all')][string]$ReaderFormat
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($Test -eq 'reader' -and !$ReaderFormat) { throw 'Specify -ReaderFormat doc, docx, or all. Only run the scope needed for your change.' }
if ($Test -ne 'reader' -and $ReaderFormat) { throw '-ReaderFormat applies only to -Test reader.' }
$previous = $env:BOOKS_RUN_INTERACTIVE_TESTS
Push-Location $repo
try {
    Write-Host "Running interactive test: $Test $ReaderFormat. This opens windows and may change focus/fullscreen."
    $env:BOOKS_RUN_INTERACTIVE_TESTS = '1'
    if ($Test -eq 'reader') {
        & python tests/reader_fixture.py --interactive --format $ReaderFormat
    } else {
        $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
        $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (!$vs) { throw 'Visual Studio C++ tools are required.' }
        $ctest = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'
        $output = @(& $ctest --test-dir build -C Release --output-on-failure --no-tests=error -R "^${Test}$" 2>&1)
        $exitCode = $LASTEXITCODE
        $output | ForEach-Object { Write-Host $_ }
        if ($exitCode -and $Test -eq 'shell_selection' -and ($output -join "`n") -match 'HRESULT:\s*80040154') {
            Write-Warning 'shell_selection requires validation in the signed-in interactive Windows user context; REGDB_E_CLASSNOTREG from this process does not block release.'
            $exitCode = 0
        }
        if ($exitCode) { throw "Interactive test failed ($exitCode)." }
    }
    if ($Test -eq 'reader' -and $LASTEXITCODE) { throw "Interactive test failed ($LASTEXITCODE)." }
} finally {
    $env:BOOKS_RUN_INTERACTIVE_TESTS = $previous
    Pop-Location
}
