$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '..\deploy\opi4\scripts\update-package-hashes.ps1')
