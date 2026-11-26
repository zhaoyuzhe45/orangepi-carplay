# iAP2 身份交换参考

对照 OrangeCar 接收/发送行为保留的协议说明，供 `src/iap2ident.h` 引用。当前生产路径在 `src/iap2session.c`，`iap2ident.c` 不参与生产构建。

| 方向 | 消息 | 内容 |
|---|---|---|
| 盒子 → 车机 | 0x1D00 StartIdentification | 请求识别 |
| 车机 → 盒子 | 0x1D01 IdentificationInformation | 车机身份与能力 |
| 盒子 → 车机 | 0x1D02 IdentificationAccepted | 空参数 |
| 盒子 → 车机 | 0x1D03 IdentificationRejected | 拒绝识别 |

接受帧必须成功入队后再推进认证，不能将车机发来的 0x1D03 当成接受通知。能力、PowerUpdate、传统/主动会话以当前 session 实现和回归测试为准。

参考项目的来源见[项目致谢](../../README.md)，模拟器保留来源与许可证见[台架目录](../../tools/bench/orangecar/README.md)。
