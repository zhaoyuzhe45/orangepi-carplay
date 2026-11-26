param(
    [Parameter(Mandatory=$true)][string]$Device,
    [string]$ExpectedProduct = 'rk3399_mid',
    [int]$Port = 49152
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$adbCandidates = @()
if ($env:ANDROID_SDK_ROOT) { $adbCandidates += (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools\adb.exe') }
if ($env:ANDROID_HOME) { $adbCandidates += (Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe') }
$adbCandidates += (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe')
$adb = $adbCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $adb) { throw 'adb.exe not found' }

& $adb connect $Device | Out-Host
if ($LASTEXITCODE -ne 0) { throw "adb connect failed: $Device" }
& $adb -s $Device wait-for-device
if ($LASTEXITCODE -ne 0) { throw "adb device unavailable: $Device" }
$product = (& $adb -s $Device shell getprop ro.product.device).Trim()
if ($product -ne $ExpectedProduct) {
    throw "Device identity mismatch: '$product' != '$ExpectedProduct'"
}

$status = $null
$lastStatus = $null
for ($attempt = 0; $attempt -lt 30 -and -not $status; $attempt++) {
    & $adb -s $Device forward --remove "tcp:$Port" 2>$null | Out-Null
    & $adb -s $Device forward "tcp:$Port" "tcp:$Port" | Out-Null
    try {
        $lastStatus = Invoke-RestMethod "http://127.0.0.1:$Port/api/status" -NoProxy -TimeoutSec 2
        if ($lastStatus.state -eq 'carplay' -and $lastStatus.display.nativeSize) {
            $status = $lastStatus
            break
        }
    } catch {
    }
    Start-Sleep -Seconds 1
}
if (-not $status) {
    $observed = if ($lastStatus) { $lastStatus.state } else { 'unavailable' }
    throw "CarPlay did not recover within 30 seconds on $Device (state=$observed)"
}
$sizeText = (& $adb -s $Device shell wm size) -join "`n"
$densityText = (& $adb -s $Device shell wm density) -join "`n"
$overrideWidth = 0
$overrideHeight = 0
$physicalWidth = 0
$physicalHeight = 0
if ($sizeText -match 'Override size:\s*(\d+)x(\d+)') {
    $overrideWidth = [int]$Matches[1]
    $overrideHeight = [int]$Matches[2]
}
if ($sizeText -match 'Physical size:\s*(\d+)x(\d+)') {
    $physicalWidth = [int]$Matches[1]
    $physicalHeight = [int]$Matches[2]
}
$wmWidth = if ($overrideWidth) { $overrideWidth } else { $physicalWidth }
$wmHeight = if ($overrideHeight) { $overrideHeight } else { $physicalHeight }
$overrideDensity = 0
$physicalDensity = 0
if ($densityText -match 'Override density:\s*(\d+)') { $overrideDensity = [int]$Matches[1] }
if ($densityText -match 'Physical density:\s*(\d+)') { $physicalDensity = [int]$Matches[1] }
$wmDensity = if ($overrideDensity) { $overrideDensity } else { $physicalDensity }

$failures = @()
if ($status.display.nativeSize) {
    if ($status.display.width -ne $wmWidth -or $status.display.height -ne $wmHeight) {
        $failures += "status display $($status.display.width)x$($status.display.height) != wm $($wmWidth)x$wmHeight"
    }
    if ($status.display.density -ne $wmDensity) {
        $failures += "status density $($status.display.density) != wm $wmDensity"
    }
    if ($status.display.error -ne 'ok') {
        $failures += "native display error: $($status.display.error)"
    }
}
if ($status.display.requestedFps -lt 0 -or $status.display.actualFps -gt $status.display.requestedFps) {
    $failures += "invalid fps requested=$($status.display.requestedFps) actual=$($status.display.actualFps)"
}
if ($status.state -eq 'carplay' -and $status.audio.streams -gt 0 -and
    (-not $status.audio.media -or -not $status.audio.microphone)) {
    $failures += 'carplay audio session is active but media or microphone is unavailable'
}

"state=$($status.state) status=$($status.display.width)x$($status.display.height)@$($status.display.density) wm=$($wmWidth)x${wmHeight}@$wmDensity fps=$($status.display.actualFps)/$($status.display.requestedFps) audio=$($status.audio.streams)"
if ($failures.Count) {
    $failures | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    throw "Runtime acceptance failed with $($failures.Count) error(s)."
}
Write-Host 'Runtime acceptance passed.'