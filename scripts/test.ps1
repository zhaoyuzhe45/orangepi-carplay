param(
    [ValidateSet('AArch64','ARMv7','All')]
    [string]$Architecture = 'All',
    [string]$Device = '',
    [string]$ExpectedProduct = '',
    [switch]$NoBuild
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
if ($Device -and $Architecture -eq 'All') {
    throw 'Select AArch64 or ARMv7 when -Device is supplied.'
}

$sourceDir = Join-Path $projectRoot 'src'
$fixtureDir = Join-Path $projectRoot 'tests\fixtures'
$buildDir = Join-Path $projectRoot 'tests\build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$archList = @()
if ($Architecture -eq 'All' -or $Architecture -eq 'AArch64') { $archList += 'AArch64' }
if ($Architecture -eq 'All' -or $Architecture -eq 'ARMv7') { $archList += 'ARMv7' }

function Get-Target([string]$Name) {
    if ($Name -eq 'AArch64') { return 'aarch64-linux-android21' }
    return 'armv7a-linux-androideabi21'
}

function Get-Suffix([string]$Name) {
    if ($Name -eq 'AArch64') { return 'aarch64' }
    return 'armv7'
}

function Invoke-TestBuild {
    param(
        [string]$Arch,
        [string]$Name,
        [string[]]$Sources,
        [string[]]$Definitions,
        [string[]]$Libraries
    )
    $suffix = Get-Suffix $Arch
    $output = Join-Path $buildDir "$Name-$suffix"
    $arguments = @(
        "--target=$(Get-Target $Arch)", '-std=c11', '-O2',
        '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
        "-I$sourceDir", "-I$fixtureDir"
    ) + $Definitions + $Sources + $Libraries + @('-o',$output)
    Write-Host "Building $Name for $Arch"
    & $clang @arguments
    if ($LASTEXITCODE -ne 0) { throw "Test build failed: $Name ($Arch)" }
    return $output
}

$results = @()
foreach ($arch in $archList) {
    $source = {
        param([string]$Name)
        Join-Path $sourceDir $Name
    }
    $testSource = {
        param([string]$Name)
        Join-Path $projectRoot ("tests\" + $Name)
    }

    if (-not $NoBuild) {
        $workspace = @(
            (& $testSource 'airplay_protocol_test.c'),
            (& $source 'airplay_input.c'), (& $source 'airplay_protocol.c'),
            (& $source 'airplay_bplist.c'), (& $source 'airplay_video.c'),
            (& $source 'airplay_h264.c'), (& $source 'airplay_screen_fixture.c'),
            (& $source 'carplay_capabilities.c'), (& $source 'carplay_audio_bridge.c'),
            (& $source 'airplay_crypto.c'), (& $source 'airplay_legacy.c')
        )
        $results += Invoke-TestBuild $arch 'airplay_protocol_test' $workspace @('-DCARPLAY_AUDIO_TESTING','-DAIRPLAY_TEST_VIDEO') @('-lOpenSLES','-llog','-ldl','-pthread','-lm')

        $support = @(
            (& $testSource 'carplay_support_test.c'),
            (& $source 'carplay_config.c'), (& $source 'carplay_log.c'),
            (& $source 'carplay_capabilities.c'), (& $source 'airplay_bplist.c'),
            (& $source 'carplay_vehicle_state.c'), (& $source 'carplay_time_sync.c'),
            (& $source 'usb_vendor.c')
        )
        $results += Invoke-TestBuild $arch 'carplay_support_test' $support @() @('-llog','-ldl','-pthread','-lm')

        $boxRole = @(
            (& $testSource 'box_role_test.c'),
            (& $source 'box_role.c')
        )
        $results += Invoke-TestBuild $arch 'box_role_test' $boxRole @() @('-pthread')

        $display = @(
            (& $testSource 'display_policy_test.c'),
            (& $source 'carplay_display.c')
        )
        $results += Invoke-TestBuild $arch 'display_policy_test' $display @() @('-llog','-pthread')

        $uinputAxis = @(
            (& $testSource 'uinput_axis_frame_test.c'),
            (& $source 'airplay_uinput.c')
        )
        $results += Invoke-TestBuild $arch 'uinput_axis_frame_test' $uinputAxis @() @('-llog','-pthread')

        $crypto = @((& $testSource 'airplay_crypto_selftest.c'), (& $source 'airplay_crypto.c'))
        $results += Invoke-TestBuild $arch 'airplay_crypto_selftest' $crypto @() @('-ldl','-pthread','-lm')

        $lockdown = @((& $testSource 'lockdown_test.c'), (& $source 'lockdown.c'))
        $results += Invoke-TestBuild $arch 'lockdown_test' $lockdown @() @('-ldl','-pthread')

        $usbmux = @((& $testSource 'usbmux_test.c'), (& $source 'usbmux.c'))
        $results += Invoke-TestBuild $arch 'usbmux_test' $usbmux @() @('-ldl','-pthread')

        $ncmStop = @((& $testSource 'ncm_worker_stop_test.c'), (& $source 'usb_watchdog.c'))
        $results += Invoke-TestBuild $arch 'ncm_worker_stop_test' $ncmStop @('-ffunction-sections','-fdata-sections') @('-Wl,--gc-sections','-pthread')

        $ncmPeer = @((& $testSource 'ncm_peer_address_test.c'), (& $source 'usb_watchdog.c'))
        $results += Invoke-TestBuild $arch 'ncm_peer_address_test' $ncmPeer @('-ffunction-sections','-fdata-sections') @('-Wl,--gc-sections','-pthread')
        $ncmReady = @((& $testSource 'ncm_address_ready_test.c'))
        $results += Invoke-TestBuild $arch 'ncm_address_ready_test' $ncmReady @('-ffunction-sections','-fdata-sections') @('-Wl,--gc-sections','-pthread')

        $ncmNtb = @((& $testSource 'ncm_ntb_test.c'), (& $source 'usb_watchdog.c'))
        $results += Invoke-TestBuild $arch 'ncm_ntb_test' $ncmNtb @('-ffunction-sections','-fdata-sections') @('-Wl,--gc-sections','-pthread')

        $watchdog = @((& $testSource 'usb_watchdog_test.c'), (& $source 'usb_watchdog.c'))
        $results += Invoke-TestBuild $arch 'usb_watchdog_test' $watchdog @() @('-pthread')
        $usbRecovery = @((& $testSource 'usbhost_recovery_test.c'))
        $results += Invoke-TestBuild $arch 'usbhost_recovery_test' $usbRecovery @() @()

        $ident = @((& $testSource 'iap2_ident_test.c'), (& $source 'iap2ident.c'))
        $results += Invoke-TestBuild $arch 'iap2_ident_test' $ident @() @('-pthread')

        $linkReliability = @((& $testSource 'iap2_link_reliability_test.c'), (& $source 'iap2link.c'))
        $results += Invoke-TestBuild $arch 'iap2_link_reliability_test' $linkReliability @() @()

        $timing = @((& $testSource 'airplay_timing_test.c'), (& $source 'airplay_timing.c'))
        $results += Invoke-TestBuild $arch 'airplay_timing_test' $timing @() @('-pthread')
        $timingScope = @((& $testSource 'airplay_timing_scope_test.c'))
        $results += Invoke-TestBuild $arch 'airplay_timing_scope_test' $timingScope @() @('-pthread')

        $preset = @((& $testSource 'display_preset_test.c'), (& $source 'carplay_capabilities.c'), (& $source 'airplay_bplist.c'))
        $results += Invoke-TestBuild $arch 'display_preset_test' $preset @() @()
        $preparation = @((& $testSource 'display_preparation_test.c'), (& $source 'carplay_display.c'))
        $results += Invoke-TestBuild $arch 'display_preparation_test' $preparation @() @()
        $knob = @((& $testSource 'carplay_knob_test.c'), (& $source 'carplay_knob.c'))
        $results += Invoke-TestBuild $arch 'carplay_knob_test' $knob @() @()
        $keys = @((& $testSource 'carplay_keys_test.c'), (& $source 'carplay_keys.c'))
        $results += Invoke-TestBuild $arch 'carplay_keys_test' $keys @() @('-pthread')

        $encoder = @((& $testSource 'carplay_encoder_test.c'), (& $source 'carplay_encoder.c'))
        $results += Invoke-TestBuild $arch 'carplay_encoder_test' $encoder @() @()
        $videoRecovery = @((& $testSource 'carplay_video_recovery_test.c'))
        $results += Invoke-TestBuild $arch 'carplay_video_recovery_test' $videoRecovery @() @()

        $session = @(
            (& $testSource 'iap2_session_test.c'),
            (& $source 'iap2session.c'), (& $source 'carplay_vehicle_state.c'), (& $source 'carplay_now_playing.c'), (& $source 'carplay_calls.c'), (& $source 'carplay_route.c'), (& $source 'carplay_file.c')
        )
        $results += Invoke-TestBuild $arch 'iap2_session_test' $session @('-DIAP2_REAL_VEHICLE_STATE','-DCARPLAY_NOW_TEST_PROVIDER','-ffunction-sections','-fdata-sections') @('-Wl,--gc-sections','-pthread')
        $nowPlaying = @((& $testSource 'carplay_now_playing_test.c'), (& $source 'carplay_now_playing.c'), (& $source 'carplay_calls.c'), (& $source 'carplay_route.c'))
        $results += Invoke-TestBuild $arch 'carplay_now_playing_test' $nowPlaying @() @()
        $telephone = @((& $testSource 'carplay_telephone_hid_test.c'), (& $source 'carplay_capabilities.c'), (& $source 'airplay_bplist.c'))
        $results += Invoke-TestBuild $arch 'carplay_telephone_hid_test' $telephone @() @()
        $route = @((& $testSource 'carplay_route_test.c'), (& $source 'carplay_route.c'))
        $results += Invoke-TestBuild $arch 'carplay_route_test' $route @() @()
        $fileTransfer = @((& $testSource 'carplay_file_test.c'), (& $source 'carplay_file.c'))
        $results += Invoke-TestBuild $arch 'carplay_file_test' $fileTransfer @() @()
        $callState = @((& $testSource 'carplay_calls_test.c'), (& $source 'carplay_calls.c'))
        $results += Invoke-TestBuild $arch 'carplay_calls_test' $callState @() @()

        $legacy = @((& $testSource 'airplay_legacy_test.c'), (& $source 'airplay_legacy.c'),
            (& $source 'airplay_video.c'), (& $source 'airplay_crypto.c'), (& $source 'airplay_bplist.c'))
        $results += Invoke-TestBuild $arch 'airplay_legacy_test' $legacy @() @('-ldl','-pthread')
        $modes = @((& $testSource 'carplay_modes_test.c'), (& $source 'carplay_modes.c'), (& $source 'airplay_bplist.c'))
        $results += Invoke-TestBuild $arch 'carplay_modes_test' $modes @() @()
        $mdns = @((& $testSource 'carplay_mdns_test.c'))
        $results += Invoke-TestBuild $arch 'carplay_mdns_test' $mdns @() @('-pthread')
        $audioPause = @((& $testSource 'carplay_audio_pause_test.c'),
            (& $source 'carplay_capabilities.c'), (& $source 'airplay_bplist.c'))
        $results += Invoke-TestBuild $arch 'carplay_audio_pause_test' $audioPause @() @('-pthread')
        $legacyAudio = @((& $testSource 'carplay_legacy_audio_test.c'),
            (& $source 'carplay_audio_bridge.c'), (& $source 'carplay_capabilities.c'),
            (& $source 'airplay_bplist.c'), (& $source 'airplay_crypto.c'))
        $results += Invoke-TestBuild $arch 'carplay_legacy_audio_test' $legacyAudio @('-DCARPLAY_AUDIO_TESTING') @('-lOpenSLES','-llog','-ldl','-pthread','-lm')
        $navAudio = @((& $testSource 'carplay_nav101_test.c')) + $legacyAudio[1..4]
        $results += Invoke-TestBuild $arch 'carplay_nav101_test' $navAudio @('-DCARPLAY_AUDIO_TESTING') @('-lOpenSLES','-llog','-ldl','-pthread','-lm')
        $vehicle = @((& $testSource 'vehicle_services_test.c'), (& $source 'carplay_vehicle_state.c'))
        $results += Invoke-TestBuild $arch 'vehicle_services_test' $vehicle @() @('-pthread')
        $vehicleNight = @((& $testSource 'vehicle_night_worker_test.c'), (& $source 'carplay_vehicle_state.c'))
        $results += Invoke-TestBuild $arch 'vehicle_night_worker_test' $vehicleNight @() @('-pthread')
    }
}

if (-not $NoBuild) {
    $results | ForEach-Object { Get-Item -LiteralPath $_ } |
        Select-Object Name, Length, @{Name='SHA256';Expression={(Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash}} |
        Format-Table -AutoSize
}

if (-not $Device) {
    Write-Host 'All selected test binaries compiled. Supply -Device to execute them on Android.'
    exit 0
}

# adb reports server start-up on stderr; under 'Stop' that is fatal, so fold
# stderr into the stream and let the exit code decide.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $adb start-server 2>&1 | Out-Null
& $adb connect $Device 2>&1 | Out-Host
$connectCode = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($connectCode -ne 0) { throw "adb connect failed: $Device" }
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $adb -s $Device wait-for-device 2>&1 | Out-Host
$waitCode = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($waitCode -ne 0) { throw "adb device unavailable: $Device" }

$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$product = (& $adb -s $Device shell getprop ro.product.device 2>&1).Trim()
$ErrorActionPreference = $prevEap
if (-not $ExpectedProduct) {
    if ($Architecture -eq 'AArch64') { $ExpectedProduct = 'rk3399_mid' }
    else { $ExpectedProduct = 'petrel-p1' }
}
if ($product -ne $ExpectedProduct) {
    throw "Device identity mismatch: '$product' != '$ExpectedProduct'"
}
Write-Host "Verified device identity: $product"

$suffix = Get-Suffix $Architecture
$remote = "/data/local/tmp/carplay-tests-$suffix-$PID"
& $adb -s $Device shell "mkdir -p $remote; chmod 700 $remote"
if ($LASTEXITCODE -ne 0) { throw 'Unable to prepare device test directory.' }

$names = @('airplay_protocol_test','carplay_support_test','box_role_test','display_policy_test','uinput_axis_frame_test','airplay_crypto_selftest','lockdown_test','usbmux_test','ncm_worker_stop_test','ncm_peer_address_test','ncm_address_ready_test','ncm_ntb_test','usb_watchdog_test','usbhost_recovery_test','iap2_ident_test','iap2_link_reliability_test','iap2_session_test','airplay_timing_test','airplay_timing_scope_test','carplay_encoder_test')
$names += @('carplay_video_recovery_test','airplay_legacy_test','carplay_modes_test','carplay_mdns_test','carplay_audio_pause_test')
$names += @('carplay_legacy_audio_test','carplay_nav101_test','vehicle_services_test','vehicle_night_worker_test')
$names += @('carplay_now_playing_test','carplay_calls_test')
$names += 'carplay_telephone_hid_test'
$names += 'carplay_route_test'
$names += 'carplay_file_test'
$names += @('display_preset_test','display_preparation_test','carplay_knob_test','carplay_keys_test')
foreach ($name in $names) {
    $local = Join-Path $buildDir "$name-$suffix"
    if (-not (Test-Path -LiteralPath $local)) { throw "Test binary missing: $local" }
    & $adb -s $Device push $local "$remote/$name" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "adb push failed: $name" }
}
& $adb -s $Device shell "chmod 755 $remote/*"
if ($LASTEXITCODE -ne 0) { throw 'Unable to make test binaries executable.' }

foreach ($name in $names) {
    Write-Host "Running $name on $Device"
    # Test binaries log their progress to stderr, which PowerShell would
    # otherwise treat as a terminating error under 'Stop'.  Capture both
    # streams, print them, and judge by the exit code.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $adb -s $Device shell "$remote/$name" 2>&1
    $testCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($output) { $output | ForEach-Object { Write-Host "$_" } }
    if ($testCode -ne 0) { throw "Device test failed: $name" }
}
Write-Host "All tests passed on $Device"
