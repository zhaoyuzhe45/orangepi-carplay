# Run the OrangeCar simulator over LAN. Allow inbound connections to the selected Python executable.
# Pass an explicit ADB device; this changes the sender endpoint for a bench session.
param(
    [Parameter(Mandatory=$true)][string]$Device,
    [int]$Port = 7002,
    [int]$RunSeconds = 45,
    [string]$LocalAddress = '',
    [string]$SimPath = '',
    [switch]$KeepSimLog
)

$ErrorActionPreference = 'Stop'
if (-not $SimPath) {
    $SimPath = Join-Path $PSScriptRoot 'orangecar\carplay_receiver_sim.py'
}
if (-not (Test-Path -LiteralPath $SimPath)) {
    throw "receiver simulator not found: $SimPath (restore tools\bench\orangecar or pass -SimPath)"
}

# Under $ErrorActionPreference = 'Stop' PowerShell turns a native tool's stderr
# into a terminating error -- adb writes "daemon not running; starting now" to
# stderr on a cold start, which aborted this script before it did anything.
function Invoke-Native {
    param([string]$FilePath, [string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = (& $FilePath @Arguments 2>&1 | Out-String).Trim()
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    return [pscustomobject]@{ Output = $output; ExitCode = $code }
}

function Find-Adb {
    $candidates = @()
    if ($env:ANDROID_SDK_ROOT) { $candidates += Join-Path $env:ANDROID_SDK_ROOT 'platform-tools\adb.exe' }
    if ($env:ANDROID_HOME) { $candidates += Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe' }
    $candidates += (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe')
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    throw 'adb.exe not found. Set ANDROID_SDK_ROOT.'
}

# Only return an interpreter the firewall already allows inbound for.  Asking
# for the rule is cheaper than diagnosing a silent SYN drop later.
function Pick-Python {
    $rules = Get-NetFirewallRule -Direction Inbound -Enabled True -ErrorAction SilentlyContinue |
        Where-Object { $_.Action -eq 'Allow' }
    $allowed = @()
    foreach ($rule in $rules) {
        $filter = $rule | Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue
        if ($filter -and $filter.Program -and $filter.Program -ne 'Any') {
            $allowed += $filter.Program
        }
    }
    $candidates = @()
    $candidates += (Get-Command python.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    foreach ($candidate in $candidates) {
        if (-not $candidate -or -not (Test-Path -LiteralPath $candidate)) { continue }
        foreach ($program in $allowed) {
            if ($program -ieq $candidate) { return $candidate }
        }
    }
    throw 'No python.exe with an inbound firewall allow rule. Add one for the interpreter you want to use, or copy the simulator run to an allowed interpreter.'
}

# The box talks to us over the LAN, so we need the address on the same subnet
# as the box's wlan0 -- not "the first IPv4 address on this host".
function Resolve-BoxAddress {
    param([string]$Adb, [string]$Serial)
    $result = Invoke-Native -FilePath $Adb -Arguments @('-s', $Serial, 'shell', 'ip -4 addr show wlan0')
    $match = [regex]::Match($result.Output, 'inet (\d+\.\d+\.\d+\.\d+)/(\d+)')
    if (-not $match.Success) { return '' }
    $boxIp = $match.Groups[1].Value
    $prefix = [int]$match.Groups[2].Value
    # NB: in PS 5.1 the literal 0xFFFFFFFF is Int32 -1, so it cannot be cast to
    # UInt64; keep the mask in decimal and widen explicitly.
    $mask = [uint32]0
    if ($prefix -gt 0 -and $prefix -le 32) {
        $wide = ([uint64]4294967295) -shl (32 - $prefix)
        $mask = [uint32]($wide -band [uint64]4294967295)
    }
    $bytes = [System.Net.IPAddress]::Parse($boxIp).GetAddressBytes()
    $boxValue = ([uint32]$bytes[0] -shl 24) -bor ([uint32]$bytes[1] -shl 16) -bor ([uint32]$bytes[2] -shl 8) -bor [uint32]$bytes[3]
    $boxNet = $boxValue -band $mask
    foreach ($ip in Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue) {
        if ($ip.IPAddress -like '127.*' -or $ip.IPAddress -like '169.254.*') { continue }
        $b = [System.Net.IPAddress]::Parse($ip.IPAddress).GetAddressBytes()
        $v = ([uint32]$b[0] -shl 24) -bor ([uint32]$b[1] -shl 16) -bor ([uint32]$b[2] -shl 8) -bor [uint32]$b[3]
        if (($v -band $mask) -eq $boxNet) { return $ip.IPAddress }
    }
    return ''
}

$adb = Find-Adb
$python = Pick-Python
(Invoke-Native -FilePath $adb -Arguments @('connect', $Device)) | Out-Null
$state = (Invoke-Native -FilePath $adb -Arguments @('-s', $Device, 'get-state')).Output
if ($state -ne 'device') { throw "adb cannot see $Device (state: $state)" }
if (-not $LocalAddress) { $LocalAddress = Resolve-BoxAddress -Adb $adb -Serial $Device }
if (-not $LocalAddress) {
    throw 'Could not work out which local address shares the box subnet. Pass -LocalAddress explicitly (ipconfig /all).'
}

Write-Host "simulator : $SimPath"
Write-Host "python    : $python"
Write-Host "box       : $Device -> ${LocalAddress}:$Port"
Write-Host ''

$simLog = Join-Path ([System.IO.Path]::GetTempPath()) "carplay-sim-$Port.log"
$arguments = @($SimPath, '--port', "$Port", '--run-secs', "$RunSeconds")
$info = New-Object System.Diagnostics.ProcessStartInfo
$info.FileName = $python
$info.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
$info.UseShellExecute = $false
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.WorkingDirectory = Split-Path -Parent $SimPath
$sim = [System.Diagnostics.Process]::Start($info)

try {
    Start-Sleep -Milliseconds 800
    if ($sim.HasExited) {
        $early = $sim.StandardOutput.ReadToEnd() + $sim.StandardError.ReadToEnd()
        throw "simulator exited immediately:`n$early"
    }
    Write-Host '--- box side -------------------------------------------------'
    # The sender loops until killed; the timeout is what ends the run, and it
    # is shorter than the simulator's window so the teardown is ours.
    $budget = [Math]::Max(5, $RunSeconds - 5)
    # Streaming, not captured: the sender's log IS the result.  stderr is merged
    # on the *device* side, otherwise PowerShell flushes adb's stderr after the
    # next header and the transcript reads out of order.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $adb -s $Device shell "su 0 timeout $budget /data/carplay/bin/iap2d --fg --airplay-endpoint ${LocalAddress}:$Port 2>&1"
    $ErrorActionPreference = $previous
    Write-Host ''
    Write-Host '--- simulator side --------------------------------------------'
    if (-not $sim.WaitForExit($RunSeconds * 1000)) { $sim.Kill() }
    $simText = $sim.StandardOutput.ReadToEnd() + $sim.StandardError.ReadToEnd()
    $simText
    if ($KeepSimLog) {
        $simText | Set-Content -LiteralPath $simLog -Encoding utf8
        Write-Host "simulator log: $simLog"
    }
} finally {
    if ($sim -and -not $sim.HasExited) { $sim.Kill() }
}

Write-Host ''
Write-Host 'Reading the result:'
Write-Host '  "session aborted at <step>" is this box naming the request the receiver'
Write-Host '  refused.  The simulator only speaks the unpaired AirPlay path, so expect'
Write-Host '  "pair-setup M1 refused (status=470)" -- that is a faithful difference,'
Write-Host '  not a regression: a head unit that pairs never answers 470 there.'

