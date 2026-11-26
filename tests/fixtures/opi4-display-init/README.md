# OPi4 启动顺序 fixture

2026-11-13 从已核对身份的 rk3399_mid / Android 8.1 只读采集。保存 init、fstab 与 HWC/SurfaceFlinger 服务定义，用于验证 cache 挂载、持久属性重放和图形启动顺序。

测试仅修改临时副本，用于拒绝不支持的布局。本目录是测试输入，不是可清理的构建缓存。
