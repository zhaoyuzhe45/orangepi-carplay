$ErrorActionPreference='Stop'
$project=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$build=Join-Path $project 'build/legacy-services-20261111/vehicle-probe-apk'
$sdk=$env:ANDROID_SDK_ROOT
$jdk=$env:JAVA_HOME
$bt=Join-Path $sdk 'build-tools/30.0.2'
$android=Join-Path $sdk 'platforms/android-30/android.jar'
$src=Join-Path $project 'src/android_audio_capture/src/com/carplaybox/audio'
New-Item -ItemType Directory -Force "$build/classes","$build/dex" | Out-Null
Copy-Item -LiteralPath "$project/tests/vehicle-probe-manifest.xml" -Destination "$build/AndroidManifest.xml" -Force
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 1.8 -target 1.8 -classpath $android -d "$build/classes" "$src/VehicleNmeaParser.java" "$src/VehicleLocationBridge.java" "$src/VehicleAndroidProvider.java" "$project/tests/VehicleLocationProbe.java"
if($LASTEXITCODE){throw 'vehicle probe javac failed'}
$env:JAVA_HOME=$jdk
$classes=@(Get-ChildItem -LiteralPath "$build/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
& "$bt/d8.bat" --lib $android --min-api 27 --output "$build/dex" @classes
if($LASTEXITCODE){throw 'vehicle probe d8 failed'}
& "$bt/aapt.exe" package -f -M "$build/AndroidManifest.xml" -I $android -F "$build/probe-unsigned.apk"
if($LASTEXITCODE){throw 'vehicle probe aapt failed'}
Push-Location "$build/dex"
try { & "$bt/aapt.exe" add "$build/probe-unsigned.apk" classes.dex }
finally { Pop-Location }
if($LASTEXITCODE){throw 'vehicle probe dex packaging failed'}
& "$bt/zipalign.exe" -f 4 "$build/probe-unsigned.apk" "$build/probe-aligned.apk"
if($LASTEXITCODE){throw 'vehicle probe zipalign failed'}
& "$bt/apksigner.bat" sign --ks "$project/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$project/config/signing/carplay-audio.password" --out "$build/VehicleProbe.apk" "$build/probe-aligned.apk"
if($LASTEXITCODE){throw 'vehicle probe sign failed'}
Write-Output "$build/VehicleProbe.apk"
