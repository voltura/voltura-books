param([int]$CleanupPid = 0)
$ErrorActionPreference = 'Stop'
$expected = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs/Voltura Books'))
if (!$CleanupPid) {
    $executable = Join-Path $expected 'VolturaBooks.exe'
    if (!(Test-Path -LiteralPath $executable)) { throw 'Voltura Books is not installed.' }
    $process = Start-Process -FilePath $executable -ArgumentList '--uninstall' -Wait -PassThru
    if ($process.ExitCode) { throw 'Uninstall failed.' }
    return
}
# Only the installed helper may clean up files, and only known package files.
if ([IO.Path]::GetFullPath($PSScriptRoot) -ne $expected) { throw 'Unexpected installation directory; refusing cleanup.' }
$running = Get-Process -Id $CleanupPid -ErrorAction SilentlyContinue
if ($running) { $running.WaitForExit(30000) | Out-Null; if (!$running.HasExited) { throw 'Application did not exit.' } }
foreach ($name in @('VolturaBooks.exe', 'VolturaBooksReader.exe','VolturaBooksDoc.exe','DocSharp.Binary.Doc.dll','DocSharp.Binary.Common.dll','System.IO.Compression.dll', 'THIRD-PARTY-NOTICES.txt', 'README.md', 'LICENSE', 'uninstall.ps1')) {
    $target = [IO.Path]::GetFullPath((Join-Path $expected $name))
    if ([IO.Path]::GetDirectoryName($target) -ne $expected) { throw 'Unexpected cleanup path.' }
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
}
if ((Get-ChildItem -LiteralPath $expected -Force | Measure-Object).Count -eq 0) { Remove-Item -LiteralPath $expected }
