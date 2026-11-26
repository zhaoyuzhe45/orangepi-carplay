param()
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$entries = @()
foreach ($folder in @('bin','apk','config','scripts')) {
    $folderPath = Join-Path $packageRoot $folder
    if (-not (Test-Path -LiteralPath $folderPath)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $folderPath -File -Recurse | Sort-Object FullName) {
        $relative = $file.FullName.Substring($packageRoot.Length + 1).Replace('\','/')
        if ($relative -eq 'scripts/update-package-hashes.ps1') { continue }
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToUpperInvariant()
        $entries += "$hash  $relative"
    }
}
if ($entries.Count -eq 0) { throw 'No deploy package files found.' }
$manifest = Join-Path $packageRoot 'SHA256SUMS'
# LF only. Set-Content on Windows PowerShell 5.1 writes CRLF, and a manifest with
# CRLF makes `sha256sum -c` fail on the device and in WSL: the trailing carriage
# return becomes part of the file name, so every entry "disappears". WriteAllText
# stores the string verbatim, which is the only way to pin the line ending.
$text = ($entries -join "`n") + "`n"
[System.IO.File]::WriteAllText($manifest, $text, [System.Text.Encoding]::ASCII)
Write-Host "Updated $manifest with $($entries.Count) entries."
