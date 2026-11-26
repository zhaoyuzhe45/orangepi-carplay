# OPi4 交付包

适用 Orange Pi 4 LTS / RK3399、Android 8.1、`ro.product.device=rk3399_mid`。默认运行 AArch64；ARMv7 二进制用于兼容回归。功能及待验收范围见[项目说明](../../README.md)。

这是本地交付包的目录约定。Git 只包含配置模板、脚本和说明；`bin`、签名 APK 和 SHA256SUMS 由本地构建/组包产生。首次获取源码先完成[构建与依赖准备](../../docs/building.md)，不要直接运行安装器。

## 包内容

| 路径 | 用途 |
|---|---|
| bin/iap2d、bin/iap2d-armv7 | 协议与媒体守护进程 |
| bin/carplay-encoder.jar | MediaCodec 硬件 H.264 编码器 |
| bin/gnss/*.so | 当前固件专用标准 GNSS HAL 及同固件接口库 |
| apk/CarPlayAudio.apk | 音频、定位、应用集成、控制台与播放队列 |
| bin/shared-microphone、scripts/install-shared-microphone.ps1 | 已验证固件的板载麦克风共享模块、状态探针及专用安装器 |
| apk/CarPlayPhone.apk | 同签名独立电话 UI |
| apk/CarPlayBluetoothHfpOverlay.apk、bin/bluetooth.default.arm64.so | 当前固件专用蓝牙适配 |
| apk/carplay-audio-default-permissions.xml | 系统应用默认权限 |
| bin/frpc、config、scripts | 远程维护、显示预设、安装及运行脚本 |
| SHA256SUMS | 交付文件校验清单 |

## 检查与部署

项目根目录 PowerShell：

```powershell
.\scripts\verify-package.ps1
$env:ANDROID_ADB_SERVER_PORT = '5038'
.\scripts\deploy-opi4.ps1 -Device '<ADB地址:端口>' -ExpectedProduct rk3399_mid -InstallBoot
```

设备需要 root。安装前核对设备身份和回滚副本；首次 init、权限或蓝牙覆盖层变更可能需要重启。安装后同时核对运行中的 APK 与系统副本。新板还需要匹配的内核/resource、NCM 和 USB 角色切换支持。

当前固件原本没有 gps 提供器。新板部署时在上面的命令添加 `-InstallVehicleGnss`，安装标准 GNSS HAL 与 manifest 条目，再重启一次。已部署 Audio 的设备也可单独运行 `scripts/install-vehicle-gnss.ps1 -Device '<ADB地址:端口>' -AdbPort 5038`；`-CheckOnly` 仅检查。安装器核对 Android 版本、系统接口库、active/system APK 和已有 HAL，保留备份并拒绝未知实现；目前仅支持已验证的 Permissive 固件，不改 SELinux 策略。

定位优先由 GNSS 回调进入 gps，再由 Android 分发至 passive；合并同一时刻的 RMC/GGA 保留速度、方向和高度。保留旧硬件接口与 mock 兼容路径，但 passive 单独更新不算成功。GPS 开关和精确定位权限必须开启。系统特权权限仍需 `/system/priv-app/CarPlayAudio/` 的包扫描。

GNSS 安装是一次性操作，不加入开机脚本或启动等待。GPS 长时间连接后的数据延迟仍待实车复测。

`-Build` 会重编 native，`-RebuildAudio` 会重编 Audio/Phone；不要为了 JAR 打包时间戳差异替换已验证编码器。Audio/Phone 必须保持同签名。更新交付文件后运行 `scripts/update-package-manifest.ps1` 和 `scripts/verify-package.ps1`。

## 板载麦克风共享

Audio APK 使用一次板载采集向多个普通录音应用分发。共享本身不请求音乐焦点、不静音、不启用车机麦克风；应用自身仍可申请音频焦点。只更新 APK 不会安装所需 HAL，未检测到已验证模块或权限时共享保持禁用。首次启用必须用专用安装器，不能直接覆盖系统库。

在交付包目录执行以下命令；新板先完成普通部署。`Apply` 仅接受验证过的原厂模块，`-RefreshFramework` 会短暂重启 Android 框架以扫描 HOTWORD 特权权限。已启用共享的盒子更新用 `Update`，无需此参数。

```powershell
.\scripts\install-shared-microphone.ps1 -Mode Apply -Module .\bin\shared-microphone\audio.r_submix.fanout.production.so -AudioApk .\apk\CarPlayAudio.apk -Device '<ADB地址:端口>' -RefreshFramework
.\scripts\install-shared-microphone.ps1 -Mode Verify -Module .\bin\shared-microphone\audio.r_submix.fanout.production.so -AudioApk .\apk\CarPlayAudio.apk -Device '<ADB地址:端口>'
# 恢复安装前原厂 HAL 和 Audio APK：
.\scripts\install-shared-microphone.ps1 -Mode Rollback -Device '<ADB地址:端口>'
```

安装器保留 `/data/carplay/backups/shared-mic-20261119`，核对实际加载的模块及 active/base APK。模块机制与构建见[共享麦克风说明](../../system-patch/shared-microphone/README.md)，音质与实车通道切换按[验收清单](../../docs/audi-a4l-validation.md)复测。

## 运行

`carplay_box` 由 init 启动，supervisor 管理主进程与 USB 角色；先呈现 Apple gadget，收到有效请求后切换 Host，失败按预算恢复。Audio 的音频、定位和应用集成服务开机自启，Phone UI 不强制弹出。控制 API 监听 `127.0.0.1:49152`。

蓝牙备用上网默认关闭，重启保留开关，清除应用数据后恢复关闭。手动启用后由 AppIntegrationService 后台管理：可上网的 Wi-Fi 优先，Wi-Fi 断开或系统检测不能联网时连接已配对手机的蓝牙共享，Wi-Fi 恢复后断开 PAN 并切回。控制台“蓝牙电话”页可关闭备用功能，电话应用负责选择手机；手机端需开启蓝牙网络共享。不开机等待网络，不改变电话音频路由。

只读检查：

```powershell
$adb = Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe'
$d = '<ADB地址:端口>'
& $adb -P 5038 -s $d shell getprop ro.product.device
& $adb -P 5038 -s $d shell getprop init.svc.carplay_box
& $adb -P 5038 -s $d shell wm size
& $adb -P 5038 -s $d shell su 0 sh /data/carplay/bin/usb_lock.sh status
```

未接车时 `usb-wait` / `not attached` 可以是正常等待；验收需确认真实画面、输入、音频和重连，不能仅看服务 running。

## 物理显示与开机缓存

预设为 `800x480`、`1024x480`、`1280x600`、`1280x720`、`1920x900`、`1920x1080`，均为 60 Hz，density 范围 120–480。其它有效尺寸选择最近预设，再将完整源画面缩放编码成接收端尺寸；奇数宽高向下取偶。编码填充和 SPS 裁剪用于尺寸对齐。

需要切换物理模式时先结束握手并释放媒体，通过有锁、有期限、读回及回滚的准备流程切换，再重连。首个真实关键帧成功发送后才核验并原子保存 `/cache/carplay/display-profile.conf`。仅准备完成或收到 RECORD 不写成功缓存。

首次启动或缓存无效时默认 `800x480@60`、density 160。init 在 `post-fs-data` 清理强制 WM 设置，`early-boot` 在图形启动前应用缓存；FDE 属性重放后重申活动预设。开机路径不轮询、不 sleep、不重启图形服务。

显示和旋钮已获用户实车确认；冷启动耗时、其他车型和尺寸组合按[验收清单](../../docs/audi-a4l-validation.md)检查。

## 日志与回滚

诊断读取主日志、supervisor、usb-state 和 usb-role 快照，结合 PID、boot ID、单调时间判断。控制台导出仅包含主日志尾部。

使用对应批次 `/data/carplay/backups/` 的文件回滚；软件、蓝牙库/overlay、kernel/resource 各有独立流程。设备备份是否可用须现场核对，本地固件备份单独保存，不包含在源码仓库中。
