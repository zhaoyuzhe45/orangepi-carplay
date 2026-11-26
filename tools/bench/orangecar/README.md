# OrangeCar 接收端模拟器

`carplay_receiver_sim.py` 来自 [sbuysse/orangecar](https://github.com/sbuysse/orangecar) 的 `tools/bench/carplay_receiver_sim.py`，对应提交 `31a4625c6e6e8d7de2cc5a53d4ec28b437dfcbdd`。原项目许可证随附于 [LICENSE](LICENSE)。

保留既有本地修改：增加 `--bind` 参数，默认监听 `0.0.0.0`，支持盒子从 LAN 连接。仅依赖 Python 标准库。

由上级目录的 `run-orangecar-bench.ps1` 启动，或供 `legacy_receiver.py` 读取 `/info` 数据。它是台架工具，不随盒子交付包部署。
