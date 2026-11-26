# 配置与身份材料

本目录在本机保存签名、身份和远程维护配置。Git 仅包含说明及 `.example` 模板，实际材料不会随源码分发。首次构建见[构建说明](../docs/building.md)。

| 路径 | 用途 |
|---|---|
| certs/lockdown-* | lockdown TLS 证书、公钥与私钥 |
| certs/diplay-mfi.* | 测试证书及私钥 |
| signing/carplay-audio.keystore | Audio/Phone 应用签名 |
| signing/carplay-audio.password | 本机自动构建口令 |
| [adb](adb/README.md) | 远程 ADB 客户端密钥 |
| [frp](frp/README.md) | 设备远程接入配置备份 |
| [platform-keys/rk3399](platform-keys/rk3399/README.md) | 指定系统镜像的平台签名材料 |

`src/lockdown_identity.h`、`src/lockdown_tls_identity.h` 将测试身份编译进 daemon；证书与密钥需配对。测试材料不代表生产授权或认证。

构建口令来源依次为命令行参数、`CARPLAY_AUDIO_KEYSTORE_PASSWORD`、本目录口令文件。`scripts/carplay-keystore.ps1 -Action status` 查看 Windows 凭据状态，`-Action copy` 复制口令。正常更新沿用原 keystore；换签名会影响原地升级及 Phone/Audio 的同签名权限。

私钥、keystore、口令和 frp token 应独立备份，不随公开项目或诊断报告分发。两份身份头文件也被 Git 忽略，由 `scripts/prepare-local-identity.py` 从本地 RSA 证书/私钥生成。提交前运行 `python scripts/check-public-source.py`，不要使用 `git add -f` 强行加入本地材料。
