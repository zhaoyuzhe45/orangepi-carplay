param(
    [string]$Device = '192.168.2.67:5555',
    [string]$ExpectedProduct = 'rk3399_mid'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sdkRoot = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { $env:ANDROID_SDK_ROOT }
$clang = Join-Path $sdkRoot 'ndk\22.1.7171670\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
$adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
$output = Join-Path $projectRoot 'tests\build\input_injection_probe-aarch64'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
& $clang --target=aarch64-linux-android21 -std=c11 -O2 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'src')" `
    (Join-Path $PSScriptRoot '..\input_injection_probe.c') `
    (Join-Path $projectRoot 'src\airplay_uinput.c') -o $output
if ($LASTEXITCODE -ne 0) { throw 'Unable to build input injection probe.' }

& $adb connect $Device | Out-Host
& $adb -s $Device wait-for-device
$product = (& $adb -s $Device shell getprop ro.product.device).Trim()
if ($product -ne $ExpectedProduct) {
    throw "Device identity mismatch: '$product' != '$ExpectedProduct'"
}
& $adb -s $Device shell input keyevent HOME
Start-Sleep -Milliseconds 800
& $adb -s $Device push $output /data/local/tmp/carplay-input-probe | Out-Host
& $adb -s $Device shell chmod 755 /data/local/tmp/carplay-input-probe
$xml = & $adb -s $Device shell 'su 0 uiautomator dump /data/local/tmp/input.xml >/dev/null; su 0 cat /data/local/tmp/input.xml'
$match = [regex]::Match(($xml -join ''), 'text="通讯录"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"')
if (-not $match.Success) { throw 'Contacts launcher target was not found.' }
$x = ([int]$match.Groups[1].Value + [int]$match.Groups[3].Value) / 2
$y = ([int]$match.Groups[2].Value + [int]$match.Groups[4].Value) / 2
& $adb -s $Device shell "su 0 /data/local/tmp/carplay-input-probe $([int]$x) $([int]$y)"
if ($LASTEXITCODE -ne 0) { throw 'Production uinput backend could not inject touch.' }
Start-Sleep -Milliseconds 800
$focus = & $adb -s $Device shell 'dumpsys activity activities | grep mResumedActivity | tail -n 1'
if (($focus -join '') -notmatch 'com\.android\.contacts') {
    throw "Touch was not delivered to the launcher: $focus"
}
Write-Host 'Device touch injection passed.'
