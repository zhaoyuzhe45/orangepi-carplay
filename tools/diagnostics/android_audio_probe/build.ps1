$ErrorActionPreference='Stop'
$project=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$build=Join-Path $project 'build/legacy-services-20261111/probe-apk'
$sdk=$env:ANDROID_SDK_ROOT
$jdk=$env:JAVA_HOME
$bt=Join-Path $sdk 'build-tools/30.0.2'
$android=Join-Path $sdk 'platforms/android-30/android.jar'
New-Item -ItemType Directory -Force "$build/classes","$build/dex" | Out-Null
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 1.8 -target 1.8 -classpath $android -d "$build/classes" "$PSScriptRoot/CarServicesProbe.java" "$PSScriptRoot/ProbeService.java"
if($LASTEXITCODE){throw 'probe javac failed'}
$env:JAVA_HOME=$jdk
$classes=@(Get-ChildItem -LiteralPath "$build/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
& "$bt/d8.bat" --lib $android --min-api 27 --output "$build/dex" @classes
if($LASTEXITCODE){throw 'probe d8 failed'}
& "$bt/aapt.exe" package -f -M "$PSScriptRoot/AndroidManifest.xml" -I $android -F "$build/probe-unsigned.apk"
if($LASTEXITCODE){throw 'probe aapt failed'}
Push-Location "$build/dex"
try { & "$bt/aapt.exe" add "$build/probe-unsigned.apk" classes.dex }
finally { Pop-Location }
if($LASTEXITCODE){throw 'probe dex package failed'}
& "$bt/zipalign.exe" -f 4 "$build/probe-unsigned.apk" "$build/probe-aligned.apk"
if($LASTEXITCODE){throw 'probe zipalign failed'}
& "$bt/apksigner.bat" sign --ks "$project/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$project/config/signing/carplay-audio.password" --out "$build/CarPlayProbe.apk" "$build/probe-aligned.apk"
if($LASTEXITCODE){throw 'probe sign failed'}
Write-Output "$build/CarPlayProbe.apk"
