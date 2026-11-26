param(
    [string]$OutputApk='',
    [string]$Keystore='',
    [string]$Alias='CARPLAY',
    [string]$StorePassword='',
    [string]$KeyPassword=''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/android-phone'
$jdk=@($env:JAVA_HOME) |
    Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'bin/javac.exe')) } | Select-Object -First 1
$sdk=@($env:ANDROID_SDK_ROOT,$env:ANDROID_HOME) |
    Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'build-tools/30.0.2/aapt.exe')) } | Select-Object -First 1
if(-not $jdk -or -not $sdk){throw 'Phone build requires JDK and Android SDK build-tools 30.0.2'}
$bt=Join-Path $sdk 'build-tools/30.0.2'
$jar=Join-Path $sdk 'platforms/android-30/android.jar'
if(-not $OutputApk){$OutputApk=Join-Path $root 'deploy/opi4/apk/CarPlayPhone.apk'}
if(-not $Keystore){$Keystore=Join-Path $root 'config/signing/carplay-audio.keystore'}
if(-not $StorePassword){$StorePassword=$env:CARPLAY_AUDIO_KEYSTORE_PASSWORD}
if(-not $StorePassword){$StorePassword=[IO.File]::ReadAllText((Join-Path $root 'config/signing/carplay-audio.password')).TrimEnd([char[]]@("`r","`n"))}
if(-not $KeyPassword){$KeyPassword=$StorePassword}
$env:JAVA_HOME=$jdk
foreach($leaf in @('classes','gen','dex')){
    $path=[IO.Path]::GetFullPath((Join-Path $out $leaf))
    if(-not $path.StartsWith(([IO.Path]::GetFullPath($out)+'\'),[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid build path'}
    if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Recurse -Force}
    New-Item -ItemType Directory -Force $path | Out-Null
}
$source=Join-Path $root 'src/android_phone'
& "$bt/aapt.exe" package -f -m -J "$out/gen" -M "$source/AndroidManifest.xml" -S "$source/res" -I $jar
if($LASTEXITCODE){throw 'Phone resources failed'}
$files=@(Get-ChildItem "$source/src","$out/gen" -Filter '*.java' -Recurse | ForEach-Object FullName)
foreach($shared in @('Ui','BluetoothCalls','PhoneUiModel','PhoneContacts','PhoneState','PhoneBridgeContract')){$files+=Join-Path $root "src/android_audio_capture/src/com/carplaybox/audio/$shared.java"}
& "$jdk/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -cp $jar -d "$out/classes" @files
if($LASTEXITCODE){throw 'Phone Java failed'}
if(Test-Path "$out/classes/com/carplaybox/audio/BluetoothCallBridge.class"){throw 'Phone APK must not own the Bluetooth service'}
$classes=@(Get-ChildItem "$out/classes" -Filter '*.class' -Recurse | ForEach-Object FullName)
& "$bt/d8.bat" --lib $jar --min-api 26 --output "$out/dex" @classes
if($LASTEXITCODE){throw 'Phone DEX failed'}
& "$bt/aapt.exe" package -f -M "$source/AndroidManifest.xml" -S "$source/res" -I $jar -F "$out/unsigned.apk" "$out/dex"
if($LASTEXITCODE){throw 'Phone packaging failed'}
& "$bt/zipalign.exe" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Phone alignment failed'}
$target=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputApk)
New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
$oldStore=$env:CARPLAY_PHONE_SIGN_STORE
$oldKey=$env:CARPLAY_PHONE_SIGN_KEY
try {
    $env:CARPLAY_PHONE_SIGN_STORE=$StorePassword
    $env:CARPLAY_PHONE_SIGN_KEY=$KeyPassword
    & "$bt/apksigner.bat" sign --ks $Keystore --ks-key-alias $Alias --ks-pass env:CARPLAY_PHONE_SIGN_STORE --key-pass env:CARPLAY_PHONE_SIGN_KEY --v4-signing-enabled false --out $target "$out/aligned.apk"
    if($LASTEXITCODE){throw 'Phone signing failed'}
} finally {
    $env:CARPLAY_PHONE_SIGN_STORE=$oldStore
    $env:CARPLAY_PHONE_SIGN_KEY=$oldKey
}
& "$bt/apksigner.bat" verify $target
if($LASTEXITCODE){throw 'Phone signature failed'}
Get-FileHash $target
if($target -eq [IO.Path]::GetFullPath((Join-Path $root 'deploy/opi4/apk/CarPlayPhone.apk'))){
    & (Join-Path $root 'deploy/opi4/scripts/update-package-hashes.ps1')
}
