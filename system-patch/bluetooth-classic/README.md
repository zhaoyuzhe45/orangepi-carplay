# Android 8.1 Classic 蓝牙兼容补丁

仅适用已核对的 rk3399_mid ARM64 固件。补丁对 BR/EDR 已连接场景跳过额外 LE 自动服务发现，保留原有配对、密钥保存、加密及其它 LE 路径。

| /system/lib64/hw/bluetooth.default.so | SHA-256 |
|---|---|
| 原库 | D33810D95EF68EB05CEF71144B25BCF4C84117F5B83755D5324C97EB927F1077 |
| 补丁库 | 7985C96FC09E864ABC5F50D7E7B822A57FBE6294B12B23C38A759984872EE9B8 |

在项目根运行 `scripts/build-bluetooth-classic.ps1`；先按[构建说明](../../docs/building.md)导入原厂库到 `build/deps/bluetooth/`，产物输出到 `build/bluetooth-classic/`。`tests/test_bluetooth_classic_patch.py` 使用 Unicorn 2.1.4 验证两条分支、寄存器和修改范围。

设备端以 root 运行 `install_bluetooth_classic.sh <已推送的补丁库路径>`，随后重启加载。回滚使用 `rollback_bluetooth_classic.sh` 和 `/data/carplay/backups/bluetooth-classic/` 中匹配哈希的原库，也需重启。

已观察到 HFP/PBAP 保持连接及通讯录同步；实际回滚和真实双向通话未验收。同时依赖 BR/EDR 与 LE 自动发现的设备可能受影响。验收项目见[实车清单](../../docs/audi-a4l-validation.md)。
