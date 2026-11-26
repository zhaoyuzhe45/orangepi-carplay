# NCM 类描述符与 FunctionFS

当前 vendor 4.4 的 FunctionFS 不接受直接注入所需的 CS_INTERFACE 类描述符。生产优先通过 configfs 内核 NCM 函数生成 CDC-NCM 控制、数据及功能描述符。

TUN 回退由用户态描述接口并处理 NTB，不等同于内核 NCM 的完整类描述符能力。两种布局和端点数量由 `tests/usb_descriptor_layout_test.sh` 检查；以实际端点与配置读回判断路径，不能仅凭 NCM 接口类代码确认驱动已绑定。

配置、角色与维护通道见[USB 架构](../usb-lock-layers.md)。
