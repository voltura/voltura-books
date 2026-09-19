#Requires -Version 7.0
[CmdletBinding()]
param([string]$Version, [switch]$PrepareOnly, [switch]$InteractiveTests, [string]$KeyPath = $env:BOOKS_KEYPATH)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$InteractiveTests) { throw 'Release validation includes visible UI/fullscreen tests. Run on an available desktop with -InteractiveTests to explicitly opt in.' }
$root = Split-Path $PSScriptRoot -Parent
$repository = 'voltura/voltura-books'
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE). Publication stopped." }
}
function Phase([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
Push-Location $root
try {
    Phase 'Validate version and release notes'
    $cmake = Get-Content CMakeLists.txt -Raw
    if ($cmake -notmatch 'project\(VolturaBooks VERSION (\d+\.\d+\.\d+)') { throw 'CMake version missing.' }
    $current = $Matches[1]
    if (!$Version) { $Version = $current }
    if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or $Version -ne $current) {
        throw 'Use the version in CMakeLists.txt. Update all version metadata before releasing another version.'
    }
    $rc = Get-Content src/app.rc -Raw
    $numeric = $Version.Replace('.', ',') + ',0'
    foreach ($expected in @("FILEVERSION $numeric", "PRODUCTVERSION $numeric", "`"FileVersion`", `"$Version`"", "`"ProductVersion`", `"$Version`"")) {
        if (!$rc.Contains($expected)) { throw "Resource version mismatch: $expected" }
    }
    if (!(Get-Content src/app.manifest -Raw).Contains("assemblyIdentity version=`"$Version.0`"")) { throw 'Manifest version mismatch.' }
    if (!(Get-Content src/integration.cpp -Raw).Contains("L`"DisplayVersion`", L`"$Version`"")) { throw 'Installed-app version mismatch.' }
    $notes = Join-Path $root "docs/releases/$Version.md"
    if (!(Test-Path $notes)) { throw "Write release notes in $notes first." }
    $body = (Get-Content $notes -Raw) -replace '(?m)^#.*$', '' -replace '(?s)<!--.*?-->', ''
    if ($body -notmatch '\p{L}' -or $body -match '\b(TODO|TBD|FIXME)\b') { throw 'Complete the release notes first.' }
    if (!$PrepareOnly) {
        Invoke-Checked git @('rev-parse','--show-toplevel')
        $branch = Invoke-Checked git @('branch','--show-current')
        if ($branch -ne 'main') { throw 'Publish from main.' }
        if (Invoke-Checked git @('status','--porcelain','--untracked-files=all')) { throw 'Review and commit changes before publishing.' }
        $origin = Invoke-Checked git @('remote','get-url','origin')
        if ($origin -notin @("https://github.com/$repository.git", "https://github.com/$repository", "git@github.com:$repository.git")) { throw 'Unexpected origin.' }
        Invoke-Checked gh @('auth','status')
        $remote = (Invoke-Checked gh @('repo','view',$repository,'--json','visibility') | ConvertFrom-Json)
        if ($remote.visibility -ne 'PUBLIC') { throw 'The release target must be the public repository.' }
        $releases = @(Invoke-Checked gh @('api',"repos/$repository/releases",'--paginate','--jq','.[].tag_name'))
        if ($releases -contains "v$Version") { throw 'This release already exists. Inspect it before retrying.' }
        if (Invoke-Checked git @('ls-remote','--tags','origin',"refs/tags/v$Version")) { throw 'The release tag already exists.' }
        $revision = Invoke-Checked git @('rev-parse','HEAD')
    }
    if (!$KeyPath -or !(Test-Path -LiteralPath $KeyPath)) { throw 'Set BOOKS_KEYPATH to the encrypted Books signing key.' }
    Phase 'Build and test'
    & "$PSScriptRoot/build.ps1" -Test core,drop,mail_setup,settings_cleanup,history,rtf_fit,docx_text,doc_conversion,doc_format,reader_loading,reader_bridge,reader_native_loading,instance_forwarding
    foreach ($test in @('browser','queue','edit_layout','shell_selection')) { & "$PSScriptRoot/test-ui.ps1" -Test $test }
    & "$PSScriptRoot/test-ui.ps1" -Test reader -ReaderFormat all
    Invoke-Checked python @('tests/smtp_fixture.py')
    Invoke-Checked python @('tests/cover_fixture.py')
    Phase 'Package'
    & "$PSScriptRoot/build-installer.ps1" -SkipBuild
    $output = Join-Path $root "dist/releases/$Version"
    if (Test-Path $output) { throw "Output exists: $output. Move it aside before preparing again." }
    New-Item -ItemType Directory -Path $output | Out-Null
    $installer = Join-Path $output "VolturaBooks-Setup-$Version-win-x64.exe"
    Copy-Item dist/VolturaBooks-Setup.exe $installer
    $zip = Join-Path $output "VolturaBooks-$Version-win-x64.zip"
    $files = @('VolturaBooks.exe','VolturaBooksReader.exe','VolturaBooksDoc.exe','DocSharp.Binary.Doc.dll','DocSharp.Binary.Common.dll','System.IO.Compression.dll','README.md','LICENSE','THIRD-PARTY-NOTICES.txt','install.ps1','uninstall.ps1') | ForEach-Object { Join-Path "$root/dist" $_ }
    Compress-Archive -LiteralPath $files -DestinationPath $zip
    & "$PSScriptRoot/sign-update.ps1" -Version $Version -Directory $output -KeyPath $KeyPath
    $manifest = Join-Path $output "VolturaBooks-Update-$Version.json"
    $signature = Join-Path $output "VolturaBooks-Update-$Version.sig"
    Invoke-Checked "$root/build/Release/update_tests.exe" @($manifest,$signature,"$root/src/update-signing-public.pem")
    $checksums = Join-Path $output SHA256SUMS.txt
    @($installer,$zip,$manifest,$signature) | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_ -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path $_ -Leaf) } | Set-Content $checksums -Encoding utf8
    if ($PrepareOnly) { Write-Host "SUCCESS: prepared $output. Nothing published." -ForegroundColor Green; return }
    if (Invoke-Checked git @('status','--porcelain','--untracked-files=all')) { throw 'Source changed during preparation.' }
    if ((Invoke-Checked git @('rev-parse','HEAD')) -ne $revision) { throw 'HEAD changed during preparation.' }
    Phase 'Push source and create draft release'
    Invoke-Checked git @('push','origin','HEAD:refs/heads/main')
    Invoke-Checked gh @('api',"repos/$repository/commits/$revision",'--silent')
    Invoke-Checked gh @('release','create',"v$Version",$installer,$zip,$manifest,$signature,$checksums,'--repo',$repository,'--target',$revision,'--draft','--title',"Voltura Books $Version",'--notes-file',$notes)
    Phase 'Verify uploaded files and publish'
    $download = Join-Path $output 'verified-download'
    Invoke-Checked gh @('release','download',"v$Version",'--repo',$repository,'--dir',$download)
    foreach ($asset in @($installer,$zip,$manifest,$signature,$checksums)) {
        $downloaded = Join-Path $download (Split-Path $asset -Leaf)
        if ((Get-FileHash $asset).Hash -ne (Get-FileHash $downloaded).Hash) { throw 'Uploaded asset mismatch. Release remains a draft.' }
    }
    Invoke-Checked gh @('release','edit',"v$Version",'--repo',$repository,'--draft=false','--latest')
    $latest = Invoke-Checked gh @('api',"repos/$repository/releases/latest",'--jq','.tag_name')
    if ($latest -ne "v$Version") { throw 'Could not confirm Latest release. Inspect GitHub before retrying.' }
    Write-Host "SUCCESS: https://github.com/$repository/releases/tag/v$Version" -ForegroundColor Green
} finally { Pop-Location }
