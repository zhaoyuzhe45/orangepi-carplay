# RK3399 内核重建参考

对应 Orange Pi 4 LTS 的 Android 8.1 / vendor 4.4.167 固件。维护者确认固件源码来自香橙派官网的 Orange Pi 4 LTS Android 8.1 源码包；原始压缩包的具体文件名、版本及 SHA256 尚未核对。`config-4.4.167-20220525.txt` 是原厂配置快照；当前角色切换修正所用配置在 `system-patch/kernel/config-used`。原厂配置未启用 NCM，不能不加核对直接作为当前构建配置。

重建需要匹配的 Orange Pi 4 LTS SDK 源码、配置和 AArch64 工具链，并验证外部 `sprdwl_ng`、`sprdbt_tty` 模块的 vermagic / 符号 CRC。仅内核版本号相同不足以保证模块兼容。原厂工具链记录为 Linaro GCC 6.3.1。

| 资料 | 路径（项目根目录下） |
|---|---|
| 原厂分区及哈希 | build/kernel-backup |
| 设备树备份 | build/dtb-backup |
| 当前角色修正的源码摘录、补丁、配置 | system-patch/kernel |
| 本机内核 Image | build/role-latency/kernel |
| 角色修正镜像 | build/role-latency/stage/kernel-role-latency.img |
| 修正前内核和配置 | build/role-latency/device-before |

构建时验证 NCM、USB 双角色、Wi-Fi/蓝牙模块和实际板型。分区大小与启动介质以目标设备查询结果为准。刷写和恢复入口见[恢复说明](../../flash-boot-and-adb-recovery.md)。
