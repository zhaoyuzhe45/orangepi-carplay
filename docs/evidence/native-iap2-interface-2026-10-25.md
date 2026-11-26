# CarPlay 配置与 Android USB 接口隔离

CarPlay 的 Apple 配置由 supervisor 与 iap2d 管理，不能混入 `ffs.adb` 等 Android 功能；维护走 TCP ADB。

板级 USB init 规则需要与运行时接管配合，避免 `sys.usb.config` 变化重新挂入 stock Android 组合。`system-patch/ramdisk/init.usb.configfs.rc` 是配套 BSP 输入；完整配置与角色切换以[USB 架构](../usb-lock-layers.md)及当前交付脚本为准。

PTP、Apple Mobile Device、Ethernet 标记与 NCM 的实际组合受内核能力和端点预算约束，不能通过手工删除所有非 iAP2 函数代替完整初始化。
