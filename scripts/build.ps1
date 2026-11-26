param(
    [ValidateSet('AArch64','ARMv7','All')]
    [string]$Architecture = 'All',
    [string]$OutputDirectory = '',
    [switch]$PublishFinal
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
foreach ($identity in @('lockdown_identity.h','lockdown_tls_identity.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot "src/$identity"))) {
        throw 'Local TLS identity is missing. See docs/building.md and scripts/prepare-local-identity.py.'
    }
}

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
if (-not (Test-Path -LiteralPath $clang)) {
    throw "Android NDK r22 clang not found: $clang"
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot 'build\opi4'
}
$OutputDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$sourceNames = @(
    'iap2d.c','iap2link.c','usbmux.c','lockdown.c','lockdown_tls.c',
    'iap2session.c','carplay_now_playing.c','carplay_calls.c','carplay_route.c','carplay_file.c','ncm_bridge.c','airplay_client.c','airplay_input.c','airplay_uinput.c','carplay_knob.c','carplay_keys.c',
    'airplay_crypto.c','airplay_protocol.c','airplay_bplist.c','airplay_timing.c',
    'airplay_legacy.c','carplay_modes.c','carplay_mdns.c',
    'airplay_video.c','airplay_screen_fixture.c','airplay_h264.c','carplay_encoder.c',
    'carplay_capabilities.c','carplay_display.c','carplay_audio_bridge.c',
    'carplay_vehicle_state.c','carplay_time_sync.c','carplay_config.c','carplay_log.c',
    'carplay_control.c','usb_vendor.c','usb_watchdog.c','box_role.c','usbhost.c'
)
$sourceDir = Join-Path $projectRoot 'src'
$sources = @()
foreach ($name in $sourceNames) {
    $source = Join-Path $sourceDir $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Source file missing: $source" }
    $sources += $source
}

if ($PublishFinal -and $Architecture -ne 'AArch64') {
    throw '-PublishFinal is only valid for the OPi4 AArch64 production artifact.'
}

$targets = @()
if ($Architecture -eq 'All' -or $Architecture -eq 'AArch64') {
    $targets += [pscustomobject]@{ Name = 'AArch64'; Target = 'aarch64-linux-android21'; File = 'iap2d-aarch64' }
}
if ($Architecture -eq 'All' -or $Architecture -eq 'ARMv7') {
    $targets += [pscustomobject]@{ Name = 'ARMv7'; Target = 'armv7a-linux-androideabi21'; File = 'iap2d-armv7' }
}

$built = @()
foreach ($item in $targets) {
    $output = Join-Path $OutputDirectory $item.File
    $arguments = @(
        "--target=$($item.Target)", '-std=c11', '-O2',
        '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
        "-I$sourceDir"
    ) + $sources + @(
        '-lOpenSLES', '-llog', '-ldl', '-pthread', '-o', $output
    )
    Write-Host "Building iap2d for $($item.Name) -> $output"
    & $clang @arguments
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $($item.Name)" }
    $built += Get-Item -LiteralPath $output
}

$encoderJar = Join-Path $OutputDirectory 'carplay-encoder.jar'
& (Join-Path $PSScriptRoot 'build-encoder.ps1') -OutputJar $encoderJar
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $encoderJar)) {
    throw 'MediaCodec encoder helper build failed.'
}

if ($PublishFinal) {
    $packageBin = Join-Path $projectRoot 'deploy\opi4\bin'
    New-Item -ItemType Directory -Force -Path $packageBin | Out-Null
    $destination = Join-Path $packageBin 'iap2d'
    Copy-Item -LiteralPath (Join-Path $OutputDirectory 'iap2d-aarch64') -Destination $destination -Force
    Copy-Item -LiteralPath $encoderJar -Destination (Join-Path $packageBin 'carplay-encoder.jar') -Force
    Write-Host "Published OPi4 artifact: $destination"
    & (Join-Path $projectRoot 'deploy\opi4\scripts\update-package-hashes.ps1')
}

$built | Select-Object FullName, Length, @{Name='SHA256';Expression={(Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash}} | Format-Table -AutoSize


