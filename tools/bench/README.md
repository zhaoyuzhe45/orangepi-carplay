# 台架工具

- `run-orangecar-bench.ps1`：启动内置的 [OrangeCar 模拟器](orangecar/README.md)，通过 LAN 测试盒子发送端；`-SimPath` 可指定其他模拟器。
- `legacy_receiver.py`：传统模式接收端，默认使用内置模拟器的 `/info` 数据；`--reference` 可覆盖来源。
- `validate_legacy_capture.py`、`validate_legacy_services.py`：检查抓取的视频与服务交互。
- `run_legacy_services.sh`、`run_vehicle_probe.sh`：设备端手动台架脚本，会操作服务与测试应用。

在项目根目录启动传统模式接收端：

```powershell
python tools/bench/legacy_receiver.py --output build/legacy-bench
```

接收端默认监听端口 7011，运行 40 秒。无需另行下载 OrangeCar 工程。台架工具不参与盒子开机。
