# 诊断工具

| 入口 | 用途 |
|---|---|
| state_dump.sh、quick_state.sh | 连接、进程、角色和日志快照 |
| capture-usbhost-enum.sh | 按控制器拓扑抓取 USB 枚举；`--snapshot` 为只读快照，`30 180` 为等待最多 180 秒并采集 30 秒 |
| usb_host_enum_capture.sh、usb_probe.sh、ddbox_idle_watch.sh | 外接设备描述符、OTG 物理状态及重连观察 |
| capture-field-streams.ps1 | 连续采集会话、GPS 内容时间/序列/年龄、Android 耗时和 USB 状态 |
| connect_probe.c、airplay_probe.c、bplist-dump.c、ntb-dump.c | 网络和协议诊断源码 |
| sps_probe.c、sps_report.py、frame_geometry.py | H.264 可见尺寸和画面几何分析 |
| ddbox_probe.c | 手工 USB 探测源码，需为目标平台编译 |
| layout_dump.sh、predeploy_check.sh、postdeploy_check.sh | 布局与部署前后状态检查 |
| android_audio_probe/ | Android 音频和车辆服务探针源码、构建器 |

这些工具不参与开机服务。采集结果不能代替当前状态读回；会发送协议请求或操作设备的探针在独立台架执行。H.264 的 coded 尺寸与裁剪后的可见尺寸应分别检查。

日常采集见[现场诊断](../../docs/validate-usb-on-host.md)，当前音频回归入口为 `scripts/test-audio-routing.ps1`。Java 探针按当前 SDK 重建。
