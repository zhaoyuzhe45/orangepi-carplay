# 测试入口

在项目根目录运行。本机回归、设备探针和实车验收分别执行；已修复问题的测试继续保留，防止回归。

| 命令 / 入口 | 范围 |
|---|---|
| `python tests/test_source_release.py` | Git 公开范围、暂存区泄露拦截及本地身份生成 |
| `scripts/test.ps1 -Architecture All` | 编译 native 测试；不带设备参数时仅编译 |
| `scripts/test.ps1 -Architecture AArch64 -Device '<ADB地址:端口>' -ExpectedProduct rk3399_mid` | 推送并执行设备 native 测试 |
| `tests/java/run-encoder-tests.ps1` | 编码配置、缩放、包边界与生命周期 |
| `tests/java/run-location-poll-tests.ps1` | 定位轮询与耗时补偿 |
| `tests/java/run-media-capture-tests.ps1` | 媒体采集状态 |
| `scripts/test-audio-routing.ps1` | 实际录音需求及音频策略 |
| `scripts/test-queue-navigation.ps1` | 导航混音、播放队列及应用快照 |
| `scripts/test-bluetooth-network.ps1` | 默认关闭、Wi-Fi 优先、PAN 选择及退避 |
| `scripts/test-shared-microphone-relay.ps1` | 多读者音频转发 |
| `python tests/test_bluetooth_classic_patch.py` | ARM64 蓝牙补丁仿真 |
| `python tests/vehicle_android_provider_test.py` | Android 定位 provider 生命周期；需要 JDK |
| `python3 tests/run_review_fixes.py` | Linux/WSL 连接、显示、输入回归；`--baseline` 验证保留故障基线 |
| `python3 tests/run_shell_regressions.py` | Linux/WSL 下全部 shell 回归 |
| `bash tests/run_shared_microphone_hal_test.sh` | 共享麦克风 HAL 主机测试 |
| `sh tests/vehicle_gnss_install_test.sh` | 隔离文件系统下 GNSS 安装与失败恢复 |

native 完整列表以 `scripts/test.ps1` 为准，包括 iAP2、NCM、显示、音频、按键、车辆数据和校时。`screenrecord_pipeline_test.sh` 虽沿用旧文件名，检查的是当前 MediaCodec 路径。

shell 的 mDNS 用例需要 root 创建隔离网络，`usb_role_switch_test.sh` 的只读场景需要普通用户；不要把整批 root 执行结果当作各场景均有效。

## 设备探针

`-BuildDeviceProbe` 只构建探针，安装和执行另行进行。探针依赖 `build/android-audio/classes`，电话探针还依赖 Phone classes；先运行相应 APK 构建。设备脚本可能重启服务、切换网络或注入位置，应在相应台架条件下执行。

- `device/`：USB、NCM、输入、单例与恢复检查。
- `carplay_scaled_encoder_probe.c`、`carplay_display_device_probe.c`：编码尺寸与物理切换。
- `scripts/build-vehicle-gnss-probe.ps1`：固定坐标 GNSS 探针，仅在盒子未接车时运行；完成后卸载探针、清除测试位置缓存并恢复 GPS 开关。
- `BluetoothNetworkDeviceTest`：真实手机共享与 Wi-Fi 切换，可能中断远程 ADB；结束后恢复后台服务。
- `vehicle_gnss_hal_test.cpp`：GNSS ABI 与生命周期，不与生产 GNSS 同时占用 socket。

## 测试输入

`fixtures/`、`java/` 和测试源码随仓库分发。蓝牙原库和 GNSS 接口库需按[构建说明](../docs/building.md)自行提供；Unicorn 固定为 2.1.4，guard 由蓝牙构建生成。DiPlay APK 不随仓库分发。`--baseline` 仅供持有历史故障基线的本机使用，不属于公开回归前提。`tests/build` 和其他编译输出可以重建。

当前待验收项目和 GPS 长时延迟检查见[实车验收](../docs/audi-a4l-validation.md)。
