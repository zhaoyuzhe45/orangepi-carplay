param([switch]$BuildDeviceProbe)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/phone-ui-20261112'
$jdk=$env:JAVA_HOME
$env:JAVA_HOME=$jdk
New-Item -ItemType Directory -Force "$out/model-tests" | Out-Null
$sources=@('src/android_audio_capture/src/com/carplaybox/audio/BluetoothCalls.java','src/android_audio_capture/src/com/carplaybox/audio/PhoneUiModel.java','src/android_audio_capture/src/com/carplaybox/audio/PhoneContacts.java','tests/PhoneUiModelTest.java','tests/BluetoothCallsTest.java','tests/PhoneContactsTest.java') | ForEach-Object { Join-Path $root $_ }
& "$jdk/bin/javac.exe" -encoding UTF-8 -d "$out/model-tests" @sources
if($LASTEXITCODE){throw 'Model compilation failed'}
foreach($test in @('PhoneUiModelTest','BluetoothCallsTest','PhoneContactsTest')) {
    & "$jdk/bin/java.exe" -cp "$out/model-tests" "com.carplaybox.audio.$test"
    if($LASTEXITCODE){throw "Failed $test"}
}
if(-not $BuildDeviceProbe){return}
$out=Join-Path $out 'probe'
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$jar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
New-Item -ItemType Directory -Force "$out/classes","$out/dex" | Out-Null
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -cp "$jar;$root/build/android-phone/classes;$root/build/android-audio/classes" -d "$out/classes" "$root/tests/PhoneUiDeviceTest.java" "$root/tests/PhoneServicesDeviceTest.java" "$root/tests/PhonePairingDeviceTest.java" "$root/tests/PhoneBridgeDeviceTest.java"
if($LASTEXITCODE){throw 'Probe compilation failed'}
$classes=@(Get-ChildItem "$out/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
& "$bt/d8.bat" --lib $jar --min-api 27 --output "$out/dex" @classes
if($LASTEXITCODE){throw 'Probe DEX failed'}
Copy-Item "$root/tests/phone-ui-probe-manifest.xml" "$out/AndroidManifest.xml" -Force
& "$bt/aapt.exe" package -f -M "$out/AndroidManifest.xml" -I $jar -F "$out/unsigned.apk" "$out/dex"
if($LASTEXITCODE){throw 'Probe packaging failed'}
& "$bt/zipalign.exe" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Probe alignment failed'}
& "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$out/PhoneUiProbe.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Probe signing failed'}
& "$bt/apksigner.bat" verify "$out/PhoneUiProbe.apk"
if($LASTEXITCODE){throw 'Probe signature failed'}
