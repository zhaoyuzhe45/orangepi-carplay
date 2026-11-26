$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/shared-microphone/status'
$jdk=$env:JAVA_HOME
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$androidJar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
$env:JAVA_HOME=$jdk
New-Item -ItemType Directory -Force "$out/classes","$out/dex" | Out-Null
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -d "$out/classes" "$root/tests/SharedMicrophoneStatusProbe.java"
if($LASTEXITCODE){throw 'Status probe compilation failed'}
& "$jdk/bin/jar.exe" cf "$out/classes.jar" -C "$out/classes" .
if($LASTEXITCODE){throw 'Status class archive failed'}
& "$bt/d8.bat" --lib $androidJar --min-api 27 --output "$out/dex" "$out/classes.jar"
if($LASTEXITCODE){throw 'Status dex failed'}
& "$jdk/bin/jar.exe" cf "$out/shared-mic-status.jar" -C "$out/dex" classes.dex
if($LASTEXITCODE){throw 'Status dex archive failed'}
Get-FileHash "$out/shared-mic-status.jar" -Algorithm SHA256
