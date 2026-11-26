param([switch]$BuildDeviceProbe)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/bluetooth-network-20261117'
$jdk=$env:JAVA_HOME
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$jar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
$env:JAVA_HOME=$jdk
New-Item -ItemType Directory -Force "$out/models" | Out-Null
& "$jdk/bin/javac.exe" '-J-Duser.language=en' -encoding UTF-8 -d "$out/models" "$root/src/android_audio_capture/src/com/carplaybox/audio/BluetoothPanPolicy.java" "$root/tests/BluetoothPanPolicyTest.java"
if($LASTEXITCODE){throw 'PAN policy compile failed'}
& "$jdk/bin/java.exe" -cp "$out/models" com.carplaybox.audio.BluetoothPanPolicyTest
if($LASTEXITCODE){throw 'PAN policy tests failed'}
if(!$BuildDeviceProbe){return}
$dir="$out/probe"
New-Item -ItemType Directory -Force "$dir/classes","$dir/dex" | Out-Null
& "$jdk/bin/javac.exe" '-J-Duser.language=en' -encoding UTF-8 -source 8 -target 8 -cp "$jar;$root/build/android-audio/classes" -d "$dir/classes" "$root/tests/BluetoothNetworkDeviceTest.java"
if($LASTEXITCODE){throw 'PAN probe compile failed'}
$classes=@(Get-ChildItem "$dir/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
& "$bt/d8.bat" --lib $jar --min-api 27 --output "$dir/dex" @classes
if($LASTEXITCODE){throw 'PAN probe DEX failed'}
Copy-Item -LiteralPath "$root/tests/bluetooth-network-probe-manifest.xml" -Destination "$dir/AndroidManifest.xml" -Force
& "$bt/aapt.exe" package -f -M "$dir/AndroidManifest.xml" -I $jar -F "$dir/unsigned.apk" "$dir/dex"
if($LASTEXITCODE){throw 'PAN probe packaging failed'}
& "$bt/zipalign.exe" -f 4 "$dir/unsigned.apk" "$dir/aligned.apk"
if($LASTEXITCODE){throw 'PAN probe alignment failed'}
& "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$dir/probe.apk" "$dir/aligned.apk"
if($LASTEXITCODE){throw 'PAN probe signing failed'}
& "$bt/apksigner.bat" verify "$dir/probe.apk"
if($LASTEXITCODE){throw 'PAN probe signature failed'}
