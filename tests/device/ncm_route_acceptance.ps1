param(
    [Parameter(Mandatory=$true)][string]$Device,
    [string]$ExpectedProduct = 'rk3399_mid',
    [string]$Interface = 'iap2ncm0'
)

$ErrorActionPreference = 'Stop'
$adbCandidates = @()
if ($env:ANDROID_SDK_ROOT) { $adbCandidates += (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools\adb.exe') }
if ($env:ANDROID_HOME) { $adbCandidates += (Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe') }
$adbCandidates += (Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe')
$adb = $adbCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $adb) { throw 'adb.exe not found' }

function Invoke-AdbShell {
    param([string]$Command)
    $output = & $adb -s $Device shell $Command 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "adb shell failed: $Command`n$($output -join "`n")"
    }
    return ($output -join "`n")
}

& $adb connect $Device | Out-Host
if ($LASTEXITCODE -ne 0) { throw "adb connect failed: $Device" }
& $adb -s $Device wait-for-device
if ($LASTEXITCODE -ne 0) { throw "adb device unavailable: $Device" }

$product = Invoke-AdbShell 'getprop ro.product.device'
if ($product.Trim() -ne $ExpectedProduct) {
    throw "Device identity mismatch: '$($product.Trim())' != '$ExpectedProduct'"
}

$state = Invoke-AdbShell "ip -details link show dev $Interface; ip -6 addr show dev $Interface; ip -6 rule show; ip -6 route show table 100; ip -6 route get fe80::2 from fe80::1 oif $Interface"
$failures = @()
if ($state -notmatch "UP,LOWER_UP") {
    $failures += "$Interface is not UP/LOWER_UP"
}
if ($state -notmatch 'inet6 fe80::1/64') {
    $failures += "$Interface does not own fe80::1/64"
}
if ($state -notmatch '(?m)^50:\s+from fe80::1 lookup 100\s*$') {
    $failures += 'IPv6 priority-50 source policy rule is missing or incorrect'
}
if ($state -notmatch "(?m)fe80::/64 dev $Interface\s+src fe80::1\s+metric") {
    $failures += "IPv6 fe80::/64 route is missing from table 100 for $Interface"
}
if ($state -notmatch "fe80::2 from fe80::1 dev $Interface\s+table 100\s+src fe80::1") {
    $failures += "route lookup for fe80::2 does not select $Interface table 100"
}

if ($failures.Count) {
    $failures | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    throw "NCM policy-routing acceptance failed with $($failures.Count) error(s)."
}
Write-Host 'NCM policy-routing acceptance passed.'