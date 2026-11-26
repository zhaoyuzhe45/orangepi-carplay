# 生产源码

C11 守护进程（API 21）、Android 后台/控制台和独立 Phone UI（minSdk 26、targetSdk 27），运行目标为 RK3399 / Android 8.1。

| 模块 | 职责 |
|---|---|
| iap2d、iap2link、iap2session | 主循环、可靠链路、识别认证、会话与车辆服务 |
| usbhost、box_role、usb_vendor、usb_watchdog、usbmux、lockdown* | USB 角色、枚举、恢复及 Apple 传输 |
| ncm_bridge、carplay_mdns、airplay_legacy、airplay_client | 网络发现、传统/主动连接与媒体生命周期 |
| airplay_protocol、airplay_bplist、airplay_crypto、airplay_timing | 消息、加密和时钟 |
| carplay_capabilities、carplay_display、carplay_encoder、airplay_h264、airplay_video | 能力协商、物理预设、编码与视频传输 |
| airplay_input、airplay_uinput、carplay_knob、carplay_keys | 触控、旋钮和按键 |
| carplay_audio_bridge、carplay_modes | 音频 IPC 与资源借还 |
| carplay_now_playing、carplay_calls、carplay_route、carplay_file | 媒体信息、电话、导航与封面 |
| carplay_vehicle_state、carplay_time_sync | 车辆数据、GPS 和 UTC |
| carplay_config、carplay_control、carplay_log | 配置、回环 API 与日志 |
| [android_encoder](android_encoder/README.md) | MediaCodec helper |
| [android_audio_capture](android_audio_capture/README.md) | 音频、定位、应用集成与控制台 |
| android_phone | 独立电话 UI，通过同签名 Provider 使用 Audio 后台 |

生产编译清单以 [build.ps1](../scripts/build.ps1) 为准。`iap2ident.c` 是历史实验编码器，不参与当前会话；生产不启用 `CARPLAY_AUDIO_TESTING`、`AIRPLAY_TEST_VIDEO`。

在项目根执行 `scripts/build.ps1 -Architecture All`，输出到 `build/opi4/`；发布 AArch64 使用 `-Architecture AArch64 -PublishFinal`。Android 构建见[脚本索引](../scripts/README.md)。

`lockdown_identity.h`、`lockdown_tls_identity.h` 由本地证书/私钥生成并被 Git 忽略，替换需保持配对，管理规则见 [config](../config/README.md)。
