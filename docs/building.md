# 从源码构建

支持的宿主入口为 Windows PowerShell，目标为 Orange Pi 4 LTS / RK3399 Android 8.1。维护者使用的固件源码来自香橙派官网的 Orange Pi 4 LTS Android 8.1 源码包。源码仓库不包含本机私钥、签名 APK、固件库、镜像或第三方工具安装包。Git 忽略规则不删除本地文件，也不妨碍构建脚本继续使用它们。日常主程序和 APK 修复无需重建整套 Android 固件；设备安装另外执行，不属于构建流程。

## 工具链

安装 Git、Python 3.12、JDK 8、OpenSSL，以及 Android SDK 的 NDK `22.1.7171670`、build-tools `30.0.2`、platforms `android-30`、platform-tools。设置 `JAVA_HOME`、`ANDROID_SDK_ROOT`，将 Python、OpenSSL 和 SDK platform-tools 加入 PATH。自定义 NDK 位置可设置 `ANDROID_NDK_ROOT`，主守护进程构建使用 SDK 内的固定版本。

以下命令均从项目根目录执行。仅重新生成图标/占位视频时才需要带 libx264 的 ffmpeg；已生成的中性素材包含在源码中。

## 本地身份与应用签名

开发环境首次运行：

```powershell
python scripts/prepare-local-identity.py --generate
.\scripts\new-audio-keystore.ps1
```

已持有自己的 TLS 身份时，将 RSA PEM 证书和私钥分别放到 `config/certs/lockdown-cert.pem`、`lockdown-key.pem`，运行不带 `--generate` 的准备脚本。它校验配对后生成两个被忽略的头文件。自签身份仅用于开发，不替代 MFi 认证，也不保证所有接收端接受。

已有设备继续使用原来的 TLS 身份和 Audio/Phone keystore；不要重新生成它们。签名文件不进入源码仓库，需独立备份。Audio 和 Phone 必须使用同一签名。

## 主程序与应用

```powershell
.\scripts\build.ps1 -Architecture All
.\scripts\build-audio-apk.ps1 -OutputApk build\release\CarPlayAudio.apk
.\scripts\build-phone-apk.ps1 -OutputApk build\release\CarPlayPhone.apk
.\scripts\build-bluetooth-overlay.ps1 -OutputDirectory build\release\overlay
```

native 与编码器输出到 `build/opi4`；`-Architecture AArch64 -PublishFinal` 同时复制主程序和编码器 JAR 到本地交付目录。上述 APK 示例输出到构建目录，便于检查后再组包。

## GNSS、蓝牙和麦克风适配

这些组件依赖匹配 ROM。先准备公开头文件：

```powershell
python scripts/prepare-dependencies.py --headers
# 如有需要可添加 --proxy http://代理地址:端口
```

下载使用 AOSP `android-8.1.0_r1` 和 VNDK 固定提交；全部 390 个头文件用 `scripts/dependencies.lock.json` 校验。下载失败不会用空文件替代依赖。

从自己的原厂固件提取以下目录，保留相对路径。清单中的 8 个文件及其 SHA256 是本版本支持的原始输入，不能使用已经打过补丁的蓝牙库：

- `system/lib64/`：`android.hardware.gnss@1.0.so`、`libc++.so`、`libcutils.so`、`libhidlbase.so`、`libhidltransport.so`、`liblog.so`、`libutils.so`。
- `system/lib64/hw/bluetooth.default.so`：原厂 ARM64 蓝牙库。

```powershell
python scripts/prepare-dependencies.py --firmware-root 'D:\firmware-extracted' --check
.\scripts\build-vehicle-gnss.ps1
.\scripts\build-bluetooth-classic.ps1
.\scripts\build-shared-microphone-hal.ps1
.\scripts\build-shared-microphone-status.ps1
```

GNSS 默认输出到 `build/vehicle-gnss`，蓝牙到 `build/bluetooth-classic`，共享麦克风到 `build/shared-microphone`。导入器只操作本地文件，全部固件哈希通过后才写入 `build/deps`。其他 ROM 需重新适配和验收，不能仅修改校验值绕过检查。

平台签名仅在相关系统适配需要时自行提供匹配 SDK 的密钥，不随公开源码分发。内核、resource 分区和刷机流程见[恢复说明](flash-boot-and-adb-recovery.md)。

## 检查与组包

```powershell
python tests/test_source_release.py
.\scripts\verify-syntax.ps1
.\tests\java\run-encoder-tests.ps1
.\scripts\test.ps1 -Architecture All
# 可选蓝牙 ARM64 仿真，先在自己的 Python 虚拟环境安装：
python -m pip install unicorn==2.1.4
python tests/test_bluetooth_classic_patch.py
```

更多离线和设备测试见 [tests](../tests/README.md)。原机固件的库依赖不包含在通用 CI 中；设备测试必须显式指定目标。

交付包所需路径见 [deploy/opi4](../deploy/opi4/README.md)。构建输出验证后再复制到相应 `bin`、`apk` 路径，并运行 `scripts/update-package-manifest.ps1` 与 `scripts/verify-package.ps1`。frpc 为可选维护工具，若组装现有完整交付包需自行提供相应版本并核对来源。源码仓库本身不承诺带有可直接安装的完整交付包。

提交前运行 `python scripts/check-public-source.py`；它同时检查 Git 可见文件和暂存区，输出文件名及原因，不打印密钥。仍需人工检查最终差异及第三方许可。许可范围见[第三方说明](../THIRD_PARTY_NOTICES.md)。
