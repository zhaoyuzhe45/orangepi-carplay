param([string]$JavaHome)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build/shared-microphone/java-relay/classes'
if (!$JavaHome) {
    $JavaHome = @($env:JAVA_HOME) | Where-Object {
        $_ -and (Test-Path -LiteralPath "$_/bin/javac.exe")
    } | Select-Object -First 1
}
if (!$JavaHome -or !(Test-Path -LiteralPath "$JavaHome/bin/javac.exe")) { throw 'Set -JavaHome to a JDK directory' }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$source = Join-Path $root 'src/android_audio_capture/src/com/carplaybox/audio/SharedMicrophoneRelay.java'
$test = Join-Path $root 'tests/SharedMicrophoneRelayTest.java'
& "$JavaHome/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -d $out $source $test
if ($LASTEXITCODE -ne 0) { throw 'Shared mic relay test compilation failed' }
& "$JavaHome/bin/java.exe" -cp $out com.carplaybox.audio.SharedMicrophoneRelayTest
if ($LASTEXITCODE -ne 0) { throw 'Shared mic relay tests failed' }
