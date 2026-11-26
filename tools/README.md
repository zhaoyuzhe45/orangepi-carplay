# 辅助工具

| 路径 | 用途 |
|---|---|
| CarPlayCredentialStore.cs | Windows 构建凭据访问 |
| build-kernel-image.sh、rk_bootimg.py、rk_kernelimg.py | 内核构建与 Rockchip 镜像处理 |
| patch_dr_mode.py、patch_resource.py、patch_composite_unconfigured.py | 指定 BSP/DTB 补丁工具，使用前核对目标版本 |
| generate-screen-fixture.py、render-placeholder.py | 中性应用图标和视频占位帧生成；需要 ffmpeg/libx264 |
| scrcpy（本地工具，Git 忽略） | 从 scrcpy 官方发行页自行获取；不随源码分发 |
| [diagnostics](diagnostics/README.md) | USB、音视频及定位诊断 |
| [bench](bench/README.md) | CarPlay 台架接收端与采集校验 |
| [reference](reference/README.md) | 内核、HWC 和 EDID 参考 |

WM 尺寸调整不等同于生产物理预设切换。日常部署和服务恢复使用 `deploy/opi4/scripts`；固件工具不参与盒子开机。诊断输出写入被忽略的 `build`；来源和分发范围见[第三方说明](../THIRD_PARTY_NOTICES.md)。
