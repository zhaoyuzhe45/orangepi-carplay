param(
    [Parameter(Mandatory=$true)]
    [string]$Device,
    [ValidateSet('AArch64','ARMv7')]
    [string]$Arch = 'AArch64',
    [string]$ExpectedProduct = '',
    [string]$AudioApk = '',
    [string]$PhoneApk = '',
    [switch]$SkipAudioApp,
    [switch]$InstallBoot,
    [switch]$InstallVehicleGnss,
    [switch]$Build,
    [switch]$RebuildAudio,
    [string]$AudioKeystorePassword = '',
    [string]$AudioAlias = 'CARPLAY'
)
$ErrorActionPreference = 'Stop'
$deploy = Join-Path $PSScriptRoot '..\deploy\opi4\scripts\deploy_carplay.ps1'
& $deploy @PSBoundParameters
