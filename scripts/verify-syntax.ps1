$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$failed = 0
foreach ($file in Get-ChildItem -LiteralPath $projectRoot -Filter '*.ps1' -File -Recurse |
                   Where-Object { $_.FullName -notmatch '[\\/](build|tests[\\/]build)[\\/]' }) {
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        $file.FullName, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count -ne 0) {
        $failed++
        Write-Host "FAIL $($file.FullName)" -ForegroundColor Red
        foreach ($error in $errors) { Write-Host "  $($error.Message)" }
    } else {
        Write-Host "OK   $($file.FullName.Substring($projectRoot.Length + 1))"
    }
}
if ($failed -ne 0) { throw "PowerShell syntax verification failed for $failed file(s)." }
Write-Host 'PowerShell syntax verification passed.'
