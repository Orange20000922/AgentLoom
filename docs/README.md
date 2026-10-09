# AgentLoom Documentation

本文档目录同时保存当前规范、实现说明、实验记录和早期架构资料。阅读时应先确认文档状态；历史文档中的旧项目名、路径和阶段判断用于保留开发上下文，不代表当前公开 API 或部署方式。

## 文档状态

| 状态 | 含义 |
|------|------|
| 当前规范 | 与当前源码边界一致，修改相关实现时应同步更新 |
| 当前设计 | 已确定方向，部分生产组装或验收仍在进行 |
| 讨论稿 | 用于方案评审，接口和实施范围尚未冻结，不代表已实现 |
| 实现参考 | 描述一个具体模块、实验或部署路径，使用前应结合源码核对 |
| 实验记录 | 有日期、环境和输入边界的可复现实验，不自动升级为生产承诺 |
| 历史记录 | 保存早期方案、迁移过程或商业化讨论，不作为当前实现承诺 |

## 分域导航

公开文档按所有权和变更边界归档。每个分域目录保存该领域的 canonical 文档与索引；跨领域文档只通过相对链接引用，不复制另一份实现承诺。

| 领域 | 入口 | 内容 |
|---|---|---|
| Architecture | [architecture/README.md](architecture/README.md) | 当前进程边界、协议、Session、Skill 和资源治理设计 |
| Gateway | [gateway/README.md](gateway/README.md) | HTTP/SSE/WebSocket 协议、前端对齐和 Session affinity |
| Runtime | [runtime/README.md](runtime/README.md) | 配置、部署、扩展、模型、网络和运行时依赖 |
| Data | [data/README.md](data/README.md) | Memory、缓存、Redis 和数据所有权边界 |
| Security | [security/README.md](security/README.md) | 认证、Secret、输入、日志和协议安全 |
| Performance | [performance/README.md](performance/README.md) | 正式 E2E 基线、A/B、容量边界和实验数据规则 |
| Development | [development/README.md](development/README.md) | 可移植工具链、Windows/WSL 隔离、CUDA 和验证顺序 |
| Archive | [archive/README.md](archive/README.md) | 旧路线、迁移过程和历史商业架构 |

## 新读者入口

建议按以下顺序了解项目：

1. [项目 README](../README.md)：定位、构建目标、模块概览和快速开始。
2. [架构总览](architecture/ARCHITECTURE_VISUAL.md)：当前模块、进程边界和主要数据流。
3. [配置系统](runtime/CONFIG_SYSTEM.md)：Server 配置和 CLI 覆盖方式。
4. [部署指南](runtime/DEPLOYMENT.md)：推理 Server 的构建、启动和运行时依赖。
5. [Frontend/Backend API](gateway/FRONTEND_BACKEND_API_PROTOCOL.md)：Gateway HTTP/WebSocket 协议。
6. [Skill Session Protocol](architecture/SKILL_SESSION_PROTOCOL.md)：有限工具会话的通用状态机和资源边界。
7. [安全工程准则](security/SECURITY_ENGINEERING_STANDARD.md)：认证、密钥、日志、输入和依赖安全要求。
8. [扩展 AgentLoom](runtime/EXTENDING_AGENTLOOM.md)：下游库依赖、Server 边界和领域 provider 注入。
9. [开发与性能验证环境](development/README.md)：可移植 Windows/v145/CUDA 基线和验证顺序。
10. [Agent Runtime E2E 性能报告](performance/AGENT_RUNTIME_E2E_PERFORMANCE_REPORT_2026_08.md)：当前容量基线、历史数据审计与优化优先级。
11. [Chat Completions / Gateway HTTP SSE](architecture/LLM_HTTP_SSE_PROTOCOL.md)：流式事件、提交边界、心跳与重连。

## 当前规范与设计

