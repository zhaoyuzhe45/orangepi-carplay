# 远程维护配置

`frpc.toml` 是本机私有配置备份，含实际 token，被 Git 忽略。公开模板为 `deploy/opi4/config/frpc.toml.template`；自行填写服务端地址、端口和 token。

恢复时将配置放到设备 `/data/carplay/frpc.toml`，设置 `root:root`、0600 并执行 restorecon，再让 frpc 重新加载。ADB 仍使用[公钥白名单](../adb/README.md)认证，远程地址由调用者显式提供。
