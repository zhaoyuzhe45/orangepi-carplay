param(
    [string]$Keystore = '',
    [string]$Alias = 'CARPLAY',
    [string]$StorePassword = '',
    [string]$KeyPassword = '',
    [string]$OutputApk = '',
    [string]$BuildToolsVersion = '30.0.2',
    [string]$PlatformVersion = 'android-30',
    [switch]$SkipSign
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

function Find-SdkRoot {
    $candidates = @()
    if ($env:ANDROID_SDK_ROOT) { $candidates += $env:ANDROID_SDK_ROOT }
    if ($env:ANDROID_HOME) { $candidates += $env:ANDROID_HOME }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'build-tools'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'Android SDK not found. Set ANDROID_SDK_ROOT or ANDROID_HOME.'
}

function Find-JdkRoot {
    $candidates = @()
    if ($env:JAVA_HOME) { $candidates += $env:JAVA_HOME }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'bin\javac.exe'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $javac = Get-Command javac.exe -ErrorAction SilentlyContinue
    if ($javac) { return (Split-Path -Parent (Split-Path -Parent $javac.Source)) }
    throw 'JDK not found. Set JAVA_HOME.'
}

$sdkRoot = Find-SdkRoot
$jdkRoot = Find-JdkRoot
$buildTools = Join-Path $sdkRoot "build-tools\$BuildToolsVersion"
$aapt = Join-Path $buildTools 'aapt.exe'
$d8 = Join-Path $buildTools 'd8.bat'
$zipalign = Join-Path $buildTools 'zipalign.exe'
$apksigner = Join-Path $buildTools 'apksigner.bat'
$androidJar = Join-Path $sdkRoot "platforms\$PlatformVersion\android.jar"
foreach ($tool in @($aapt, $d8, $zipalign, $apksigner, $androidJar)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Android build dependency missing: $tool" }
}

$env:JAVA_HOME = $jdkRoot
$env:PATH = (Join-Path $jdkRoot 'bin') + ';' + $env:PATH

if (-not $Keystore) { $Keystore = Join-Path $projectRoot 'config\signing\carplay-audio.keystore' }
if (-not (Test-Path -LiteralPath $Keystore)) { throw "Keystore not found: $Keystore" }
$Keystore = (Resolve-Path -LiteralPath $Keystore).Path
if (-not $StorePassword) { $StorePassword = $env:CARPLAY_AUDIO_KEYSTORE_PASSWORD }
if (-not $StorePassword) {
    $passwordFile = Join-Path $projectRoot 'config\signing\carplay-audio.password'
    if (Test-Path -LiteralPath $passwordFile) {
        $StorePassword = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $passwordFile).Path,
            [Text.Encoding]::UTF8).TrimEnd([char[]]@("`r", "`n"))
    }
}
if (-not $KeyPassword) { $KeyPassword = $StorePassword }
if (-not $SkipSign -and -not $StorePassword) {
    throw 'Keystore password is empty. Set CARPLAY_AUDIO_KEYSTORE_PASSWORD, put it in config\signing\carplay-audio.password, or pass -StorePassword.'
}
if (-not $OutputApk) { $OutputApk = Join-Path $projectRoot 'deploy\opi4\apk\CarPlayAudio.apk' }
$OutputApk = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputApk)

