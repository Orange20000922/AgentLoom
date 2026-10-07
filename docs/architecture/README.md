# Architecture Documents

本目录保存跨进程、跨模块和资源生命周期的 canonical 架构文档。Gateway 公开协议、运行装配、数据持久化、安全和实验结果分别由其他分域负责。

## 当前架构与协议

- [架构总览](ARCHITECTURE_VISUAL.md)
- [Skill Session Protocol](SKILL_SESSION_PROTOCOL.md)
- [Skill Tool-Calling Runtime (2026-09)](SKILL_TOOL_CALLING_RUNTIME_2026_09.md)
- [Streaming Architecture](STREAMING_ARCHITECTURE.md)
- [Runtime Observability Contract](RUNTIME_OBSERVABILITY_CONTRACT.md)
- [Multimodal Perception Layers](MULTIMODAL_PERCEPTION_LAYERS.md)

## 当前设计

- [LLM 协议解耦与对话 Streaming（2026-10，讨论稿）](LLM_PROTOCOL_AND_STREAMING_REFACTOR_DESIGN_2026_10.md)
- [Resource Governance Refactor](RESOURCE_GOVERNANCE_REFACTOR_DESIGN.md)
- [Downstream Integration Execution Plan (2026-08)](DOWNSTREAM_INTEGRATION_EXECUTION_PLAN_2026_08.md)
- [Session Persistence Contracts (2026-08)](SESSION_PERSISTENCE_CONTRACTS_2026_08.md)
- [Skill Media Drain/Spool](SKILL_MEDIA_DRAIN_SPOOL_DESIGN.md)
- [Chat Prompt KV Cache](CHAT_PROMPT_KV_CACHE_REUSE_DESIGN.md)
- [Memory Runtime 加固设计（2026-09）](MEMORY_RUNTIME_HARDENING_DESIGN_2026_09.md)
- [记忆遗忘算法迁移前设计（2026-09）](MEMORY_FORGETTING_MIGRATION_DESIGN_2026_09.md)

## 使用规则

- “当前规范”必须与源码接口同步；
- “当前设计”允许部分生产装配尚未完成，但必须写清未完成项；
- Gateway、Data 和 Runtime 的具体边界由各自分域文档维护；
- 旧架构图和商业讨论从 [archive 导航](../archive/README.md) 访问，不作为当前实现承诺。
