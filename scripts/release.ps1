#Requires -Version 7.0
[CmdletBinding()]
param([string]$Version, [switch]$PrepareOnly, [switch]$InteractiveTests, [string]$KeyPath = $env:BOOKS_KEYPATH)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$InteractiveTests) { throw 'Release validation includes visible UI/fullscreen tests. Run on an available desktop with -InteractiveTests to explicitly opt in.' }
$root = Split-Path $PSScriptRoot -Parent
$repository = 'voltura/voltura-books'
$metadataFiles = @('CMakeLists.txt','src/app.manifest','src/app.rc','src/integration.cpp')
$originalMetadata = @{}
$expectedMetadata = @{}
$metadataChanged = $false
$versionCommitCreated = $false
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE). Publication stopped." }
}
function Phase([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Assert-ExactText([string]$Path, [string]$Expected, [string]$Label) {
    $text = [IO.File]::ReadAllText((Join-Path $root $Path))
    $count = [regex]::Matches($text, [regex]::Escape($Expected)).Count
    if ($count -ne 1) { throw "$Label must occur exactly once in $Path; found $count." }
}
function Assert-VersionMetadata([string]$Value) {
    $numeric = $Value.Replace('.', ',') + ',0'
    Assert-ExactText 'CMakeLists.txt' "project(VolturaBooks VERSION $Value LANGUAGES C CXX RC)" 'CMake project version'
    Assert-ExactText 'src/app.manifest' "assemblyIdentity version=`"$Value.0`"" 'Manifest version'
    Assert-ExactText 'src/app.rc' "FILEVERSION $numeric" 'FILEVERSION'
    Assert-ExactText 'src/app.rc' "PRODUCTVERSION $numeric" 'PRODUCTVERSION'
    Assert-ExactText 'src/app.rc' "`"FileVersion`", `"$Value`"" 'FileVersion string'
    Assert-ExactText 'src/app.rc' "`"ProductVersion`", `"$Value`"" 'ProductVersion string'
    Assert-ExactText 'src/integration.cpp' "L`"DisplayVersion`", L`"$Value`"" 'Installed-app version'
}
function Replace-ExactText([string]$Path, [string]$Old, [string]$New, [string]$Label) {
    Assert-ExactText $Path $Old $Label
    $fullPath = Join-Path $root $Path
    $bytes = [IO.File]::ReadAllBytes($fullPath)
    $hasUtf8Bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf
    $text = [IO.File]::ReadAllText($fullPath)
    [IO.File]::WriteAllText($fullPath, $text.Replace($Old, $New), [Text.UTF8Encoding]::new($hasUtf8Bom))
}
function Set-VersionMetadata([string]$Old, [string]$New) {
    $oldNumeric = $Old.Replace('.', ',') + ',0'
    $newNumeric = $New.Replace('.', ',') + ',0'
    Replace-ExactText 'CMakeLists.txt' "project(VolturaBooks VERSION $Old LANGUAGES C CXX RC)" "project(VolturaBooks VERSION $New LANGUAGES C CXX RC)" 'CMake project version'
    Replace-ExactText 'src/app.manifest' "assemblyIdentity version=`"$Old.0`"" "assemblyIdentity version=`"$New.0`"" 'Manifest version'
    Replace-ExactText 'src/app.rc' "FILEVERSION $oldNumeric" "FILEVERSION $newNumeric" 'FILEVERSION'
    Replace-ExactText 'src/app.rc' "PRODUCTVERSION $oldNumeric" "PRODUCTVERSION $newNumeric" 'PRODUCTVERSION'
    Replace-ExactText 'src/app.rc' "`"FileVersion`", `"$Old`"" "`"FileVersion`", `"$New`"" 'FileVersion string'
    Replace-ExactText 'src/app.rc' "`"ProductVersion`", `"$Old`"" "`"ProductVersion`", `"$New`"" 'ProductVersion string'
    Replace-ExactText 'src/integration.cpp' "L`"DisplayVersion`", L`"$Old`"" "L`"DisplayVersion`", L`"$New`"" 'Installed-app version'
}
function Compare-StableVersion([string]$Left, [string]$Right) {
    $leftParts = $Left.Split('.') | ForEach-Object { [Numerics.BigInteger]::Parse($_) }
    $rightParts = $Right.Split('.') | ForEach-Object { [Numerics.BigInteger]::Parse($_) }
    for ($index = 0; $index -lt 3; $index++) {
        if ($leftParts[$index] -lt $rightParts[$index]) { return -1 }
        if ($leftParts[$index] -gt $rightParts[$index]) { return 1 }
    }
    return 0
}
function Assert-SamePaths([object[]]$Actual, [string[]]$Expected, [string]$Message) {
    $actualPaths = @($Actual | Where-Object { $_ } | Sort-Object -Unique)
    $expectedPaths = @($Expected | Sort-Object -Unique)
    if (@(Compare-Object $actualPaths $expectedPaths).Count -ne 0) { throw $Message }
}
Push-Location $root
try {
    Phase 'Validate version and release notes'
    $cmake = Get-Content CMakeLists.txt -Raw
    if ($cmake -notmatch 'project\(VolturaBooks VERSION (\d+\.\d+\.\d+)') { throw 'CMake version missing.' }
    $current = $Matches[1]
    if (!$Version) { $Version = $current }
    if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Version must be a stable X.Y.Z value.' }
    Assert-VersionMetadata $current
    $notes = Join-Path $root "docs/releases/$Version.md"
    $notesRelative = "docs/releases/$Version.md"
    if (!(Test-Path $notes)) { throw "Write release notes in $notes first." }
    $body = (Get-Content $notes -Raw) -replace '(?m)^#.*$', '' -replace '(?s)<!--.*?-->', ''
    if ($body -notmatch '\p{L}' -or $body -match '\b(TODO|TBD|FIXME)\b') { throw 'Complete the release notes first.' }
    if (!$KeyPath -or !(Test-Path -LiteralPath $KeyPath)) { throw 'Set BOOKS_KEYPATH to the encrypted Books signing key.' }
    $bumpRequired = $false
    if (!$PrepareOnly) {
        Invoke-Checked git @('rev-parse','--show-toplevel')
        $branch = Invoke-Checked git @('branch','--show-current')
        if ($branch -ne 'main') { throw 'Publish from main.' }
        if (Invoke-Checked git @('status','--porcelain','--untracked-files=all')) { throw 'Review and commit changes before publishing.' }
        $trackedNotes = Invoke-Checked git @('ls-tree','--name-only','HEAD','--',$notesRelative)
        if ($trackedNotes -ne $notesRelative) { throw "Commit $notesRelative before publishing." }
        $origin = Invoke-Checked git @('remote','get-url','origin')
        if ($origin -notin @("https://github.com/$repository.git", "https://github.com/$repository", "git@github.com:$repository.git")) { throw 'Unexpected origin.' }
        Invoke-Checked gh @('auth','status')
        $remote = (Invoke-Checked gh @('repo','view',$repository,'--json','visibility') | ConvertFrom-Json)
        if ($remote.visibility -ne 'PUBLIC') { throw 'The release target must be the public repository.' }
        $releases = @(Invoke-Checked gh @('api',"repos/$repository/releases",'--paginate','--jq','.[].tag_name'))
        if ($releases -contains "v$Version") { throw 'This release already exists. Inspect it before retrying.' }
        if (Invoke-Checked git @('ls-remote','--tags','origin',"refs/tags/v$Version")) { throw 'The release tag already exists.' }
        $baselineRevision = Invoke-Checked git @('rev-parse','HEAD')
        $comparison = Compare-StableVersion $Version $current
        if ($comparison -lt 0) { throw "Version $Version is lower than the checked-in version $current." }
        $bumpRequired = $comparison -gt 0
        if ($bumpRequired) {
            foreach ($file in $metadataFiles) { $originalMetadata[$file] = [IO.File]::ReadAllBytes((Join-Path $root $file)) }
            $metadataChanged = $true
            Set-VersionMetadata $current $Version
            Assert-VersionMetadata $Version
            foreach ($file in $metadataFiles) { $expectedMetadata[$file] = [IO.File]::ReadAllBytes((Join-Path $root $file)) }
        }
    } elseif ($Version -ne $current) {
        throw 'Prepare-only uses the version already checked into CMakeLists.txt and the other metadata files.'
    }
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
    if ((Invoke-Checked git @('rev-parse','HEAD')) -ne $baselineRevision) { throw 'HEAD changed during preparation.' }
    $unstaged = @(Invoke-Checked git @('diff','--name-only'))
    $staged = @(Invoke-Checked git @('diff','--cached','--name-only'))
    $untracked = @(Invoke-Checked git @('ls-files','--others','--exclude-standard'))
    if ($bumpRequired) {
        Assert-SamePaths $unstaged $metadataFiles 'Files other than the release metadata changed during preparation.'
        Assert-SamePaths $staged @() 'Files were staged during preparation.'
        Assert-SamePaths $untracked @() 'Untracked files appeared during preparation.'
        Assert-VersionMetadata $Version
        foreach ($file in $metadataFiles) {
            $actual = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $root $file)))
            $expected = [Convert]::ToBase64String($expectedMetadata[$file])
            if ($actual -ne $expected) { throw "$file changed after the release version was generated." }
        }
        Phase 'Commit release metadata'
        Invoke-Checked git (@('add','--') + $metadataFiles)
        Assert-SamePaths @(Invoke-Checked git @('diff','--name-only')) @() 'Release metadata was not staged cleanly.'
        Assert-SamePaths @(Invoke-Checked git @('diff','--cached','--name-only')) $metadataFiles 'The release commit would contain unexpected files.'
        Invoke-Checked git @('commit','-m',"Release Voltura Books $Version")
        $versionCommitCreated = $true
        $metadataChanged = $false
    } else {
        Assert-SamePaths $unstaged @() 'Source changed during preparation.'
        Assert-SamePaths $staged @() 'Files were staged during preparation.'
        Assert-SamePaths $untracked @() 'Untracked files appeared during preparation.'
    }
    if (Invoke-Checked git @('status','--porcelain','--untracked-files=all')) { throw 'The release source is not clean after version preparation.' }
    $revision = Invoke-Checked git @('rev-parse','HEAD')
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
} finally {
    if ($metadataChanged -and !$versionCommitCreated) {
        & git restore --staged -- @metadataFiles 2>$null
        foreach ($file in $metadataFiles) { [IO.File]::WriteAllBytes((Join-Path $root $file), $originalMetadata[$file]) }
    }
    Pop-Location
}
