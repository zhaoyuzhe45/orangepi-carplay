param([switch]$BuildDeviceProbe)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/queue-navigation-20261112'
$jdk=$env:JAVA_HOME
$env:JAVA_HOME=$jdk
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$jar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
New-Item -ItemType Directory -Force "$out/models" | Out-Null
$modelSources=@('NavigationMixer','MediaQueue','AppSnapshot','MediaExtras') | ForEach-Object {"$root/src/android_audio_capture/src/com/carplaybox/audio/$_.java";"$root/tests/$($_)Test.java"}
& "$jdk/bin/javac.exe" '-J-Duser.language=en' -encoding UTF-8 -d "$out/models" @modelSources
if($LASTEXITCODE){throw 'Model javac failed'}
foreach($model in @('NavigationMixer','MediaQueue','AppSnapshot','MediaExtras')){
 & "$jdk/bin/java.exe" -cp "$out/models" "com.carplaybox.audio.$($model)Test"
 if($LASTEXITCODE){throw "Model tests failed: $model"}
}
if(-not $BuildDeviceProbe){return}
foreach($kind in @('split','tone','media')){
 $dir="$out/probe-$kind"
 foreach($leaf in @('classes','dex')){
  $cleanPath=[IO.Path]::GetFullPath("$dir/$leaf")
  if(-not $cleanPath.StartsWith([IO.Path]::GetFullPath($out)+[IO.Path]::DirectorySeparatorChar)){throw 'Unsafe probe output path'}
  if(Test-Path -LiteralPath $cleanPath){Remove-Item -LiteralPath $cleanPath -Recurse -Force}
 }
 New-Item -ItemType Directory -Force "$dir/classes","$dir/dex" | Out-Null
 if($kind -eq 'split'){
  $sources=@("$root/tests/AudioSplitDeviceTest.java","$root/tests/MediaQueueDeviceTest.java")
  $manifest="$root/tests/audio-split-probe-manifest.xml"
 }elseif($kind -eq 'tone'){$sources=@("$root/tests/AudioToneFixture.java");$manifest="$root/tests/audio-tone-manifest.xml"}
 else{$sources=@("$root/tests/AppMediaFixture.java","$root/tests/AppVoiceFixture.java");$manifest="$root/tests/app-fixture-manifest.xml"}
 & "$jdk/bin/javac.exe" '-J-Duser.language=en' -encoding UTF-8 -source 8 -target 8 -cp "$jar;$root/build/android-audio/classes" -d "$dir/classes" @sources
 if($LASTEXITCODE){throw 'Probe javac failed'}
 $classes=@(Get-ChildItem "$dir/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
 & "$bt/d8.bat" --lib $jar --min-api 27 --output "$dir/dex" @classes
 if($LASTEXITCODE){throw 'Probe DEX failed'}
 Copy-Item -LiteralPath $manifest -Destination "$dir/AndroidManifest.xml" -Force
 & "$bt/aapt.exe" package -f -M "$dir/AndroidManifest.xml" -I $jar -F "$dir/unsigned.apk" "$dir/dex"
 if($LASTEXITCODE){throw 'Probe packaging failed'}
 & "$bt/zipalign.exe" -f 4 "$dir/unsigned.apk" "$dir/aligned.apk"
 if($LASTEXITCODE){throw 'Probe alignment failed'}
 & "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$dir/probe.apk" "$dir/aligned.apk"
 if($LASTEXITCODE){throw 'Probe signing failed'}
}
