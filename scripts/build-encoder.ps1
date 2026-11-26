param(
    [string]$OutputJar = '',
    [string]$JdkRoot = '',
    [string]$SdkRoot = '',
    [string]$BuildToolsVersion = '30.0.2',
    [string]$PlatformVersion = 'android-30'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $JdkRoot) {
    $candidates = @($env:JAVA_HOME)
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'bin\javac.exe'))) { $JdkRoot = $candidate; break }
    }
}
if (-not $SdkRoot) {
    $candidates = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'build-tools'))) { $SdkRoot = $candidate; break }
    }
}
if (-not $JdkRoot -or -not $SdkRoot) { throw 'JDK or Android SDK missing. Pass -JdkRoot/-SdkRoot or set JAVA_HOME/ANDROID_SDK_ROOT.' }
$javac = Join-Path $JdkRoot 'bin\javac.exe'
$jar = Join-Path $JdkRoot 'bin\jar.exe'
$d8 = Join-Path $SdkRoot "build-tools\$BuildToolsVersion\d8.bat"
$androidJar = Join-Path $SdkRoot "platforms\$PlatformVersion\android.jar"
foreach ($dependency in @($javac,$jar,$d8,$androidJar)) {
    if (-not (Test-Path -LiteralPath $dependency)) { throw "Encoder build dependency missing: $dependency" }
}
$buildRoot = Join-Path $projectRoot 'build\encoder'
$classesDir = Join-Path $buildRoot 'classes'
$dexDir = Join-Path $buildRoot 'dex'
foreach ($directory in @($classesDir,$dexDir)) {
    if (Test-Path -LiteralPath $directory) {
        $resolved = (Resolve-Path -LiteralPath $directory).Path
        $boundary = [IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to clean path outside encoder build: $resolved" }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
if (-not $OutputJar) { $OutputJar = Join-Path $buildRoot 'carplay-encoder.jar' }
$OutputJar = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputJar)
$sources = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src\android_encoder\src') -Filter '*.java' -Recurse -File | ForEach-Object FullName)
if ($sources.Count -eq 0) { throw 'No encoder Java sources found' }
$previousJavaHome = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $JdkRoot
    Write-Host 'Compiling encoder Java sources'
    & $javac '-encoding' 'UTF-8' '-source' '1.8' '-target' '1.8' '-classpath' $androidJar '-d' $classesDir @sources
    if ($LASTEXITCODE -ne 0) { throw 'Encoder javac failed' }
    $classFiles = @(Get-ChildItem -LiteralPath $classesDir -Filter '*.class' -Recurse -File | ForEach-Object FullName)
    if ($classFiles.Count -eq 0) { throw 'Encoder javac produced no classes' }
    Write-Host 'Converting encoder to Android 8.1 DEX'
    & $d8 '--lib' $androidJar '--min-api' '27' '--output' $dexDir @classFiles
    if ($LASTEXITCODE -ne 0) { throw 'Encoder d8 failed' }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputJar) | Out-Null
    & $jar 'cf' $OutputJar '-C' $dexDir 'classes.dex'
    if ($LASTEXITCODE -ne 0) { throw 'Encoder jar packaging failed' }
    $contents = & $jar 'tf' $OutputJar
    if ($LASTEXITCODE -ne 0 -or -not ($contents -contains 'classes.dex')) { throw 'Encoder jar has no classes.dex' }
    $artifact = Get-Item -LiteralPath $OutputJar
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $OutputJar
    Write-Host ("Encoder JAR: {0} ({1} bytes) SHA256={2}" -f $artifact.FullName,$artifact.Length,$hash.Hash)
} finally { $env:JAVA_HOME = $previousJavaHome }
