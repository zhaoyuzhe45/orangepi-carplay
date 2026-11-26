param(
    [Parameter(Mandatory=$true)]
    [string]$Device,
    [int]$Cycles = 5,
    [string]$ExpectedProduct = 'rk3399_mid',
    [int]$ControlPort = 49152,
    [switch]$KillDaemon
)

$ErrorActionPreference = 'Stop'
$adb = (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe')
$base = "http://127.0.0.1:$ControlPort"

function Invoke-DeviceShell([string]$Command) {
    & $adb -s $Device shell $Command
    if ($LASTEXITCODE -ne 0) { throw "adb shell failed: $Command" }
}

function Update-Forward {
    & $adb -s $Device forward "tcp:$ControlPort" "tcp:$ControlPort" | Out-Null
}

function Wait-ControlReady([int]$Seconds = 20) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            Update-Forward
            $status = Invoke-RestMethod -Uri "$base/api/status" -NoProxy -TimeoutSec 2
            if ($status.state) { return $status }
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    throw 'control service did not become ready'
}

function Get-DaemonMetrics {
    $raw = (& $adb -s $Device shell 'p=$(pidof iap2d | cut -d " " -f 1); if [ -n "$p" ]; then fds=$(ls /proc/$p/fd 2>/dev/null | wc -l); threads=$(sed -n "s/^Threads:[[:space:]]*//p" /proc/$p/status | head -n 1); rss=$(sed -n "s/^VmRSS:[[:space:]]*//p" /proc/$p/status | head -n 1); rss=${rss%% *}; printf "pid=%s fds=%s threads=%s rssKb=%s" "$p" "$fds" "$threads" "$rss"; fi').Trim()
    if (-not $raw) { throw 'iap2d is not running' }
    $values = @{}
    foreach ($pair in $raw.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
        $parts = $pair.Split('=', 2)
        if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
    }
    [pscustomobject]@{
        Pid = [int]$values.pid
        Fds = [int]$values.fds
        Threads = [int]$values.threads
        RssKb = [int]$values.rssKb
    }
}

& $adb connect $Device | Out-Host
& $adb -s $Device wait-for-device
if ((& $adb -s $Device shell id -u).Trim() -ne '0') { throw 'root is required' }
$actualProduct = (& $adb -s $Device shell getprop ro.product.device).Trim()
if ($actualProduct -ne $ExpectedProduct) {
    throw "device identity mismatch: $Device is '$actualProduct', expected '$ExpectedProduct'"
}
& $adb -s $Device forward "tcp:$ControlPort" "tcp:$ControlPort" | Out-Null

$initialStatus = Wait-ControlReady
if (-not $initialStatus.state) { throw 'status API returned no state' }
$config = Invoke-RestMethod -Uri "$base/api/config" -NoProxy
$json = $config | ConvertTo-Json -Depth 6 -Compress
$samples = [System.Collections.Generic.List[object]]::new()
$samples.Add((Get-DaemonMetrics))

for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    $before = Get-DaemonMetrics
    if ($KillDaemon) {
        Invoke-DeviceShell "kill -KILL $($before.Pid)" | Out-Null
    } else {
        Invoke-RestMethod -Method Put -Uri "$base/api/config" -NoProxy -ContentType 'application/json' -Body $json | Out-Null
    }
    # The configured reconnect maximum can be 30 seconds. Leave additional
    # time for process startup and API readiness after that delay.
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    $after = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        try {
            $candidate = Get-DaemonMetrics -ErrorAction SilentlyContinue
            if ($candidate -and $candidate.Pid -ne $before.Pid) {
                Start-Sleep -Milliseconds 500
                Update-Forward
                $confirmed = Get-DaemonMetrics -ErrorAction SilentlyContinue
                if ($confirmed -and $confirmed.Pid -eq $candidate.Pid) {
                    $status = Invoke-RestMethod -Uri "$base/api/status" -NoProxy -TimeoutSec 2
                    if ($status.state) {
                        $after = $confirmed
                        break
                    }
                }
            }
        } catch {}
    }
    if (-not $after) { throw "daemon did not recover after cycle $cycle" }
    Write-Host ("cycle={0} old_pid={1} new_pid={2} fds={3} threads={4} rssKb={5}" -f `
        $cycle, $before.Pid, $after.Pid, $after.Fds, $after.Threads, $after.RssKb)
    $samples.Add($after)
}

$maxFds = ($samples | Measure-Object Fds -Maximum).Maximum
$maxThreads = ($samples | Measure-Object Threads -Maximum).Maximum
$maxRss = ($samples | Measure-Object RssKb -Maximum).Maximum
$minRss = ($samples | Measure-Object RssKb -Minimum).Minimum
Write-Host "summary cycles=$Cycles maxFds=$maxFds maxThreads=$maxThreads rssKb=$minRss..$maxRss"
Write-Host 'Physical USB unplug/replug still must be run during idle, setup, video, audio, and microphone sessions.'
