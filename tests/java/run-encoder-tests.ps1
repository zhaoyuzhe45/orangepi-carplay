param([string]$JdkRoot = $env:JAVA_HOME)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$classes = Join-Path $projectRoot 'build\encoder-tests\classes'
if (Test-Path -LiteralPath $classes) {
    $resolved = (Resolve-Path -LiteralPath $classes).Path
    $boundary = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build\encoder-tests')).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to clean path outside encoder tests: $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $classes | Out-Null
$production = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src\android_encoder\src') -Recurse -Filter '*.java' -File | ForEach-Object FullName)
$testRoots = @((Join-Path $PSScriptRoot 'android'), (Join-Path $PSScriptRoot 'com\carplaybox\encoder'))
$tests = @(Get-ChildItem -LiteralPath $testRoots -Recurse -Filter '*.java' -File | ForEach-Object FullName)
& (Join-Path $JdkRoot 'bin\javac.exe') '-J-Duser.language=en' '-encoding' 'UTF-8' '-source' '1.8' '-target' '1.8' '-d' $classes @production @tests
if ($LASTEXITCODE -ne 0) { throw 'Encoder host test compilation failed' }
& (Join-Path $JdkRoot 'bin\java.exe') '-ea' '-cp' $classes 'com.carplaybox.encoder.EncoderProtocolTest'
if ($LASTEXITCODE -ne 0) { throw 'Encoder protocol tests failed' }
& (Join-Path $JdkRoot 'bin\java.exe') '-ea' '-cp' $classes 'com.carplaybox.encoder.ScreenEncoderTest'
if ($LASTEXITCODE -ne 0) { throw 'Screen encoder lifecycle tests failed' }
