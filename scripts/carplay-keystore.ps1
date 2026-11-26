param(
    [ValidateSet('status', 'copy')]
    [string]$Action = 'status'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$target = 'CarPlayBox/CarPlayAudio/Keystore'
$source = Join-Path $projectRoot 'tools\CarPlayCredentialStore.cs'
if (-not ('CarPlayBox.CredentialStore' -as [type])) { Add-Type -Path $source }
if (-not [CarPlayBox.CredentialStore]::Exists($target)) {
    throw "Keystore credential is not stored: $target"
}
if ($Action -eq 'copy') {
    $password = [CarPlayBox.CredentialStore]::Read($target)
    try { Set-Clipboard -Value $password } finally { $password = $null; [GC]::Collect() }
    Write-Host "Keystore password copied from Windows Credential Manager: $target"
} else {
    Write-Host "Stored: $target"
}
