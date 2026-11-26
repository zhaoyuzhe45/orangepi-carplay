# CarPlay USB 接口布局参考

保留已采集 iPhone 8 布局的维护结论，供 `src/iap2d.c` 与 `tests/usb_descriptor_layout_test.sh` 引用。

| 顺序 / 类型 | 用途 |
|---|---|
| PTP，06/01/01 | Apple 复合设备布局中的首接口 |
| ff/fe/02 | Apple Mobile Device；共享裸 iAP2 与 USBMUX bulk 通道 |
| ff/fd/01 | Apple Ethernet 标记；当前端点布局以构建函数为准 |
| CDC-NCM 控制 / 数据 | CarPlay IP 传输 |

接口顺序、端点预算、备用配置和内核 NCM 能力共同影响实际描述符。当前构建由 `build_speed_block()` 与 supervisor 协调；回归测试覆盖内核 NCM 和 TUN 两条布局，不能把历史抓取表直接写回当前配置。

FunctionFS 对类描述符的限制见[NCM 说明](ncm-class-descriptors-2026-10-25.md)，运行时接管见[USB 架构](../usb-lock-layers.md)。
