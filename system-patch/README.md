# 系统层补丁

本目录包含当前固件的车机 GNSS、[蓝牙兼容补丁](bluetooth-classic/README.md)和早期 RK3399 / Android 8.1 BSP 补丁。

## 车机 GNSS

`vehicle-gnss/VehicleGnss.cpp` 实现 Android GNSS 1.0 passthrough HAL，把 Audio 应用发送的有效车机样本回调到标准 gps。只接受该包 UID 的本地 socket，验证字段、revision 和 5 秒新鲜度；断连/撤销/停止后不重发旧位置。没有伪造卫星数据，暂未提供 NMEA/SV 回调。

构建入口为 `scripts/build-vehicle-gnss.ps1`；依赖当前 ROM 的接口库及匹配头文件，准备入口为 `scripts/prepare-dependencies.py`，见[构建说明](../docs/building.md)。本地组包需包含模块及同固件接口库，一次性安装见[部署说明](../deploy/opi4/README.md)。HAL 初始化不等待车机，没有开机安装或 sleep；精确开机耗时未测量。GPS 长时延迟仍待实车复测，见[验收清单](../docs/audi-a4l-validation.md)。

## 历史 BSP 脚本

`apply.sh` / `verify.sh` 的 S1–S7 作用：

| 项目 | 内容 |
|---|---|
| S1/S2 | 替换 USB init 规则，移除 stock function 切换与 MS OS 描述符 |
| S3/S4 | 默认 USB 配置与 TCP ADB 属性 |
| S5 | 将 DTS 的 dr_mode 设为 peripheral |
| S6 | 启用 CONFIG_USB_CONFIGFS_NCM |
| S7 | 去除 composite 对 bcdUSB 的强制覆盖 |

S5 是早期固定 peripheral 方案，不能作为当前双角色运行的整板重建基线。当前内核配置、角色切换补丁和源码摘录位于 [kernel](kernel/README.md)，镜像仍在被忽略的本地构建目录。

只读预览与校验：

```sh
sh system-patch/apply.sh <SDK源码根目录> --dry-run
sh system-patch/verify.sh <SDK源码根目录>
```

`--kernel-only` 只处理 S5–S7；`--adb-secure` 是 apply 的显式选项。正式 apply 会改源码并创建 .orig 备份，整文件替换受原始哈希检查。

当前运行时入口是 `deploy/opi4/scripts/carplay_supervisor.sh`、`usb_lock.sh` 和 `usb_role_switch.sh`。系统补丁与运行时职责见[USB 分层说明](../docs/usb-lock-layers.md)；刷写与回滚见[恢复流程](../docs/flash-boot-and-adb-recovery.md)。
