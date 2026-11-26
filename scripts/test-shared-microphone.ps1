param([switch]$BuildOnly, [string]$Device='', [int]$AdbPort=5038)
$ErrorActionPreference='Stop'
if (-not $BuildOnly -and -not $Device) { throw '-Device is required unless -BuildOnly is specified.' }
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/shared-microphone'
$jdk=$env:JAVA_HOME
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$androidJar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
$env:JAVA_HOME=$jdk
New-Item -ItemType Directory -Force "$out/classes","$out/dex" | Out-Null
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -cp $androidJar -d "$out/classes" "$root/tests/SharedMicrophoneReaderProbe.java"
if($LASTEXITCODE){throw 'Reader compilation failed'}
& "$jdk/bin/jar.exe" cf "$out/classes.jar" -C "$out/classes" .
if($LASTEXITCODE){throw 'Reader archive failed'}
& "$bt/d8.bat" --lib $androidJar --min-api 27 --output "$out/dex" "$out/classes.jar"
if($LASTEXITCODE){throw 'Reader dex failed'}
foreach($suffix in @('a','b')) {
 $package="com.carplaybox.sharedmicreader$suffix"
 $manifest=@"
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$package">
 <uses-sdk android:minSdkVersion="27" android:targetSdkVersion="27" />
 <uses-permission android:name="android.permission.RECORD_AUDIO" />
 <application android:label="Temporary shared mic test $suffix" />
 <instrumentation android:name="com.carplaybox.audio.SharedMicrophoneReaderProbe" android:targetPackage="$package" />
</manifest>
"@
 [IO.File]::WriteAllText("$out/AndroidManifest.xml",$manifest,[Text.UTF8Encoding]::new($false))
 & "$bt/aapt.exe" package -f -M "$out/AndroidManifest.xml" -I $androidJar -F "$out/unsigned-$suffix.apk" "$out/dex"
 if($LASTEXITCODE){throw 'Reader packaging failed'}
 & "$bt/zipalign.exe" -f 4 "$out/unsigned-$suffix.apk" "$out/aligned-$suffix.apk"
 if($LASTEXITCODE){throw 'Reader alignment failed'}
 & "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$out/reader-$suffix.apk" "$out/aligned-$suffix.apk"
 if($LASTEXITCODE){throw 'Reader signing failed'}
 if(!$BuildOnly) {
  & adb -P $AdbPort -s $Device install -r -g "$out/reader-$suffix.apk"
  if($LASTEXITCODE){throw 'Reader install failed'}
 }
}
