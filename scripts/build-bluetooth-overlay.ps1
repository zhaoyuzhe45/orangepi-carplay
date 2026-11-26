param([string]$OutputDirectory='build/bluetooth-20261112/overlay')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$bt=(Join-Path $env:ANDROID_SDK_ROOT 'build-tools/30.0.2')
$jar=(Join-Path $env:ANDROID_SDK_ROOT 'platforms/android-30/android.jar')
New-Item -ItemType Directory -Force $out | Out-Null
& "$bt/aapt.exe" package -f -M "$root/system-patch/bluetooth-hfp/AndroidManifest.xml" -S "$root/system-patch/bluetooth-hfp/res" -I $jar -F "$out/unsigned.apk"
if($LASTEXITCODE){throw 'Overlay resource build failed'}
& "$bt/zipalign.exe" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Overlay alignment failed'}
& "$bt/apksigner.bat" sign --ks "$root/config/signing/carplay-audio.keystore" --ks-key-alias CARPLAY --ks-pass "file:$root/config/signing/carplay-audio.password" --v4-signing-enabled false --out "$out/CarPlayBluetoothHfpOverlay.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Overlay signing failed'}
& "$bt/apksigner.bat" verify "$out/CarPlayBluetoothHfpOverlay.apk"
if($LASTEXITCODE){throw 'Overlay signature verification failed'}
Get-FileHash "$out/CarPlayBluetoothHfpOverlay.apk"
