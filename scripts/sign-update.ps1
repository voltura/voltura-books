#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Directory,
    [string]$KeyPath = $env:BOOKS_KEYPATH
)
$ErrorActionPreference = 'Stop'
if (!$KeyPath) { throw 'Set BOOKS_KEYPATH to the encrypted Books signing key.' }
$root = Split-Path $PSScriptRoot -Parent
$rsa = [Security.Cryptography.RSA]::Create()
$passphrase = $null
try {
    if ($env:BOOKS_UPDATE_SIGNING_PASSPHRASE) {
        $passphrase = ConvertTo-SecureString $env:BOOKS_UPDATE_SIGNING_PASSPHRASE -AsPlainText -Force
    } elseif (Test-Path -LiteralPath ($KeyPath + '.passphrase.xml')) {
        $passphrase = Import-Clixml -LiteralPath ($KeyPath + '.passphrase.xml')
    } else { throw 'Set BOOKS_UPDATE_SIGNING_PASSPHRASE to unlock the signing key.' }
    $credential = [Net.NetworkCredential]::new('', $passphrase)
    $rsa.ImportFromEncryptedPem([IO.File]::ReadAllText($KeyPath), $credential.Password)
    if ($rsa.ExportSubjectPublicKeyInfoPem().Trim() -ne (Get-Content "$root/src/update-signing-public.pem" -Raw).Trim()) { throw 'Signing key does not match the app public key.' }
    $name = "VolturaBooks-Setup-$Version-win-x64.exe"
    $installer = Get-Item -LiteralPath (Join-Path $Directory $name)
    $manifest = [ordered]@{ schema=1; version=$Version; assets=@([ordered]@{ name=$name; size=$installer.Length; sha256=(Get-FileHash $installer.FullName).Hash.ToLowerInvariant() }) }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($manifest | ConvertTo-Json -Depth 4 -Compress))
    $signature = $rsa.SignData($bytes,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pss)
    [IO.File]::WriteAllBytes((Join-Path $Directory "VolturaBooks-Update-$Version.json"),$bytes)
    [IO.File]::WriteAllBytes((Join-Path $Directory "VolturaBooks-Update-$Version.sig"),$signature)
} finally { $rsa.Dispose(); if($passphrase){$passphrase.Dispose()}; $credential=$null }
