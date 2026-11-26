param(
    [Parameter(Mandatory=$true)][string]$Device,
    [int]$AdbPort = 5038,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$sdk = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { $env:ANDROID_SDK_ROOT }
$adb = Join-Path $sdk 'platform-tools/adb.exe'
function Invoke-GnssAdb([string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $adb -P $AdbPort -s $Device @Arguments 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code) { throw "GNSS adb failed: $output" }
    return ($output -join "`n").Trim()
}
function Root-Shell([string]$Command) {
    # Use one remote shell string: this ROM does not accept su 0 sh -c reliably.
    if ($Command.Contains("'")) { throw 'Unexpected quote in GNSS root command' }
    Invoke-GnssAdb @('shell', "su -c '$Command'")
}
function Remote-Hash([string]$Path) {
    $line = Root-Shell "sha256sum $Path"
    if ($line -notmatch '^([a-fA-F0-9]{64})\s') { throw "Cannot hash $Path" }
    return $Matches[1].ToUpperInvariant()
}
if ((Root-Shell 'id -u') -ne '0') { throw 'GNSS installation requires root' }
if ((Invoke-GnssAdb @('shell','getprop','ro.product.device')) -ne 'rk3399_mid' -or
    (Invoke-GnssAdb @('shell','getprop','ro.build.version.sdk')) -ne '27') {
    throw 'Vehicle GNSS supports only this RK3399 Android 8.1 firmware'
}
if ((Root-Shell 'getenforce') -ne 'Permissive') {
    throw 'This firmware needs a separately validated SELinux policy; the installer does not change enforcement'
}
$module = Join-Path $packageRoot 'bin/gnss/android.hardware.gnss@1.0-impl.so'
$library = Join-Path $packageRoot 'bin/gnss/android.hardware.gnss@1.0.so'
$moduleHash = (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash
$libraryHash = (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash
if ((Remote-Hash '/system/lib64/android.hardware.gnss@1.0.so') -ne $libraryHash) {
    throw 'System GNSS ABI library differs from the validated firmware'
}
$apkHash = (Get-FileHash -LiteralPath (Join-Path $packageRoot 'apk/CarPlayAudio.apk') -Algorithm SHA256).Hash
$active = Invoke-GnssAdb @('shell','pm','path','com.carplaybox.audio')
if ($active -notmatch '^package:(/[^\r\n]+)$') { throw 'Audio APK is not installed' }
if ((Remote-Hash $Matches[1]) -ne $apkHash -or
    (Remote-Hash '/system/priv-app/CarPlayAudio/CarPlayAudio.apk') -ne $apkHash) {
    throw 'Deploy the packaged Audio APK to both active and system paths first'
}
$modulePath = '/vendor/lib64/hw/android.hardware.gnss@1.0-impl.so'
$libraryPath = '/vendor/lib64/android.hardware.gnss@1.0.so'
$present = (Root-Shell "if [ -e $modulePath ]; then echo yes; else echo no; fi") -eq 'yes'
if ($present) {
    $installedHash = Remote-Hash $modulePath
    $ownedHash = Root-Shell 'if [ -f /data/carplay/vehicle-gnss.sha256 ]; then cat /data/carplay/vehicle-gnss.sha256; fi'
    if ($installedHash -ne $moduleHash -and $installedHash -ne $ownedHash.Trim().ToUpperInvariant()) {
        throw 'An unknown GNSS HAL is installed; refusing to replace it'
    }
}
$libraryPresent = (Root-Shell "if [ -e $libraryPath ]; then echo yes; else echo no; fi") -eq 'yes'
if ($libraryPresent -and (Remote-Hash $libraryPath) -ne $libraryHash) {
    throw 'An unknown vendor GNSS interface library is installed'
}
$projectRoot = Split-Path -Parent (Split-Path -Parent $packageRoot)
$work = Join-Path $projectRoot 'build/vehicle-gnss-install'
New-Item -ItemType Directory -Force $work | Out-Null
$before = Join-Path $work 'manifest.before.xml'
Invoke-GnssAdb @('pull','/vendor/manifest.xml',$before) | Write-Host
$beforeHash = (Get-FileHash -LiteralPath $before -Algorithm SHA256).Hash
$xml = New-Object System.Xml.XmlDocument
$xml.PreserveWhitespace = $true
$xml.Load($before)
if ($xml.DocumentElement.Name -ne 'manifest' -or $xml.DocumentElement.GetAttribute('type') -ne 'device') {
    throw 'Unexpected vendor manifest'
}
$entries = @($xml.SelectNodes('/manifest/hal[name="android.hardware.gnss"]'))
if ($entries.Count -gt 1) { throw 'Ambiguous existing GNSS manifest entries' }
$next = Join-Path $work 'manifest.xml'
if ($entries.Count) {
    $entry = $entries[0]
    if (-not $present -or $entry.format -ne 'hidl' -or
        $entry.SelectSingleNode('transport').InnerText -ne 'passthrough' -or
        $entry.SelectSingleNode('transport').GetAttribute('arch') -ne '64' -or
        $entry.version -ne '1.0' -or $entry.interface.name -ne 'IGnss' -or
        $entry.interface.instance -ne 'default') {
        throw 'Existing GNSS manifest belongs to another implementation'
    }
    Copy-Item -LiteralPath $before -Destination $next -Force
} else {
    $fragment = $xml.CreateDocumentFragment()
    $fragment.InnerXml = '<hal format="hidl"><name>android.hardware.gnss</name><transport arch="64">passthrough</transport><version>1.0</version><interface><name>IGnss</name><instance>default</instance></interface></hal>'
    [void]$xml.DocumentElement.AppendChild($fragment)
    $xml.Save($next)
}
if ($CheckOnly) { Write-Host 'Vehicle GNSS preflight passed; no device files changed.'; return }
if ($present -and $installedHash -eq $moduleHash -and $libraryPresent -and $entries.Count) {
    Write-Host 'Vehicle GNSS already installed; no device files changed.'
    return
}
$stage = '/data/local/tmp/carplay-vehicle-gnss-stage'
Invoke-GnssAdb @('shell', "mkdir -p $stage; chmod 700 $stage") | Write-Host
foreach ($item in @(@($module,'module.so'), @($library,'interface.so'), @($next,'manifest.xml'),
                    @((Join-Path $PSScriptRoot 'install_vehicle_gnss.sh'),'install_vehicle_gnss.sh'))) {
    Invoke-GnssAdb @('push', $item[0], "$stage/$($item[1])") | Write-Host
}
$checksums = @(
    "$moduleHash  module.so",
    "$libraryHash  interface.so",
    "$((Get-FileHash -LiteralPath $next -Algorithm SHA256).Hash)  manifest.xml"
) -join "`n"
[IO.File]::WriteAllText((Join-Path $work 'SHA256SUMS'), "$checksums`n", [Text.Encoding]::ASCII)
Invoke-GnssAdb @('push', (Join-Path $work 'SHA256SUMS'), "$stage/SHA256SUMS") | Write-Host
Root-Shell "sh $stage/install_vehicle_gnss.sh $beforeHash" | Write-Host
Write-Host 'Vehicle GNSS installed. Reboot once to register gps; no boot-time installer or wait was added.'
