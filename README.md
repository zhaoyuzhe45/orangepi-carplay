# CarPlay 车盒（Orange Pi 4 LTS）

将 RK3399 / Android 8.1 盒子的桌面、音频和应用信息接入有线 CarPlay 车机，支持传统车机和 DiPlay。运行 AArch64 守护进程、MediaCodec 硬件编码器和 Android 特权应用。本项目为实验版本，主代码采用 [GPL-3.0-only](LICENSE)，第三方范围见[许可与来源说明](THIRD_PARTY_NOTICES.md)。

## 功能与边界

- 显示、旋钮、上一首/下一首、仪表歌名和媒体音乐已获用户实车确认。
- 物理显示预设为 800×480、1024×480、1280×600、1280×720、1920×900、1920×1080，均为 60 Hz。其他有效请求选择最近预设，再将完整画面缩放编码为接收端尺寸；奇数宽高向下取偶。
- 开机使用最近一次成功出图的预设，无有效缓存时为 800×480、density 160。缓存读取不增加轮询或等待。
- GPS 经标准 GNSS HAL 提供给 Android。长时间连接后位置延迟仍待实车复测，不能据短时更新判断解决；检查方法见[实车验收](docs/audi-a4l-validation.md)。
- 自动校时使用有效 RMC、ZDA 或 iAP2 时间，按 UTC epoch 比较，整秒差达到 2 秒时修正并读回，不修改时区；无有效时间不阻塞开机。
- **蓝牙备用联网默认关闭**，开关持久化，清除应用数据后恢复关闭。手动启用后，可上网的 Wi-Fi 优先；没有有效 Wi-Fi 时使用选定手机的蓝牙共享，后台重试。
- 车机麦克风为可配置功能。启用后外部应用录音可能切到双向音频；当前奥迪 legacy 通道此时为 16 kHz 单声道。双向通话、语音搜索和长时间导航仍需现场验收。

## 构建与部署

首次使用先按[从源码构建](docs/building.md)配置工具链、生成自己的开发身份与签名，并准备所需固件依赖。项目不包含私钥或预编译安装包。主要工具链为 NDK r22、JDK 8、Android build-tools 30.0.2；native 目标 API 21，Audio/Phone 为 minSdk 26、targetSdk 27。

```powershell
.\scripts\build.ps1 -Architecture All
.\scripts\build-audio-apk.ps1 -OutputApk build\release\CarPlayAudio.apk
.\scripts\build-phone-apk.ps1 -OutputApk build\release\CarPlayPhone.apk
```

native 和编码器默认输出到 `build/opi4`，`-Architecture AArch64 -PublishFinal` 同时更新本地交付主程序与编码器 JAR。组装完整交付包后生成 SHA256SUMS 并校验；固件适配及安装步骤见[交付说明](deploy/opi4/README.md)。

## 目录

| 目录 | 用途 |
|---|---|
| [src](src/README.md) | 协议、编码、音频、定位和电话 UI 源码 |
| [scripts](scripts/README.md)、[tests](tests/README.md) | 构建、检查及回归测试 |
| [deploy/opi4](deploy/opi4/README.md) | 当前交付包与设备安装脚本 |
| [config](config/README.md) | 本地签名、身份与维护配置说明；实际材料被 Git 忽略 |
| [docs](docs/README.md) | 运行维护、恢复、验收及协议参考 |
| [system-patch](system-patch/README.md) | GNSS、麦克风、蓝牙和 BSP 适配 |
| [tools](tools/README.md) | 诊断、台架及固件工具 |
| build（Git 忽略） | 构建输出、下载的依赖、固件与本地现场数据 |

## 致谢

感谢以下开源项目及其作者、贡献者：

- [DiPlay](https://github.com/shihabal3amri/DiPlay)：配件识别、USB 传输和接收端会话处理参考。
- [OrangeCar](https://github.com/sbuysse/orangecar)：有线 CarPlay 协议与连接时序参考；使用其[接收端模拟器](tools/bench/orangecar/README.md)进行台架测试，保留来源和许可证。
- [CatPlay](https://github.com/catplay-labs/catplay)：CarPlay/iAP2 处理和音频能力描述参考。
