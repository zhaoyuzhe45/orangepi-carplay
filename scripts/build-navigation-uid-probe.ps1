$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/navigation-uid-probe-20261120'
$jdk=$env:JAVA_HOME
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$jar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
$env:JAVA_HOME=$jdk
$pkg=Join-Path $root 'src/android_audio_capture/src/com/carplaybox/audio'
$probeSources=@('SplitAudioCapture','NavigationMixer','CarMicrophoneBridge','MicrophoneDemandPolicy') | ForEach-Object {"$pkg/$_.java"}
foreach($kind in @('probe','tone')){
    $dir=Join-Path $out $kind
    New-Item -ItemType Directory -Force "$dir/classes","$dir/dex" | Out-Null
    $sources=@(if($kind -eq 'probe'){$probeSources; "$root/tests/NavigationUidDeviceProbe.java"}else{"$root/tests/AudioToneFixture.java"})
    & "$jdk/bin/javac.exe" '-J-Duser.language=en' -encoding UTF-8 -source 8 -target 8 -cp $jar -d "$dir/classes" @sources
    if($LASTEXITCODE){throw "$kind compilation failed"}
    & "$jdk/bin/jar.exe" cf "$dir/classes.jar" -C "$dir/classes" .
    if($LASTEXITCODE){throw "$kind archive failed"}
    & "$bt/d8.bat" --lib $jar --min-api 27 --output "$dir/dex" "$dir/classes.jar"
    if($LASTEXITCODE){throw "$kind dex failed"}
}
foreach($suffix in @('music','a','b')){
    $package="com.carplaybox.navuidfixture$suffix"
    $manifest=@"
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$package">
 <uses-sdk android:minSdkVersion="27" android:targetSdkVersion="27" />
 <application android:label="Temporary navigation tone $suffix">
  <service android:name="com.carplaybox.fixture.AudioToneFixture" android:exported="true" android:permission="android.permission.DUMP" />
 </application>
</manifest>
"@
    $manifestDir=Join-Path $out "manifest-$suffix"
    New-Item -ItemType Directory -Force $manifestDir | Out-Null
    [IO.File]::WriteAllText("$manifestDir/AndroidManifest.xml",$manifest,[Text.UTF8Encoding]::new($false))
    & "$bt/aapt.exe" package -f -M "$manifestDir/AndroidManifest.xml" -I $jar -F "$out/unsigned-$suffix.apk" "$out/tone/dex"
    if($LASTEXITCODE){throw "$suffix packaging failed"}
    & "$bt/zipalign.exe" -f 4 "$out/unsigned-$suffix.apk" "$out/aligned-$suffix.apk"
    if($LASTEXITCODE){throw "$suffix alignment failed"}
    & "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$out/tone-$suffix.apk" "$out/aligned-$suffix.apk"
    if($LASTEXITCODE){throw "$suffix signing failed"}
}
