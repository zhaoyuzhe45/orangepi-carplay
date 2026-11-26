param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build/gps-launcher-compat-20261116/probe'
$jdk = $env:JAVA_HOME
$env:JAVA_HOME = $jdk
$bt = (Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$jar = (Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
if (-not (Test-Path "$root/build/android-audio/classes/com/carplaybox/audio/VehicleGnssChannel.class")) {
    throw 'Build the Audio APK first; this probe uses its production classes.'
}
New-Item -ItemType Directory -Force "$out/classes", "$out/dex" | Out-Null
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -cp "$jar;$root/build/android-audio/classes" -d "$out/classes" "$root/tests/VehicleGnssDeviceProbe.java"
if ($LASTEXITCODE) { throw 'GNSS probe javac failed' }
$classes = @(Get-ChildItem "$out/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
& "$bt/d8.bat" --lib $jar --min-api 26 --output "$out/dex" @classes
if ($LASTEXITCODE) { throw 'GNSS probe DEX failed' }
Copy-Item -LiteralPath "$root/tests/vehicle-gnss-probe-manifest.xml" -Destination "$out/AndroidManifest.xml" -Force
& "$bt/aapt.exe" package -f -M "$out/AndroidManifest.xml" -I $jar -F "$out/unsigned.apk" "$out/dex"
if ($LASTEXITCODE) { throw 'GNSS probe packaging failed' }
& "$bt/zipalign.exe" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
if ($LASTEXITCODE) { throw 'GNSS probe alignment failed' }
& "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$out/probe.apk" "$out/aligned.apk"
if ($LASTEXITCODE) { throw 'GNSS probe signing failed' }
Write-Host "Built bench-only probe: $out/probe.apk"
