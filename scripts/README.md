# 构建、测试与部署入口

以下示例均从项目根目录运行 PowerShell；设置 `ANDROID_SDK_ROOT`、`JAVA_HOME`，将 Python 和 platform-tools 加入 PATH。首次使用按[构建说明](../docs/building.md)准备本地身份、签名和固件依赖。

| 脚本 | 作用 |
|---|---|
| prepare-local-identity.py | 导入/生成本地 RSA TLS 开发身份；生成头文件被 Git 忽略 |
| prepare-dependencies.py / dependencies.lock.json | 下载固定头文件、导入原厂固件库并校验 SHA256 |
| check-public-source.py | 检查 Git 可见文件和暂存区，不打印密钥内容 |
| build.ps1 | `-Architecture All/AArch64/ARMv7` 编译 native；仅 AArch64 可加 `-PublishFinal` 同时更新主程序和编码器 JAR |
| build-encoder.ps1 | 生成 MediaCodec helper；`-OutputJar` 指定输出，不自动部署 |
| build-audio-apk.ps1 / build-phone-apk.ps1 | 生成签名 APK，默认写 deploy/opi4/apk |
| build-vehicle-gnss.ps1 / fetch-gnss-build-headers.py | 当前 Android 8.1 固件的 GNSS HAL 构建及头文件准备 |
| build-shared-microphone-hal.ps1 / build-shared-microphone-status.ps1 | 构建板载麦克风共享 HAL 与状态探针 |
| install-shared-microphone.ps1 | 共享麦克风专用安装、核验、更新与恢复 |
| test-shared-microphone-relay.ps1 | 共享音频转发本机回归 |
| build-vehicle-gnss-probe.ps1 | 构建使用固定坐标的台架 GPS/速度探针，不自动安装或运行 |
| test.ps1 | 编译 native 测试；带 `-Device`、`-ExpectedProduct` 时推送执行 |
| test-audio-routing.ps1 / test-queue-navigation.ps1 | 纯 Java 回归；`-BuildDeviceProbe` 另行构建设备探针 |
| test-bluetooth-network.ps1 | Wi-Fi 优先与 PAN 备用策略回归；`-BuildDeviceProbe` 构建真实手机联网探针 |
| test-phone-ui.ps1 / build-bluetooth-probe.ps1 / build-app-fixture.ps1 | 电话、蓝牙及应用联调探针 |
| test-time-sync.ps1 | 在设备上修改并读回 UTC，需要按参数指定目标时间 |
| deploy-opi4.ps1 | 转调交付包部署器；`-InstallVehicleGnss` 添加一次性 GNSS 安装 |
| install-vehicle-gnss.ps1 | 核对固件、备份并安装 GNSS；`-CheckOnly` 仅检查，安装后需重启一次 |
| verify-package.ps1 | 验证交付哈希、ELF 与 APK 签名 |
| update-package-manifest.ps1 | 交付文件变更后重新生成 SHA256SUMS |
| verify-syntax.ps1 | 只解析 PowerShell 语法，不执行脚本 |
| build-bluetooth-overlay.ps1 / build-bluetooth-classic.ps1 | 当前固件专用蓝牙适配构建 |
| new-audio-keystore.ps1 / carplay-keystore.ps1 | 签名密钥创建和本机口令管理 |

先校验构建结果，再更新交付清单和部署；不能用重算哈希代替功能验证。Audio/Phone 使用同一签名。设备探针构建依赖 `build/android-audio/classes`，电话探针还依赖 Phone 构建输出，需先运行相应 APK 构建。

宿主 shell、Python、编码器测试另见 [tests](../tests/README.md)。部分测试仍向日期命名的 `build` 子目录输出，不能据目录日期判定所有文件都已无用。
