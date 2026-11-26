param()
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$shaFile = Join-Path $packageRoot 'SHA256SUMS'
if (-not (Test-Path -LiteralPath $shaFile)) { throw "Checksum manifest missing: $shaFile" }

function Find-SdkRoot {
    $candidates = @()
    if ($env:ANDROID_SDK_ROOT) { $candidates += $env:ANDROID_SDK_ROOT }
    if ($env:ANDROID_HOME) { $candidates += $env:ANDROID_HOME }
    $candidates += $env:ANDROID_SDK_ROOT
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'build-tools'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return ''
}


function Find-JdkRoot {
    $candidates = @()
    if ($env:JAVA_HOME) { $candidates += $env:JAVA_HOME }
    $candidates += $env:JAVA_HOME
    $candidates += $env:JAVA_HOME
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'bin\javac.exe'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return ''
}

$jdkRoot = Find-JdkRoot
if ($jdkRoot) { $env:JAVA_HOME = $jdkRoot }
$failures = 0
$expected = @{}
foreach ($line in Get-Content -LiteralPath $shaFile) {
    if ($line -notmatch '^([0-9A-Fa-f]{64})\s+\*?(.+?)\s*$') { continue }
    $expected[$Matches[2].Replace('/', '\')] = $Matches[1].ToUpperInvariant()
}
if ($expected.Count -eq 0) { throw 'SHA256SUMS contains no entries.' }

foreach ($relative in $expected.Keys | Sort-Object) {
    $full = [System.IO.Path]::GetFullPath((Join-Path $packageRoot $relative))
    $packagePrefix = $packageRoot.TrimEnd('\') + '\'
    if (-not $full.StartsWith($packagePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "FAIL path traversal: $relative" -ForegroundColor Red
        $failures++
        continue
    }
    if (-not (Test-Path -LiteralPath $full)) {
        Write-Host "FAIL missing: $relative" -ForegroundColor Red
        $failures++
        continue
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash.ToUpperInvariant()
    if ($actual -ne $expected[$relative]) {
        Write-Host "FAIL hash: $relative" -ForegroundColor Red
        Write-Host "  expected $($expected[$relative])"
        Write-Host "  actual   $actual"
        $failures++
    } else {
        Write-Host "OK   $relative"
    }
}

$daemon = Join-Path $packageRoot 'bin\iap2d'
if (Test-Path -LiteralPath $daemon) {
    $bytes = [System.IO.File]::ReadAllBytes($daemon)
    if ($bytes.Length -lt 4 -or $bytes[0] -ne 0x7F -or $bytes[1] -ne 0x45 -or
        $bytes[2] -ne 0x4C -or $bytes[3] -ne 0x46) {
        Write-Host 'FAIL bin/iap2d is not an ELF file' -ForegroundColor Red
        $failures++
    }
}

foreach ($relative in @('bin\gnss\android.hardware.gnss@1.0-impl.so', 'bin\gnss\android.hardware.gnss@1.0.so')) {
    $full = Join-Path $packageRoot $relative
    if (-not $expected.ContainsKey($relative) -or -not (Test-Path -LiteralPath $full)) {
        Write-Host "FAIL missing GNSS package entry: $relative" -ForegroundColor Red
        $failures++
        continue
    }
    $bytes = [IO.File]::ReadAllBytes($full)
    if ($bytes.Length -lt 20 -or $bytes[0] -ne 0x7F -or $bytes[1] -ne 0x45 -or
        $bytes[2] -ne 0x4C -or $bytes[3] -ne 0x46 -or $bytes[4] -ne 2 -or
        $bytes[5] -ne 1 -or $bytes[18] -ne 183 -or $bytes[19] -ne 0) {
        Write-Host "FAIL GNSS file must be AArch64 ELF: $relative" -ForegroundColor Red
        $failures++
    } else { Write-Host "OK   AArch64 GNSS ELF: $relative" }
}

$sdkRoot = Find-SdkRoot
$certificates = @{}
$apksigner = if ($sdkRoot) { Join-Path $sdkRoot 'build-tools\30.0.2\apksigner.bat' } else { '' }
if (-not $apksigner -or -not (Test-Path -LiteralPath $apksigner)) {
    throw 'apksigner 30.0.2 is required to verify the paired APK signatures.'
}
foreach ($apkName in @('CarPlayAudio.apk','CarPlayPhone.apk','CarPlayBluetoothHfpOverlay.apk')) {
    $apk = Join-Path $packageRoot "apk\$apkName"
    if (-not (Test-Path -LiteralPath $apk)) {
        Write-Host "FAIL missing required APK: $apkName" -ForegroundColor Red
        $failures++
        continue
    }
    $signature = @(& $apksigner verify --print-certs $apk)
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL APK signature: $apkName" -ForegroundColor Red
        $failures++
        continue
    }
    $digest = @($signature | Where-Object { $_ -match '^Signer #1 certificate SHA-256 digest:' })
    if ($digest.Count -ne 1) { throw "Cannot identify signer: $apkName" }
    $certificates[$apkName] = ($digest[0] -split ':', 2)[1].Trim()
    Write-Host "OK   APK signature: $apkName"
}
if ($certificates['CarPlayAudio.apk'] -ne $certificates['CarPlayPhone.apk']) {
    Write-Host 'FAIL phone and audio APKs must have the same signing certificate' -ForegroundColor Red
    $failures++
} else { Write-Host 'OK   phone/audio signing certificate matches' }

if ($failures -ne 0) { throw "Package verification failed with $failures error(s)." }
Write-Host "Package verification passed: $packageRoot"
