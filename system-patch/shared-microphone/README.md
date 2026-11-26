# 板载麦克风共享

面向当前 RK3399 / Android 8.1 的 32 位音频进程，包装原厂 `r_submix` C ABI。仅拦截以 `mixr:<十进制数字>` 结尾的录音地址；普通输入和全部输出委托原库。Audio 后台通过一次板载采集向多个普通录音应用分发，本功能本身不申请音乐焦点，也不启用车机麦克风。

每个地址只有一个原厂输入和采集线程，各逻辑读者有独立游标。缓冲为 200 ms 或三个原厂周期中的较大值；慢读者跳过积压并报告丢帧。最后一个读者关闭时停止采集并释放原输入。相同地址并发重开最多等待 500 ms，超时失败；不创建第二个原厂输入，不写 PCM 到磁盘。

| 构建产物 | 用途 |
|---|---|
| audio.r_submix.fanout.probe.so | 独立探针加载原厂默认库，不替换系统模块 |
| audio.r_submix.fanout.production.so | 安装器使用，转调保存的 carplay-original 原库 |
| shared_microphone_hal_device_probe | 验证合成 PCM、多读者、standby、重开及输出进度 |

项目根目录运行 `scripts/build-shared-microphone-hal.ps1`，默认输出到 `build/shared-microphone`；先通过 `python scripts/prepare-dependencies.py --headers` 获取并校验固定 AOSP 8.1 头文件，之后可离线构建。状态探针由 `scripts/build-shared-microphone-status.ps1` 构建。

本机验证使用 `bash tests/run_shared_microphone_hal_test.sh` 和 `scripts/test-shared-microphone-relay.ps1`。安装、更新、核验与回滚通过[交付包专用安装器](../../deploy/opi4/README.md)，保留匹配原库与 APK 备份；只更新 APK 不会安装 HAL。

同一路由的采样率、声道与格式以原厂接受配置为准，不兼容重开返回配置供框架转换。原厂 capture position / MMAP 能力有限。独立应用的音频焦点、真实录音音质及车机通道切换需现场检查。
