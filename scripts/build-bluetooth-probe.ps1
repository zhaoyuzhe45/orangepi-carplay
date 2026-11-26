$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/bluetooth-20261112/probe'
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$jar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
$jdk=$env:JAVA_HOME
$env:JAVA_HOME=$jdk
New-Item -ItemType Directory -Force "$out/classes","$out/dex" | Out-Null
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -cp "$jar;$root/build/android-audio/classes" -d "$out/classes" "$root/tests/BluetoothBridgeDeviceTest.java"
if($LASTEXITCODE){throw 'Probe compile failed'}
$classes=@(Get-ChildItem "$out/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
& "$bt/d8.bat" --lib $jar --min-api 27 --output "$out/dex" @classes
if($LASTEXITCODE){throw 'Probe DEX failed'}
Copy-Item "$root/tests/bluetooth-probe-manifest.xml" "$out/AndroidManifest.xml" -Force
& "$bt/aapt.exe" package -f -M "$out/AndroidManifest.xml" -I $jar -F "$out/unsigned.apk" "$out/dex"
if($LASTEXITCODE){throw 'Probe packaging failed'}
& "$bt/zipalign.exe" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Probe alignment failed'}
& "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$out/BluetoothProbe.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Probe signing failed'}
& "$bt/apksigner.bat" verify "$out/BluetoothProbe.apk"
if($LASTEXITCODE){throw 'Probe signature failed'}
