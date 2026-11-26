# RK3399 USB 角色切换内核资料

本目录保留当前角色切换修正对应的 `config-used`、`dwc3-role-latency.patch` 和修改后的 `gadget.c`、`dwc3-rockchip.c`。固件源码由维护者确认来自香橙派官网的 Orange Pi 4 LTS Android 8.1 源码包，内核版本为 Linux 4.4.167。GPL 授权及原作者声明见源码头部和 [GPLv2 全文](../../LICENSES/GPL-2.0.txt)。文件哈希见 `SHA256SUMS`。

这是两个驱动文件的补丁和完整摘录，不是完整 SDK。必须自行取得匹配板型和外部 Wi-Fi/蓝牙模块 ABI 的官方源码包；原始压缩包的具体文件名、版本及 SHA256 尚未核对，不能据这两个文件宣称整机固件已可独立复现。源码包无需具有 Git 提交号，可用原始压缩包的 SHA256 固定来源版本。

在已有匹配 SDK 的 Linux 环境中，先执行 `git -C <SDK>/kernel apply --check <本项目绝对路径>/system-patch/kernel/dwc3-role-latency.patch`。检查通过后才能应用；若源码已修正，可对照本目录两个 `.c` 文件。拒绝使用忽略失败、模糊匹配等方式强行应用。

保留 `config-used` 的 NCM、双角色和模块配置，并在编译前后核对 `.config`；编译使用 SDK 的 Linaro GCC 6.3.1，目标为 `ARCH=arm64`、`Image`。`tools/build-kernel-image.sh` 会重新生成 defconfig，只适合准备工作，不能代替当前配置的最终构建。

内核 Image、分区镜像、resource 和设备备份均不进入源码仓库。完整固件重建与外部模块 ABI 验证仍需匹配 SDK，刷写规则见[恢复说明](../../docs/flash-boot-and-adb-recovery.md)。
