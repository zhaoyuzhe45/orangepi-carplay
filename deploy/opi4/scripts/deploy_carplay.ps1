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
if ($InstallVehicleGnss -and ($SkipAudioApp -or $Arch -ne 'AArch64')) {
    throw '-InstallVehicleGnss requires the packaged Audio APK and AArch64 firmware.'
}
$packageRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $packageRoot)

function Find-SdkRoot {
    $candidates = @()
    if ($env:ANDROID_SDK_ROOT) { $candidates += $env:ANDROID_SDK_ROOT }
    if ($env:ANDROID_HOME) { $candidates += $env:ANDROID_HOME }
    $candidates += $env:ANDROID_SDK_ROOT
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'platform-tools\adb.exe'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'Android SDK not found. Set ANDROID_SDK_ROOT or ANDROID_HOME.'
}

$sdkRoot = Find-SdkRoot
$adb = Join-Path $sdkRoot 'platform-tools\adb.exe'

function Invoke-Adb {
    param([string[]]$Arguments)
    # adb writes harmless progress noise ("daemon not running; starting now")
    # to stderr.  Under $ErrorActionPreference='Stop' PowerShell turns that
    # into a terminating error and the deploy dies before pushing anything,
    # so run adb with stderr folded into the output and judge by exit code.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $adb @Arguments 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($output) { $output | ForEach-Object { Write-Host "$_" } }
    if ($code -ne 0) {
        throw "adb command failed: adb $($Arguments -join ' ')"
    }
}

# Warm the server once so the first real command does not race its start-up.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $adb start-server 2>&1 | Out-Null
$ErrorActionPreference = $prevEap

$buildDir = Join-Path $projectRoot "build\deploy\$Arch"
if ($Build) {
    $buildScript = Join-Path $projectRoot 'scripts\build.ps1'
    if ($Arch -eq 'AArch64') {
        & $buildScript -Architecture $Arch -OutputDirectory $buildDir -PublishFinal
    } else {
        & $buildScript -Architecture $Arch -OutputDirectory $buildDir
    }
    if ($LASTEXITCODE -ne 0) { throw 'Daemon build failed.' }
}

if ($RebuildAudio -and $SkipAudioApp) {
    throw '-RebuildAudio cannot be combined with -SkipAudioApp.'
}
if ($RebuildAudio) {
    $audioScript = Join-Path $projectRoot 'scripts\build-audio-apk.ps1'
    & $audioScript -StorePassword $AudioKeystorePassword -Alias $AudioAlias
    if ($LASTEXITCODE -ne 0) { throw 'Android audio APK build failed.' }
    $phoneScript = Join-Path $projectRoot 'scripts\build-phone-apk.ps1'
    & $phoneScript -StorePassword $AudioKeystorePassword -Alias $AudioAlias
    if ($LASTEXITCODE -ne 0) { throw 'Android phone APK build failed.' }
}

$daemonName = if ($Arch -eq 'AArch64') { 'iap2d' } else { 'iap2d-armv7' }
if ($Build) {
    $daemon = if ($Arch -eq 'AArch64') { Join-Path $buildDir 'iap2d-aarch64' } else { Join-Path $buildDir 'iap2d-armv7' }
} elseif ($Arch -eq 'AArch64') {
    $daemon = Join-Path $packageRoot 'bin\iap2d'
} else {
    $daemon = Join-Path $packageRoot "bin\$daemonName"
}
if (-not (Test-Path -LiteralPath $daemon)) {
    throw "Daemon artifact not found for $Arch. Rebuild it or provide the packaged AArch64 artifact."
}

$requiredLocal = @(
    $daemon,
    (Join-Path $packageRoot 'scripts\carplay_supervisor.sh'),
    (Join-Path $packageRoot 'scripts\apple_attach.sh'),
    (Join-Path $packageRoot 'scripts\init.carplay.rc'),
    (Join-Path $packageRoot 'scripts\usb_lock.sh'),
    (Join-Path $packageRoot 'scripts\usb_role_switch.sh'),
    (Join-Path $packageRoot 'scripts\apply_display_mode.sh'),
    (Join-Path $packageRoot 'scripts\install_carplay.sh'),
    (Join-Path $packageRoot 'scripts\provision_display_modes.sh'),
    (Join-Path $packageRoot 'scripts\restore_display_modes.sh'),
    (Join-Path $packageRoot 'config\display-modes.tsv'),
    (Join-Path $packageRoot 'config\resolution-white-modes.xml')
)
$encoderJar = if ($Build) { Join-Path $buildDir 'carplay-encoder.jar' } else { Join-Path $packageRoot 'bin\carplay-encoder.jar' }
$requiredLocal += $encoderJar
if (-not $SkipAudioApp) {
    if (-not $AudioApk) { $AudioApk = Join-Path $packageRoot 'apk\CarPlayAudio.apk' }
    if (-not $PhoneApk) { $PhoneApk = Join-Path $packageRoot 'apk\CarPlayPhone.apk' }
    $requiredLocal += $AudioApk
    $requiredLocal += $PhoneApk
    $requiredLocal += (Join-Path $packageRoot 'apk\carplay-audio-default-permissions.xml')
}
foreach ($file in $requiredLocal) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Package file missing: $file" }
}