$sourceRoot = Join-Path $projectRoot 'src\android_audio_capture'
$manifest = Join-Path $sourceRoot 'AndroidManifest.xml'
$resourceDir = Join-Path $sourceRoot 'res'
$buildRoot = Join-Path $projectRoot 'build\android-audio'
$genDir = Join-Path $buildRoot 'gen'
$classesDir = Join-Path $buildRoot 'classes'
$dexDir = Join-Path $buildRoot 'dex'
$stageDir = Join-Path $buildRoot 'stage'
foreach ($dir in @($buildRoot, $genDir, $classesDir, $dexDir, $stageDir)) {
    if (Test-Path -LiteralPath $dir) {
        $resolved = (Resolve-Path -LiteralPath $dir).Path
        if (-not $resolved.StartsWith($projectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean path outside project: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$javaFiles = @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'src') -Filter '*.java' -File -Recurse | ForEach-Object { $_.FullName })
if ($javaFiles.Count -eq 0) { throw 'No Android Java source files found.' }

Write-Host 'Generating Android resources'
& $aapt package -f -m -J $genDir -M $manifest -S $resourceDir -I $androidJar
if ($LASTEXITCODE -ne 0) { throw 'aapt resource generation failed' }

$generatedJava = @(Get-ChildItem -LiteralPath $genDir -Filter '*.java' -File -Recurse | ForEach-Object { $_.FullName })
$javacArgs = @('-encoding','UTF-8','-source','1.8','-target','1.8','-classpath',$androidJar,'-d',$classesDir) + $javaFiles + $generatedJava
Write-Host 'Compiling Java sources'
& (Join-Path $jdkRoot 'bin\javac.exe') @javacArgs
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

# Keep only the console launcher entry; the helper removes pre-upgrade shortcuts.
$shortcutClass = Join-Path $classesDir 'com\carplaybox\audio\CarPlayUiShortcut.class'
$returnClass = Join-Path $classesDir 'com\carplaybox\audio\ReturnToCarActivity.class'
if (-not (Test-Path -LiteralPath $shortcutClass) -or
    (Test-Path -LiteralPath $returnClass)) {
    throw 'Legacy shortcut cleanup is missing, or removed Activity was compiled.'
}
$shortcutBytes = [IO.File]::ReadAllBytes($shortcutClass)
$shortcutText = [Text.Encoding]::Latin1.GetString($shortcutBytes)
if ($shortcutText -notmatch 'UNINSTALL_SHORTCUT' -or
    $shortcutText -match 'action\.INSTALL_SHORTCUT') {
    throw 'Compiled shortcut cleanup must remove the legacy entry without reinstalling it.'
}

$classFiles = @(Get-ChildItem -LiteralPath $classesDir -Filter '*.class' -File -Recurse | ForEach-Object { $_.FullName })
if ($classFiles.Count -eq 0) { throw 'No class files were produced.' }
Write-Host 'Converting Java bytecode to DEX'
# d8.bat runs through cmd.exe's 8191-character command line limit on Windows.
$classesJar = Join-Path $buildRoot 'classes.jar'
& (Join-Path $jdkRoot 'bin\jar.exe') cf $classesJar -C $classesDir .
if ($LASTEXITCODE -ne 0) { throw 'Class archive failed' }
& $d8 '--lib' $androidJar '--min-api' '21' '--output' $dexDir $classesJar
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }

Copy-Item -LiteralPath (Join-Path $dexDir 'classes.dex') -Destination (Join-Path $stageDir 'classes.dex') -Force
$unsigned = Join-Path $buildRoot 'CarPlayAudio-unsigned.apk'
$aligned = Join-Path $buildRoot 'CarPlayAudio-aligned.apk'
Write-Host 'Packaging APK'
& $aapt package -f -M $manifest -S $resourceDir -I $androidJar -F $unsigned $stageDir
if ($LASTEXITCODE -ne 0) { throw 'aapt packaging failed' }
& $zipalign -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputApk) | Out-Null
if ($SkipSign) {
    Copy-Item -LiteralPath $aligned -Destination $OutputApk -Force
    Write-Warning "Produced unsigned APK because -SkipSign was supplied: $OutputApk"
    exit 0
}

Write-Host 'Signing APK'
$oldStore = $env:CARPLAY_AUDIO_SIGN_STORE
$oldKey = $env:CARPLAY_AUDIO_SIGN_KEY
try {
    $env:CARPLAY_AUDIO_SIGN_STORE = $StorePassword
    $env:CARPLAY_AUDIO_SIGN_KEY = $KeyPassword
    & $apksigner sign --ks $Keystore --ks-key-alias $Alias --ks-pass env:CARPLAY_AUDIO_SIGN_STORE --key-pass env:CARPLAY_AUDIO_SIGN_KEY --v4-signing-enabled false --out $OutputApk $aligned
    if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }
} finally {
    $env:CARPLAY_AUDIO_SIGN_STORE = $oldStore
    $env:CARPLAY_AUDIO_SIGN_KEY = $oldKey
}

Write-Host 'Verifying APK signature'
& $apksigner verify --verbose $OutputApk
if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed' }

# APK must contain the project fallback asset and the Activity entry.
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
$archive = [IO.Compression.ZipFile]::OpenRead($OutputApk)
try {
    $iconEntry = @($archive.Entries | Where-Object { $_.FullName -like 'res/drawable-nodpi*/car_ui_fallback.png' }) |
        Select-Object -First 1
    if (-not $iconEntry) { throw 'APK is missing the car UI fallback icon.' }
    $manifestEntry = @($archive.Entries | Where-Object { $_.FullName -eq 'AndroidManifest.xml' }) |
        Select-Object -First 1
    if (-not $manifestEntry) { throw 'APK is missing AndroidManifest.xml.' }
} finally {
    $archive.Dispose()
}

$artifact = Get-Item -LiteralPath $OutputApk
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $OutputApk
Write-Host ("APK: {0} ({1} bytes) SHA256={2}" -f $artifact.FullName, $artifact.Length, $hash.Hash)
$defaultApk = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'deploy\opi4\apk\CarPlayAudio.apk'))
if ($artifact.FullName -eq $defaultApk) {
    & (Join-Path $projectRoot 'deploy\opi4\scripts\update-package-hashes.ps1')
}
