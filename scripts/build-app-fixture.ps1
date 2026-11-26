$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/app-integration-20261111/fixture'
$sdk=$env:ANDROID_SDK_ROOT
$bt=Join-Path $sdk 'build-tools/30.0.2'
$jdk=$env:JAVA_HOME
$env:JAVA_HOME=$jdk
$env:PATH="$jdk\bin;"+$env:PATH
New-Item -ItemType Directory -Force "$out/classes","$out/dex" | Out-Null
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -cp "$sdk/platforms/android-30/android.jar" -d "$out/classes" "$root/tests/AppMediaFixture.java" "$root/tests/AppVoiceFixture.java"
if($LASTEXITCODE){throw 'javac failed'}
$classes=@(Get-ChildItem "$out/classes" -Recurse -Filter '*.class' | ForEach-Object FullName)
& "$bt/d8.bat" --lib "$sdk/platforms/android-30/android.jar" --min-api 26 --output "$out/dex" @classes
if($LASTEXITCODE){throw 'd8 failed'}
Copy-Item "$root/tests/app-fixture-manifest.xml" "$out/AndroidManifest.xml"
& "$bt/aapt.exe" package -f -M "$out/AndroidManifest.xml" -I "$sdk/platforms/android-30/android.jar" -F "$out/unsigned.apk"
if($LASTEXITCODE){throw 'aapt failed'}
Push-Location "$out/dex"
try { & "$bt/aapt.exe" add "$out/unsigned.apk" classes.dex; if($LASTEXITCODE){throw 'aapt add failed'} } finally { Pop-Location }
& "$bt/zipalign.exe" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'zipalign failed'}
& "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --out "$out/AppFixture.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'sign failed'}
& "$bt/apksigner.bat" verify "$out/AppFixture.apk"
if($LASTEXITCODE){throw 'verify failed'}
Write-Host "Fixture: $out/AppFixture.apk"