Write-Host "Artifact SHA256: $((Get-FileHash -Algorithm SHA256 -LiteralPath $daemon).Hash)  $daemon"
Invoke-Adb @('connect',$Device)
Invoke-Adb @('-s',$Device,'wait-for-device')

$uid = (& $adb -s $Device shell id -u).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read the device uid for $Device."
}
if ($uid -ne '0') {
    $rootUid = (& $adb -s $Device shell su 0 id -u).Trim()
    if ($LASTEXITCODE -ne 0 -or $rootUid -ne '0') {
        throw "Device $Device cannot run installation commands as root (uid=$uid, su-uid=$rootUid)."
    }
    Write-Host "Verified root command access through su (adb uid=$uid)"
} else {
    Write-Host 'Verified adb root access (uid=0)'
}
$actualProduct = (& $adb -s $Device shell getprop ro.product.device).Trim()
if (-not $ExpectedProduct) {
    if ($Arch -eq 'AArch64') { $ExpectedProduct = 'rk3399_mid' }
    else { $ExpectedProduct = 'petrel-p1' }
}
if ($actualProduct -ne $ExpectedProduct) {
    throw "Device identity mismatch: $Device is '$actualProduct', expected '$ExpectedProduct'."
}
Write-Host "Verified device identity: $actualProduct"

$stage = '/data/local/tmp/carplay-stage'
if ($uid -eq '0') {
    Invoke-Adb @('-s',$Device,'shell',"rm -rf $stage; mkdir -p $stage; chmod 700 $stage")
} else {
    Invoke-Adb @('-s',$Device,'shell',"su 0 rm -rf $stage")
    Invoke-Adb @('-s',$Device,'shell',"mkdir -p $stage; chmod 700 $stage")
}
Invoke-Adb @('-s',$Device,'push',$daemon,"$stage/iap2d")
Invoke-Adb @('-s',$Device,'push',$encoderJar,"$stage/carplay-encoder.jar")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\carplay_supervisor.sh'),"$stage/carplay_supervisor.sh")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\apple_attach.sh'),"$stage/apple_attach.sh")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\init.carplay.rc'),"$stage/init.carplay.rc")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\usb_lock.sh'),"$stage/usb_lock.sh")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\usb_role_switch.sh'),"$stage/usb_role_switch.sh")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\apply_display_mode.sh'),"$stage/apply_display_mode.sh")
if (-not $SkipAudioApp) {
    Invoke-Adb @('-s',$Device,'push',(Resolve-Path -LiteralPath $AudioApk).Path,"$stage/CarPlayAudio.apk")
    Invoke-Adb @('-s',$Device,'push',(Resolve-Path -LiteralPath $PhoneApk).Path,"$stage/CarPlayPhone.apk")
    Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'apk\carplay-audio-default-permissions.xml'),"$stage/carplay-audio-default-permissions.xml")
}
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\install_carplay.sh'),"$stage/install_carplay.sh")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\provision_display_modes.sh'),"$stage/provision_display_modes.sh")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'scripts\restore_display_modes.sh'),"$stage/restore_display_modes.sh")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'config\display-modes.tsv'),"$stage/display-modes.tsv")
Invoke-Adb @('-s',$Device,'push',(Join-Path $packageRoot 'config\resolution-white-modes.xml'),"$stage/resolution-white-modes.xml")

$bootFlag = if ($InstallBoot) { '1' } else { '0' }
$installShell = if ($uid -eq '0') { 'sh' } else { 'su 0 sh' }
Invoke-Adb @('-s',$Device,'shell',"chmod 755 $stage/install_carplay.sh; $installShell $stage/install_carplay.sh $stage $bootFlag")
if ($InstallBoot) {
    Write-Host 'Boot service installed. Reboot the accessory board to validate automatic startup.'
}
if ($InstallVehicleGnss) {
    $gnssAdbPort = if ($env:ANDROID_ADB_SERVER_PORT) { [int]$env:ANDROID_ADB_SERVER_PORT } else { 5037 }
    & (Join-Path $PSScriptRoot 'install-vehicle-gnss.ps1') -Device $Device -AdbPort $gnssAdbPort
}
Write-Host "Deployment completed on $Device ($Arch)."
