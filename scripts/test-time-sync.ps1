param(
    [string]$Device = '192.168.2.67:5555',
    [string]$ExpectedProduct = 'rk3399_mid',
    [long]$TargetUnixSeconds = 0
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

function Find-SdkRoot {
    $candidates = @()
    if ($env:ANDROID_SDK_ROOT) { $candidates += $env:ANDROID_SDK_ROOT }
    if ($env:ANDROID_HOME) { $candidates += $env:ANDROID_HOME }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'ndk'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'Android SDK not found. Set ANDROID_SDK_ROOT or ANDROID_HOME.'
}

$sdkRoot = Find-SdkRoot
$clang = Join-Path $sdkRoot 'ndk\22.1.7171670\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
$adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
foreach ($tool in @($clang,$adb)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Android tool missing: $tool" }
}
if ($TargetUnixSeconds -eq 0) {
    $TargetUnixSeconds = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}

$buildDir = Join-Path $projectRoot 'tests\build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$binary = Join-Path $buildDir 'time_sync_device_probe-aarch64'
$sources = @(
    (Join-Path $projectRoot 'tests\time_sync_device_probe.c'),
    (Join-Path $projectRoot 'src\carplay_time_sync.c')
)
$compileArgs = @(
    '--target=aarch64-linux-android21', '-std=c11', '-O2',
    '-Wall', '-Wextra', '-Werror',
    "-I$(Join-Path $projectRoot 'src')"
) + $sources + @('-o', $binary)
& $clang @compileArgs
if ($LASTEXITCODE -ne 0) { throw 'Time-sync probe compilation failed.' }

& $adb connect $Device | Out-Host
if ($LASTEXITCODE -ne 0) { throw "adb connect failed: $Device" }
& $adb -s $Device wait-for-device
if ($LASTEXITCODE -ne 0) { throw "adb device unavailable: $Device" }
$product = (& $adb -s $Device shell getprop ro.product.device).Trim()
if ($product -ne $ExpectedProduct) {
    throw "Device identity mismatch: '$product' != '$ExpectedProduct'"
}

$remote = '/data/local/tmp/carplay-time-sync-probe'
try {
    & $adb -s $Device push $binary $remote | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Time-sync probe push failed.' }
    & $adb -s $Device shell chmod 700 $remote
    if ($LASTEXITCODE -ne 0) { throw 'Time-sync probe chmod failed.' }
    & $adb -s $Device shell "su -c '$remote $TargetUnixSeconds'"
    if ($LASTEXITCODE -ne 0) { throw 'Root time-sync probe failed.' }
} finally {
    & $adb -s $Device shell rm -f $remote | Out-Null
}
