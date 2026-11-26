# 硬件屏幕编码器

构建：在项目根运行 `scripts/build-encoder.ps1`，默认生成 `build/encoder/carplay-encoder.jar`；`-OutputJar` 可指定输出，不自动部署。

由守护进程以 root 启动：

```text
CLASSPATH=/data/carplay/bin/carplay-encoder.jar app_process /system/bin com.carplaybox.encoder.ScreenEncoder WIDTH HEIGHT BITRATE FPS [SOURCE_WIDTH SOURCE_HEIGHT]
```

四参数形式使用相同源尺寸和输出尺寸；六参数形式将完整物理源画面投影到输出矩形，原点为 0、旋转为 0、layer stack 为 0。尺寸必须为偶数，输出范围受 RK3399 编码器约束。上层守护进程负责接收端尺寸校验、对齐填充与 SPS 裁剪，不能把当前路径理解为固定 1920×1080 采集。

编码器使用 `OMX.rk.video_encoder.avc` 和 Surface 输入。profile/level 参数交由 Android 8.1 ACodec 默认选择，以保持当前固件兼容。

父进程需提供 stdin/stdout 私有 socketpair：stdout 为 CPE1 二进制包，stderr 为日志；stdin 接受 `K` 请求关键帧，`Q` 或 EOF 停止。只有编码线程操作 MediaCodec，退出有期限，父进程保留 TERM/KILL 兜底。

本机回归：`tests/java/run-encoder-tests.ps1`。真实硬件尺寸、关键帧和画面完整性按[实车验收](../../docs/audi-a4l-validation.md)检查。
