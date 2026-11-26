# USB 运行架构

盒子先呈现 Apple gadget，收到有效角色请求后切换 Host 枚举车机，失败或结束后由 supervisor 恢复 device。维护使用 TCP ADB，USB CarPlay 配置不混入 ADB 接口。

| 层次 | 职责 |
|---|---|
| init / carplay_supervisor.sh | 开机接管、进程生命周期、配置与角色恢复 |
| usb_lock.sh | 检查属性与配置漂移，摘除 USB ADB |
| iap2d / box_role | 校验并确认角色请求，发布角色意图并进入 Host |
| usb_role_switch.sh | 操作已核实的角色节点，协调解绑与回退 |
| usbhost / NCM | 按 OTG 控制器拓扑发现设备、检查接口与端点，管理网络通道 |
| vendor 内核 | 提供可切换的 DWC3 控制器、NCM 与停机处理 |

待机与 CarPlay 存在双配置及能力降级路径，不能用固定总线号或单个 FunctionFS 链接判断全部状态。Host 工作期间手工重绑 device 会打断会话。

只读排查使用 `/data/carplay/bin/usb_lock.sh status`、`probe` 和[现场采集](validate-usb-on-host.md)。结合主日志、角色日志、UDC、内核和 boot ID 判断重连原因，单独的 charger/CDP/CC 字段不足以判断掉电。

`system-patch/apply.sh` 的 S5 固定 peripheral 是旧 BSP 方案，不能直接作为当前双角色固件基线。内核资料与恢复边界见[恢复说明](flash-boot-and-adb-recovery.md)。
