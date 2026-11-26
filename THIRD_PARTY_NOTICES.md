# 第三方来源与发布范围

本项目自行编写的代码采用 **GPL-3.0-only**，全文见 [LICENSE](LICENSE)。第三方文件保留原作者声明与各自许可证；根许可证不替换它们的授权。修改后的第三方代码按相应许可分发。

| 组件 | 来源与许可 | 本仓库处理 |
|---|---|---|
| OrangeCar 接收端模拟器 | [sbuysse/orangecar](https://github.com/sbuysse/orangecar)，提交 `31a4625c6e6e8d7de2cc5a53d4ec28b437dfcbdd`，GPLv3 | `tools/bench/orangecar` 保留原 LICENSE；本地修改为增加 `--bind`。部分音频测试数据由该模拟器的结构生成 |
| DiPlay 接收端 | [shihabal3amri/DiPlay](https://github.com/shihabal3amri/DiPlay)，接收端 GPLv3；其 UI 另有 AGPLv3 声明 | 协议与会话行为参考。本仓库不分发其 APK、UI 源码或图标；测试时自行从上游获取。核对的上游提交为 `32f00ff21790a4a57315f02714932d5d59fdfe54` |
| CatPlay | [catplay-labs/catplay](https://github.com/catplay-labs/catplay) | 协议研究参考。截至 2026-11-26 未找到上游 LICENSE；本项目许可证不授予使用 CatPlay 代码的许可，不将其仓库或二进制纳入发行 |
| Linux USB gadget / DWC3 摘录 | Linux / Android 内核，原作者见文件头 | `tools/reference/kernel`、`system-patch/kernel` 的文件按各自 GPLv2 或 GPLv2-or-later 声明保留；[GPLv2 全文](LICENSES/GPL-2.0.txt)。与用户态守护进程分别构建 |
| Rockchip HWC 参考 | [khadas/android_hardware_rockchip_hwcomposer](https://github.com/khadas/android_hardware_rockchip_hwcomposer)，记录提交 `fd70146`，版本 0.54，Apache-2.0 | `tools/reference/hwc` 保留 AOSP/Rockchip 声明；[Apache-2.0 全文](LICENSES/Apache-2.0.txt) |
| EDID 生成器 | [akatrevorjay/edid-generator](https://github.com/akatrevorjay/edid-generator)，GPLv3 | 保留 `tools/reference/edid/edid-generator-master` 内的 LICENSE 和 README |
| Android init fixture | Android Open Source Project，Apache-2.0 | 保留 `tests/fixtures/opi4-display-init` 文件头 |

## 外部构建依赖

AOSP 8.1 头文件和固定提交的 VNDK 生成头文件由 `scripts/prepare-dependencies.py` 下载到被忽略的构建目录。AOSP 文件通常为 Apache-2.0；libc++ 保留上游双许可（MIT / University of Illinois），以各源文件和上游许可证为准。本仓库只分发获取方式与 SHA256 清单。

GNSS 系统接口库、蓝牙原库、系统镜像和平台签名文件由使用者从匹配固件或其 SDK 提供，构建脚本校验兼容性。哈希匹配仅用于防止误用固件，不代表取得再分发授权。公开源码不包含这些原件或修改后的二进制。

`tools/scrcpy`、frpc、DiPlay 测试 APK 和所有本机签名交付包均被 Git 忽略。需要时分别从 [scrcpy](https://github.com/Genymobile/scrcpy)、[frp](https://github.com/fatedier/frp)、DiPlay 获取，并遵循各自及其依赖的许可。发布二进制时另行提供相应源码、许可证和构建信息。

## 身份与品牌

公开默认图标和占位画面由 `tools/render-placeholder.py` 生成，不包含奥迪或 Apple 的图标。CarPlay、Apple、Audi 等名称仅用于说明兼容对象，不表示认证、授权或关联。

TLS 开发身份、MFi 测试材料、ADB 密钥、应用签名和远程维护配置不属于公开内容。生成自签 TLS 身份不会获得 MFi 认证。
