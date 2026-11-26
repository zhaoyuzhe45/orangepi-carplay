param(
    [Parameter(Mandatory=$true)]
    [string]$Device,
    [string]$ExpectedProduct = 'rk3399_mid',
    [string]$Adb = (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Adb)) { throw "adb not found: $Adb" }

function Invoke-Adb([string[]]$Arguments) {
    & $Adb @Arguments
    if ($LASTEXITCODE -ne 0) { throw "adb failed: $($Arguments -join ' ')" }
}

function Device-Shell([string]$Command) {
    $output = & $Adb -s $Device shell $Command
    if ($LASTEXITCODE -ne 0) { throw "device command failed: $Command" }
    return ($output -join "`n")
}

Invoke-Adb @('connect', $Device) | Out-Null
Invoke-Adb @('-s', $Device, 'wait-for-device') | Out-Null
if ((Device-Shell 'id -u').Trim() -ne '0') { throw 'root is required' }
$product = (Device-Shell 'getprop ro.product.device').Trim()
if ($product -ne $ExpectedProduct) {
    throw "device identity mismatch: $Device is '$product', expected '$ExpectedProduct'"
}

$snapshotCommand = @(
    'supervisors=$(ps -A -o PID,PPID,ARGS 2>/dev/null | grep carplay_supervisor.sh | grep -v grep)',
    'daemons=$(ps -A -o PID,PPID,ARGS 2>/dev/null | grep -E "(^| )iap2d( |$)" | grep -v grep)',
    'supervisor_count=$(printf "%s\n" "$supervisors" | sed ''/^$/d'' | wc -l)',
    'daemon_count=$(printf "%s\n" "$daemons" | sed ''/^$/d'' | wc -l)',
    'pid_file=$(cat /data/carplay/supervisor.pid 2>/dev/null || true)',
    'daemon_file=$(cat /data/carplay/daemon.pid 2>/dev/null || true)',
    'lock_pid=$(cat /data/carplay/supervisor.lock/pid 2>/dev/null || true)',
    'service=$(getprop init.svc.carplay_box 2>/dev/null || true)',
    'printf "supervisor_count=%s\ndaemon_count=%s\nsupervisor_pid=%s\ndaemon_pid=%s\nlock_pid=%s\nservice=%s\n" \',
    '    "$supervisor_count" "$daemon_count" "$pid_file" "$daemon_file" "$lock_pid" "$service"',
    'printf "%s\n" "$supervisors"',
    'printf "%s\n" "$daemons"'
) -join "`n"

function Get-Snapshot {
    $raw = Device-Shell $snapshotCommand
    $lines = @($raw -split "`r?`n")
    $values = @{}
    foreach ($line in $lines) {
        if ($line -match '^(supervisor_count|daemon_count|supervisor_pid|daemon_pid|lock_pid|service)=(.*)$') {
            $values[$Matches[1]] = $Matches[2]
        }
    }
    [pscustomobject]@{
        Raw = $raw
        SupervisorCount = [int]$values.supervisor_count
        DaemonCount = [int]$values.daemon_count
        SupervisorPid = $values.supervisor_pid
        DaemonPid = $values.daemon_pid
        LockPid = $values.lock_pid
        Service = $values.service
    }
}

$before = Get-Snapshot
if ($before.SupervisorCount -ne 1) { throw "expected one supervisor, found $($before.SupervisorCount)`n$($before.Raw)" }
if ($before.DaemonCount -ne 1) { throw "expected one iap2d, found $($before.DaemonCount)`n$($before.Raw)" }
if ($before.LockPid -ne $before.SupervisorPid) { throw "singleton lock owner mismatch`n$($before.Raw)" }
if ($before.Service -ne 'running') { throw "carplay_box init service is '$($before.Service)'`n$($before.Raw)" }

Device-Shell '/system/bin/sh /data/carplay/bin/carplay_supervisor.sh --force-attach >/dev/null 2>&1; echo duplicate_exit=$?' | Out-Null
Start-Sleep -Seconds 1
$after = Get-Snapshot
if ($after.SupervisorCount -ne 1) { throw "duplicate launch changed supervisor count to $($after.SupervisorCount)`n$($after.Raw)" }
if ($after.DaemonCount -ne 1) { throw "duplicate launch changed daemon count to $($after.DaemonCount)`n$($after.Raw)" }
if ($after.SupervisorPid -ne $before.SupervisorPid) { throw 'duplicate launch replaced the supervisor PID' }
if ($after.DaemonPid -ne $before.DaemonPid) { throw 'duplicate launch replaced the daemon PID' }

Write-Host "supervisor singleton PASS product=$product supervisor=$($after.SupervisorPid) daemon=$($after.DaemonPid) service=$($after.Service)"