# RK3399 平台签名材料

`platform.pk8` 与 `platform.x509.pem` 来自 friendlyelec/rk3399-android-8.1 的 SDK 平台密钥。匹配本机 framework-res.apk 的证书指纹：

```text
证书 SHA-256:
2D:37:0C:21:F5:DF:D5:53:D2:A7:96:31:4B:70:92:5F:B3:8A:DE:EF:90:86:4C:92:0B:BB:BB:12:88:7D:35:22
```

仅用于需要匹配该系统签名的适配包。换镜像后需重新核对 framework-res.apk 的证书；普通 Audio/Phone 应用使用 `config/signing/` 中的 keystore。

密钥来自公开 SDK，但使用它签名的系统共享 UID 应用具有更高权限。签名后用 `apksigner verify --print-certs <APK>` 核对结果。
