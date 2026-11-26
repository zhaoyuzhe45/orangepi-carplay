param([Parameter(Mandatory=$true)][string]$Device,[int]$AdbPort=5038,
      [ValidateRange(1,14400)][int]$Seconds=180,[string]$Output)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (!$Output) { $Output=Join-Path $root ('build/field-streams-'+(Get-Date -Format yyyyMMdd-HHmmss)+'.jsonl') }
$serial=@('-P',"$AdbPort",'-s',$Device)
$port=(& adb @serial forward tcp:0 tcp:49152)
if ($LASTEXITCODE -or "$port".Trim() -notmatch '^\d+$') {throw 'Cannot forward the local CarPlay status port'}
$port="$port".Trim()
$probe=@'
printf 'boot='; cat /proc/sys/kernel/random/boot_id
printf 'uptime='; cut -d ' ' -f 1 /proc/uptime
printf 'load='; cut -d ' ' -f 1 /proc/loadavg
printf 'usb='; cat /sys/class/udc/*/state
printf 'encoderLogTail='; tail -c 2048 /data/carplay/logs/encoder.err | tr -d '\000' | grep -oE 'keyframe ptsUs=[0-9]+' | tail -1
printf '\n'
logcat -d -v monotonic -s CarPlayVehicleLocation:I -t 600 | grep 'sample rmcSeq=' | tail -1
'@
try {
    $stop=[Diagnostics.Stopwatch]::StartNew(); $i=0
    do {
        $row=[ordered]@{hostTime=[DateTimeOffset]::Now.ToString('o');hostElapsedMs=$stop.ElapsedMilliseconds}
        try {
            $status=Invoke-RestMethod -Uri "http://127.0.0.1:$port/api/status" -TimeoutSec 2 -ErrorAction Stop
            $location=$status.vehicle.location
            foreach ($key in 'instance','generation','active','received','rmcSequence','rmcAgeMs','ggaSequence','ggaAgeMs') {$row[$key]=$location.$key}
            $row['session']=$status.state; $row['audioStreams']=$status.audio.streams
        } catch { $row['statusError']=$_.Exception.GetType().Name }
        if ($i % 4 -eq 0) {
            $lines=(($probe -replace "`r",'')+"`nexit 0`n") | & adb @serial shell 'su -c sh' 2>$null
            foreach ($line in $lines) {
                if ($line -match '^(boot|uptime|load|usb|encoderLogTail)=(.*)$') { $row[$Matches[1]]=$Matches[2].Trim() }
                elseif ($line -match 'sample rmcSeq=') {
                    if ($line -match '^\s*(\d+\.\d+)') {$row['androidLogTime']=$Matches[1]}
                    foreach ($m in [regex]::Matches($line,'\b(rmcSeq|ggaSeq|rmcArrivalMs|ggaArrivalMs|contentAgeMs|httpMs|applyMs)=(-?\d+)')) { $row['android_'+$m.Groups[1].Value]=[long]$m.Groups[2].Value }
                }
            }
        }
        $row | ConvertTo-Json -Depth 2 -Compress | Add-Content -LiteralPath $Output -Encoding utf8
        $i++; if ($stop.Elapsed.TotalSeconds -lt $Seconds) {Start-Sleep -Milliseconds 1000}
    } while ($stop.Elapsed.TotalSeconds -lt $Seconds)
    Get-Item -LiteralPath $Output | Select-Object FullName,Length
} finally { & adb @serial forward --remove "tcp:$port" | Out-Null }
