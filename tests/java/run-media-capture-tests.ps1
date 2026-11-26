$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$jdk = $env:JAVA_HOME
$out = Join-Path $root 'build/media-capture-tests'
$pkg = Join-Path $root 'src/android_audio_capture/src/com/carplaybox/audio'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& "$jdk/bin/javac.exe" -source 8 -target 8 -d $out `
    "$PSScriptRoot/android/os/Process.java" `
    "$pkg/CaptureThreadPriority.java" "$pkg/MediaCaptureStats.java" `
    "$pkg/NavigationMixer.java" "$pkg/MediaContinuityBuffer.java" "$pkg/NavigationCaptureUids.java" `
    "$PSScriptRoot/com/carplaybox/audio/MediaCaptureTuningTest.java" `
    "$PSScriptRoot/com/carplaybox/audio/MediaContinuityBufferTest.java" `
    "$PSScriptRoot/com/carplaybox/audio/NavigationCaptureUidsTest.java"
if ($LASTEXITCODE) { throw 'Media capture test compilation failed' }
& "$jdk/bin/java.exe" -cp $out com.carplaybox.audio.MediaCaptureTuningTest
if ($LASTEXITCODE) { throw 'Media capture tests failed' }
& "$jdk/bin/java.exe" -cp $out com.carplaybox.audio.MediaContinuityBufferTest
if ($LASTEXITCODE) { throw 'Media continuity tests failed' }
& "$jdk/bin/java.exe" -cp $out com.carplaybox.audio.NavigationCaptureUidsTest
if ($LASTEXITCODE) { throw 'Navigation UID tests failed' }
