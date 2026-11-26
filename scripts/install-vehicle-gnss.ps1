param([Parameter(Mandatory=$true)][string]$Device, [int]$AdbPort=5038, [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '../deploy/opi4/scripts/install-vehicle-gnss.ps1') @PSBoundParameters
