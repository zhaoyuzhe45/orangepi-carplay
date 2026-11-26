param(
    [string]$Module,
    [string]$AudioApk,
    [Parameter(Mandatory=$true)][string]$Device,
    [int]$AdbPort=5038,
    [ValidateSet('Check','Apply','Update','Verify','Rollback')][string]$Mode='Check',
    [switch]$RefreshFramework
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$stage='/data/local/tmp/shared-mic-stage-20261119'
$statusJar=Join-Path $root 'build/shared-microphone/status/shared-mic-status.jar'
if(!(Test-Path -LiteralPath $statusJar)){$statusJar=Join-Path $root 'bin/shared-microphone/shared-mic-status.jar'}
if(!(Test-Path -LiteralPath $statusJar)){throw 'Run scripts/build-shared-microphone-status.ps1 first.'}
$installer=Join-Path $root 'system-patch/shared-microphone/install_shared_microphone.sh'
if(!(Test-Path -LiteralPath $installer)){$installer=Join-Path $PSScriptRoot 'install_shared_microphone.sh'}
if(!(Test-Path -LiteralPath $installer)){throw 'Shared microphone device installer missing.'}
function Adb-Checked([string[]]$Arguments) {
    $lines=& adb -P $AdbPort -s $Device @Arguments 2>&1
    if($LASTEXITCODE){throw "ADB failed: $($lines -join [Environment]::NewLine)"}
    return ($lines -join [Environment]::NewLine)
}
if($Mode -eq 'Rollback') {
    Adb-Checked @('shell','mkdir','-p',$stage) | Write-Output
    Adb-Checked @('push',$statusJar,"$stage/shared-mic-status.jar") | Write-Output
    Adb-Checked @('push',$installer,"$stage/install_shared_microphone.sh") | Write-Output
    Adb-Checked @('shell','su','0','sh',"$stage/install_shared_microphone.sh",'rollback') | Write-Output
    return
}
if(!$Module -or !$AudioApk){throw '-Module and -AudioApk are required except for Rollback.'}
$moduleFile=(Resolve-Path -LiteralPath $Module).Path
$apkFile=(Resolve-Path -LiteralPath $AudioApk).Path
$moduleHash=(Get-FileHash -LiteralPath $moduleFile -Algorithm SHA256).Hash.ToLowerInvariant()
$apkHash=(Get-FileHash -LiteralPath $apkFile -Algorithm SHA256).Hash.ToLowerInvariant()
$bytes=[IO.File]::ReadAllBytes($moduleFile)
if($bytes.Length -lt 20 -or $bytes[0] -ne 127 -or $bytes[1] -ne 69 -or $bytes[2] -ne 76 -or
   $bytes[3] -ne 70 -or $bytes[4] -ne 1 -or $bytes[5] -ne 1 -or $bytes[18] -ne 40 -or $bytes[19] -ne 0){
    throw 'Shared mic module must be an ELF32 little-endian ARM library.'
}
Adb-Checked @('shell','mkdir','-p',$stage) | Write-Output
Adb-Checked @('push',$moduleFile,"$stage/audio.r_submix.shared.so") | Write-Output
Adb-Checked @('push',$apkFile,"$stage/CarPlayAudio.apk") | Write-Output
Adb-Checked @('push',$statusJar,"$stage/shared-mic-status.jar") | Write-Output
Adb-Checked @('push',$installer,"$stage/install_shared_microphone.sh") | Write-Output
$refresh=if($RefreshFramework){'refresh-framework'}else{'no'}
$result=Adb-Checked @('shell','su','0','sh',"$stage/install_shared_microphone.sh",$Mode.ToLowerInvariant(),$moduleHash,$apkHash,$refresh)
Write-Output $result
if($result -match 'FRAMEWORK_REFRESH_REQUIRED') {
    try {
        $previousSystem=Adb-Checked @('shell','pidof','system_server')
        Adb-Checked @('shell','su','0','setprop','ctl.restart','zygote') | Write-Output
        $ready=$false
        for($attempt=0;$attempt -lt 45;$attempt++) {
            Start-Sleep -Seconds 2
            $nextSystem=& adb -P $AdbPort -s $Device shell pidof system_server 2>$null
            if(!$nextSystem -or ($nextSystem -join '') -eq $previousSystem.Trim()){continue}
            $permissions=& adb -P $AdbPort -s $Device shell dumpsys package com.carplaybox.audio 2>$null
            if(($permissions -join "`n") -match 'CAPTURE_AUDIO_HOTWORD: granted=true'){$ready=$true;break}
        }
        if(!$ready){throw 'Collector permission not granted after framework restart.'}
        $result=Adb-Checked @('shell','su','0','sh',"$stage/install_shared_microphone.sh",'verify',$moduleHash,$apkHash)
        Write-Output $result
    } catch {
        Write-Warning 'Framework verification failed; restoring the original pair.'
        Adb-Checked @('shell','su','0','sh',"$stage/install_shared_microphone.sh",'rollback') | Write-Output
        throw
    }
}
if($Mode -in @('Apply','Update','Verify') -and $result -notmatch 'INSTALLED_PENDING_FUNCTIONAL_VERIFICATION') {
    throw 'Installation did not report completion. Run Verify before using the candidate.'
}
Write-Output "Module SHA256=$moduleHash"
Write-Output "Audio APK SHA256=$apkHash"