| 文档 | 状态 | 内容 |
|------|------|------|
| [LLM_PROTOCOL_AND_STREAMING_REFACTOR_DESIGN_2026_10.md](architecture/LLM_PROTOCOL_AND_STREAMING_REFACTOR_DESIGN_2026_10.md) | 当前实现 + 讨论稿 | 公共 LLM 接口与 Chat Completions 协议多态已实现；streaming 和上游状态仍待分阶段评审 |
| [CONFIG_SYSTEM.md](runtime/CONFIG_SYSTEM.md) | 当前规范 | JSON section、CLI override 和配置扩展方式 |
| [FRONTEND_BACKEND_API_PROTOCOL.md](gateway/FRONTEND_BACKEND_API_PROTOCOL.md) | 当前规范 | HTTP、WebSocket、认证、文档和 Skill API |
| [GATEWAY_FRONTEND_SESSION_ALIGNMENT.md](gateway/GATEWAY_FRONTEND_SESSION_ALIGNMENT.md) | 当前设计 | Gateway、前端和 session 生命周期对齐 |
| [GATEWAY_SESSION_AFFINITY_SCHEDULER.md](gateway/GATEWAY_SESSION_AFFINITY_SCHEDULER.md) | 当前设计 | Gateway 会话亲和、可选 ThreadPool 调度接口和公平准入 |
| [GATEWAY_SESSION_AFFINITY_BENCHMARK.md](performance/GATEWAY_SESSION_AFFINITY_BENCHMARK.md) | 实验记录 | 默认 FIFO 与 session affinity 的排队延迟和公平性 A/B 数据 |
| [AGENT_RUNTIME_E2E_PERFORMANCE_REPORT_2026_08.md](performance/AGENT_RUNTIME_E2E_PERFORMANCE_REPORT_2026_08.md) | 实验记录 | 历史 E2E 数据审计、全异步 CUDA 压测、1000 并发容量边界与优化优先级 |
| [RESOURCE_GOVERNANCE_REFACTOR_DESIGN.md](architecture/RESOURCE_GOVERNANCE_REFACTOR_DESIGN.md) | 当前设计 | 压力前馈、有界 worker 弹性、资源回落与不可变压测归档 |
| [SKILL_SESSION_PROTOCOL.md](architecture/SKILL_SESSION_PROTOCOL.md) | 当前规范 | 通用 Skill Session 生命周期与接口约束 |
| [SKILL_MEDIA_DRAIN_SPOOL_DESIGN.md](architecture/SKILL_MEDIA_DRAIN_SPOOL_DESIGN.md) | 当前设计 | 有限媒体输入的 Seal、Drain、mapped spool 和关闭语义 |
| [MULTIMODAL_PERCEPTION_LAYERS.md](architecture/MULTIMODAL_PERCEPTION_LAYERS.md) | 当前设计 | ViT/VLM 分层、视觉事件可信度和隐私边界 |
| [STREAMING_ARCHITECTURE.md](architecture/STREAMING_ARCHITECTURE.md) | 当前设计 | WebRTC、GStreamer、抽帧和视觉事件链路 |
| [CONVERSATION_CACHE_AND_INFERENCE_STRATEGY.md](data/CONVERSATION_CACHE_AND_INFERENCE_STRATEGY.md) | 当前设计 | 对话缓存、推理路由和降级策略 |
| [SECURITY_ENGINEERING_STANDARD.md](security/SECURITY_ENGINEERING_STANDARD.md) | 当前规范 | 服务端安全工程约束 |
| [EXTENDING_AGENTLOOM.md](runtime/EXTENDING_AGENTLOOM.md) | 当前规范 | 下游依赖、CMake alias 和业务扩展接口 |

## 模块与运行参考

