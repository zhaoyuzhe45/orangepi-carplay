# 维护文档

当前功能和构建入口见[项目 README](../README.md)，设备安装见[交付说明](../deploy/opi4/README.md)。

| 文档 | 用途 |
|---|---|
| [从源码构建](building.md) | 工具链、身份、固件依赖及公开源码检查 |
| [USB 运行架构](usb-lock-layers.md) | gadget、Host 交接和维护通道职责 |
| [连接与现场诊断](validate-usb-on-host.md) | 连接慢、重连、音频和定位采集 |
| [固件与 ADB 恢复](flash-boot-and-adb-recovery.md) | 镜像边界、分区校验和恢复入口 |
| [实车验收](audi-a4l-validation.md) | 显示、输入、音频、GPS 长时验收 |
| [测试入口](../tests/README.md) | 本机和设备测试 |

`evidence/` 仅保留源码或测试直接引用的协议说明，以及内核配置参考。文件名中的日期表示来源版本；当前运行行为以源码和交付包为准。未完成的 GPS 长时延迟现场资料在 `build/gps-lag-20261121`。
