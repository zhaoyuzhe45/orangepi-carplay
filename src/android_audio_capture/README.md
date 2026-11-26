# Android 后台、控制台与播放队列

包名 `com.carplaybox.audio`；独立电话 UI 为 `com.carplaybox.phone`，通过同签名 Provider 使用本包的 HFP/PBAP 后台。

| 实现 | 职责 |
|---|---|
| AudioCaptureService / SplitAudioCapture / NavigationMixer | 48 kHz 立体声媒体、指定导航 UID 分流与播报压低音乐 |
| CarMicrophoneBridge / MicrophoneDemandPolicy | 按应用的实际录音请求车载麦克风 |
| VehicleLocationService / VehicleAndroidProvider / VehicleGnssChannel | NMEA 合并、标准 GNSS 通道、权限及 provider 生命周期 |
| AppIntegrationService | MediaSession、歌名封面、队列、导航广播与通信状态 |
| BluetoothCallBridge / BluetoothContacts | 单路电话控制、DTMF 与 PBAP 同步 |
| BluetoothNetworkBridge / BluetoothPanPolicy | Wi-Fi 优先、手机蓝牙 PAN 备用与自动重连；后台线程处理 |
| BootReceiver / CarPlayServices | 开机或更新后启动后台服务 |
| MainActivity / MediaQueueActivity | 控制台、日志、电话摘要与全屏播放队列 |

动态音频策略按锁串行创建/释放，录音启动失败有限重建。导航包未安装时使用单媒体 mix，所有播放器 mix 排除虚拟麦克风，避免回声。

关闭设置中的“启用车机麦克风”，Android 应用继续走本地麦克风，盒子不会为录音切换车机主音频通道。开启此项并连接车机后，外部应用使用物理麦克风时即请求车载输入，支持前后台和常驻唤醒；最后一个应用结束录音后保留 250 ms，再释放。盒子自身采集、媒体回采以及通话上下行录制不会触发。音乐不因录音而静音；播放器的音频焦点处理和已有导航播报压低规则照常生效。不连接车机会使用原有 Android 输入。当前奥迪 legacy 协商在媒体与双向音频间切换：媒体为 48 kHz 立体声，启用车载麦克风时降为 16 kHz 单声道，常驻唤醒期间也如此。

定位合并同一观察的 RMC/GGA，避免后到的 GGA 丢失速度；5 秒过期、无定位和断连撤销更新。当前固件优先通过车机 GNSS HAL 发布真实 gps，再由系统转发至 passive；发送 ACK 还须通过 gps 回读校验，passive 单独成功不算应用。保留旧硬件接口与 mock 路径。

媒体音乐已获用户实车确认；开启车机麦克风后的常驻录音、语音搜索和真实通话仍需听测。GPS 长时间连接后的数据延迟仍待实车复测，见[实车验收](../../docs/audi-a4l-validation.md)。

在项目根运行 `scripts/build-audio-apk.ps1` 和 `scripts/build-phone-apk.ps1`。默认输出签名 APK 到 `deploy/opi4/apk/`；Audio 支持 `-OutputApk`。两包必须同签名，`-SkipSign` 产物不能部署。系统音频/媒体/定位权限需随整套安装配置。

控制台连接回环 JSON API，native 与后台通过私有 Unix socket 通信。音乐队列依赖播放器共享 MediaSession；导航源和压低比例尚未配置化，控制台导出仅含主日志尾部。

蓝牙备用上网默认关闭，开关及手机选择持久化；重启保留选择，清除应用数据后恢复关闭。手动启用后，Wi-Fi 同时具有 INTERNET 和 VALIDATED 才进入优先状态并断开 PAN；否则允许连接手机 NAP。遍历全部网络，避免 Android 将蓝牙设为默认后漏掉 Wi-Fi；网络回调配合 3 秒轮询，失败按 5–60 秒退避。仅在没有指定手机且唯一已配对手机提供 NAP 时自动选择，不猜测多台手机。不修改路由、DNS、进程网络绑定或电话音频通道。
