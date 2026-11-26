param(
    [Parameter(Mandatory=$true)][string]$CarDevice,
    [Parameter(Mandatory=$true)][string]$HostDevice,
    [string]$ExpectedCarProduct = 'rk3399_mid',
    [string]$ExpectedHostProduct = 'petrel-p1',
    [int]$ControlPort = 49152,
    [int]$Cycles = 3
)

$ErrorActionPreference = 'Stop'
$adb = (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe')
$base = "http://127.0.0.1:$ControlPort"

function Connect-Verified([string]$Device, [string]$ExpectedProduct) {
    & $adb connect $Device | Out-Null
    & $adb -s $Device wait-for-device
    if ($LASTEXITCODE -ne 0) { throw "device unavailable: $Device" }
    $product = (& $adb -s $Device shell getprop ro.product.device).Trim()
    if ($product -ne $ExpectedProduct) {
        throw "device identity mismatch: $Device is '$product', expected '$ExpectedProduct'"
    }
}

function Get-DaemonMetrics {
    $raw = (& $adb -s $CarDevice shell 'p=$(pidof iap2d | cut -d " " -f 1); fds=$(ls /proc/$p/fd 2>/dev/null | wc -l); threads=$(sed -n "s/^Threads:[[:space:]]*//p" /proc/$p/status | head -n 1); rss=$(sed -n "s/^VmRSS:[[:space:]]*//p" /proc/$p/status | head -n 1); rss=${rss%% *}; printf "%s %s %s %s" "$p" "$fds" "$threads" "$rss"').Trim()
    $parts = $raw.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
    if ($parts.Count -ne 4) { throw 'unable to read daemon metrics' }
    [pscustomobject]@{ Pid=[int]$parts[0]; Fds=[int]$parts[1]; Threads=[int]$parts[2]; RssKb=[int]$parts[3] }
}

function Get-Status {
    & $adb -s $CarDevice forward --remove "tcp:$ControlPort" 2>$null | Out-Null
    & $adb -s $CarDevice forward "tcp:$ControlPort" "tcp:$ControlPort" | Out-Null
    Invoke-RestMethod "$base/api/status" -NoProxy -TimeoutSec 2
}

Connect-Verified $CarDevice $ExpectedCarProduct
Connect-Verified $HostDevice $ExpectedHostProduct
if ((& $adb -s $HostDevice shell id -u).Trim() -ne '0') { throw 'root is required on the USB host device' }
$initial = Get-Status
$deadline = [DateTime]::UtcNow.AddSeconds(45)
while ($initial.state -ne 'carplay') {
    if ([DateTime]::UtcNow -ge $deadline) { throw "CarPlay did not become ready before host USB cycles: $($initial.state)" }
    Start-Sleep -Seconds 1
    $initial = Get-Status
}
$initialMetrics = Get-DaemonMetrics
$baselineGeneration = [uint64]$initial.generation

for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    $before = Get-DaemonMetrics
    $started = [DateTime]::UtcNow
    & $adb -s $HostDevice shell 'cd /sys/bus/platform/drivers/sunxi-ehci && echo 5101000.ehci0-controller > unbind && sleep 2 && echo 5101000.ehci0-controller > bind' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "host controller cycle failed: $cycle" }
    $status = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $candidate = Get-Status
            if ($candidate.state -eq 'carplay' -and [uint64]$candidate.generation -gt $baselineGeneration) {
                $status = $candidate
                break
            }
        } catch {}
        Start-Sleep -Seconds 1
    }
    $after = Get-DaemonMetrics
    if (-not $status) { throw "USB did not recover in cycle $cycle" }
    if ($after.Pid -ne $before.Pid) { throw "daemon PID changed in USB cycle $cycle" }
    $elapsed = [math]::Round(([DateTime]::UtcNow - $started).TotalSeconds, 1)
    Write-Host ("cycle={0} elapsed={1}s gen={2}..{3} pid={4}..{5} fds={6} threads={7} rssKb={8} audio={9}" -f $cycle,$elapsed,$baselineGeneration,$status.generation,$before.Pid,$after.Pid,$after.Fds,$after.Threads,$after.RssKb,$status.audio.streams)
    $baselineGeneration = [uint64]$status.generation
}
Write-Host "Host-controller USB cycle test passed ($Cycles cycles); daemon PID remained stable."