| 文档 | 状态 | 内容 |
|------|------|------|
| [DEPLOYMENT.md](runtime/DEPLOYMENT.md) | 实现参考 | Windows/Linux 推理服务部署 |
| [GSTREAMER_WINDOWS_SETUP.md](development/GSTREAMER_WINDOWS_SETUP.md) | 实现参考 | Windows GStreamer 安装与插件检查 |
| [NET_API_NOTES.md](runtime/NET_API_NOTES.md) | 实现参考 | Net 层接口和设计说明 |
| [REDIS_COMPATIBILITY.md](data/REDIS_COMPATIBILITY.md) | 实现参考 | Redis/Redis++ 兼容性注意事项 |
| [PHASE2_EMBEDDING_MODEL.md](runtime/PHASE2_EMBEDDING_MODEL.md) | 实现参考 | ONNX text embedding 导出与测试 |
| [E2E_TEST_AND_EMOTION_PIPELINE.md](performance/E2E_TEST_AND_EMOTION_PIPELINE.md) | 实现参考 | VLM cache E2E 和情绪链路讨论 |
| [EMOTION_FUSION_GATE_EXPERIMENT.md](performance/EMOTION_FUSION_GATE_EXPERIMENT.md) | 实验记录 | MAP 标定、LLM gate 和 domain shift 结论 |
| [PERFORMANCE_REPORT.md](performance/PERFORMANCE_REPORT.md) | 实验记录 | 特定版本和硬件条件下的性能数据 |
| [VISION_SYSTEM_DESIGN.md](architecture/VISION_SYSTEM_DESIGN.md) | 实现参考 | 视觉系统总体设计 |
| [VISION_MIGRATION.md](runtime/VISION_MIGRATION.md) | 实现参考 | 视觉链路本地化迁移思路 |
| [VISION_PIPELINE_ANALYSIS.md](performance/VISION_PIPELINE_ANALYSIS.md) | 实验记录 | 早期视觉链路和项目边界分析 |
| [VISUAL_TOOL_SESSION_PROTOCOL.md](archive/VISUAL_TOOL_SESSION_PROTOCOL.md) | 历史记录 | 已由通用 Skill Session 协议取代的视觉专用版本 |

## 历史与迁移记录

以下文档保留设计演进价值，但不应直接用于判断当前 API、构建目标或完成度：

- [NEXT_RUNTIME_ROADMAP.md](archive/NEXT_RUNTIME_ROADMAP.md)：旧 Runtime 路线图。
- [INFRASTRUCTURE_PLAN.md](archive/INFRASTRUCTURE_PLAN.md)：基础设施早期开发计划。
- [ARCHITECTURE_DIAGRAM.md](archive/ARCHITECTURE_DIAGRAM.md)：早期教育智能体商业架构图。
- [COMMERCIAL_ARCHITECTURE.md](archive/COMMERCIAL_ARCHITECTURE.md) / [SUMMARY.md](archive/SUMMARY.md)：早期商业化架构讨论和总结。
- [FRONTEND_REFACTOR_E2E_PLAN.md](archive/FRONTEND_REFACTOR_E2E_PLAN.md)：前端迁移阶段计划。
- [TEAM_IMPLEMENTATION_PLAN.md](archive/TEAM_IMPLEMENTATION_PLAN.md)：早期团队实施建议。
- [CACHE_AND_OPTIMIZATION.md](archive/CACHE_AND_OPTIMIZATION.md)：缓存和视觉对话优化草案。
- [REDIS_PLUSPLUS_MIGRATION.md](archive/REDIS_PLUSPLUS_MIGRATION.md) / [REDIS_PLUSPLUS_MIGRATION_COMPLETE.md](archive/REDIS_PLUSPLUS_MIGRATION_COMPLETE.md)：Redis++ 迁移过程记录。

## 维护规则

- 当前规范应在相关接口、配置字段或生命周期语义变化时同步更新。
- 实验数据必须写明日期、输入、硬件、构建配置和适用边界。
- 历史文档不做无意义的全量改名；如结论已失效，在文首增加状态说明。
- 文档中的命令、路径和示例配置默认使用 UTF-8，并使用占位路径或 `.example` 文件，不提交本机配置。
- 新增文档时应同时更新本索引，并明确其状态。
- 新增文档必须放入对应分域目录；跨边界调整时同步修复仓库内链接和分域索引。
- 正式性能报告必须记录机器、OS、CPU/RAM/GPU、驱动、构建工具链、Provider、模型、配置和源码/工作区版本。
