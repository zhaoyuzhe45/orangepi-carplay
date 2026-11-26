# 固件与 ADB 恢复

目标板为 Orange Pi 4 LTS / RK3399、Android 8.1。普通程序更新使用[交付安装器](../deploy/opi4/README.md)，无需重刷分区。

| 本地资料 | 用途 |
|---|---|
| `build/kernel-backup/` | 原厂 boot、kernel、resource 分区及 SHA256SUMS |
| `build/dtb-backup/` | 设备树参考与备份 |
| `build/role-latency/stage/kernel-role-latency.img` | 角色切换修正内核镜像 |
| `system-patch/kernel/` | 公开的配置、源码摘录及角色切换补丁；Image 单独构建 |
| `build/role-latency/device-before/kernel.img` | 角色修正前内核，配套兼容信息在同级 compatibility.json |
| `build/image-out/` | 不同阶段内核/boot 候选，用于比对，不是当前整机刷机包 |

`role-latency` 保留内核、配置、补丁及校验信息；固件安装使用下述正式分区工具。`system-patch/apply.sh` 的固定 peripheral 方案也不能覆盖当前双角色设置。

恢复时先核对板型、实际启动介质、分区路径/大小和镜像哈希。`deploy/opi4/scripts/flash_boot.sh` 提供 `--check`、`--apply`、`--rollback` 与 `--part boot|kernel`；resource 使用同目录 `flash_resource.sh`。先检查脚本参数和对应备份，再逐分区处理并回读验证。KRNL 容器头相同不代表 boot 与 kernel 可以互换。

设备 ADB 可达时先检查 `carplay_box`、Wi-Fi 和 frpc 状态。远程接入配置见 [config/frp](../config/frp/README.md)，认证密钥见 [config/adb](../config/adb/README.md)。内核无法启动或 Wi-Fi 驱动失配时，TCP ADB 不能承担恢复通道；使用已核实的启动介质备份或适配该板的离线恢复方式。

内核重建须匹配原固件源码、配置、工具链及外部 Wi-Fi/蓝牙模块 ABI，参考[内核资料](evidence/kernel-4.4.167/rebuild-prerequisites.md)。
