# Runtime And Operations Documents

本目录保存进程启动、配置、部署、模型装配、网络运行时和下游扩展的 canonical 文档。业务协议、持久化设计和实验结果不在本目录重复定义。

## 运行与扩展

- [配置系统](CONFIG_SYSTEM.md)
- [部署指南](DEPLOYMENT.md)
- [扩展 AgentLoom](EXTENDING_AGENTLOOM.md)
- [Net API Notes](NET_API_NOTES.md)
- [Callback 生命周期与优雅停服](CALLBACK_LIFETIME_SHUTDOWN.md)
- [Async gRPC Runtime](async_grpc_runtime.md)
- [GStreamer Windows Setup](../development/GSTREAMER_WINDOWS_SETUP.md)

## 模型与链路

- [Embedding Model](PHASE2_EMBEDDING_MODEL.md)
- [Vision System](../architecture/VISION_SYSTEM_DESIGN.md)
- [Vision Migration](VISION_MIGRATION.md)

## 运维原则

- 启动拓扑、硬件、模型 Provider 和 Secret 必须与配置文件职责分离；
- CUDA readiness 必须包含真实推理预热；
- 生产配置文件不得包含本机依赖路径，使用 ignored 实际文件与 `.example` 样例；
- Windows/Linux vcpkg install root 和 binary cache 必须隔离。
