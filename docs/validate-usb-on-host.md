# 连接与现场诊断

从项目根目录运行 PowerShell，使用 TCP ADB：

```powershell
$adb = 'D:/Software/Android/SDK/platform-tools/adb.exe'
$d = '<ADB地址:端口>'
& $adb -P 5038 -s $d shell getprop ro.product.device
& $adb -P 5038 -s $d shell getprop init.svc.carplay_box
& $adb -P 5038 -s $d shell cat /proc/sys/kernel/random/boot_id
& $adb -P 5038 -s $d shell su 0 sh /data/carplay/bin/usb_lock.sh status
```

完整快照使用 `tools/diagnostics/state_dump.sh`；枚举跟踪使用 `capture-usbhost-enum.sh`。设备日志位于 `/data/carplay/logs`，同时读取 Android logcat 和内核 dmesg；控制台导出只含主日志尾部。

连接耗时按 USB 枚举、iAP2 认证、NCM 地址就绪、RTSP SETUP/RECORD、首个真实画面分别计算。自动校时可能使墙钟跳变，应结合单调时间、PID、boot ID 和 session 判断；服务 running 或 RECORD 成功不能代替出图。

重连时检查先发生的事件：USB disconnect、FunctionFS 停用、角色失败、协议超时、进程退出或 boot ID 改变。日志只能证明事件顺序，不能仅凭 USB 断开认定线缆接触不良。

音频故障同时记录媒体/双向通道、音频流配置、PCM 是否非零和 Android 活跃录音。播放器进度走动不等于车机已收到有效声音。

定位长时采集：

```powershell
.\tools\diagnostics\capture-field-streams.ps1 -Device '<ADB地址:端口>' -Seconds 1800
```

采集包含状态、RMC/GGA 序列和到达年龄、Android 应用耗时、USB 与编码器状态。比较 GPS UTC 与可信 UTC 的差值，以及原始 NMEA 内容是否持续推进；到达时间很新仍可能是车机积压的数据。原始对照资料保留在 `build/gps-lag-20261121`。采集工具会建立临时 ADB forward，结束后移除。
