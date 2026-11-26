$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$javac = Get-Command javac.exe -ErrorAction Stop
$java = Get-Command java.exe -ErrorAction Stop
$classes = Join-Path $projectRoot 'build\vehicle-location-poll-tests\classes'
New-Item -ItemType Directory -Force -Path $classes | Out-Null
$production = Join-Path $projectRoot 'src\android_audio_capture\src\com\carplaybox\audio\VehicleLocationPollSchedule.java'
$test = Join-Path $projectRoot 'tests\VehicleLocationPollScheduleTest.java'
& $javac.Source '-encoding' 'UTF-8' '-source' '1.8' '-target' '1.8' '-d' $classes $production $test
if ($LASTEXITCODE -ne 0) { throw 'Vehicle location poll test compilation failed' }
& $java.Source '-ea' '-cp' $classes 'com.carplaybox.audio.VehicleLocationPollScheduleTest'
if ($LASTEXITCODE -ne 0) { throw 'Vehicle location poll schedule test failed' }
