# 记忆与工具运行时验收

更新日期：2026-10-29。本文列出 #18 的验收条件与测试映射。运行时契约见 [记忆上下文、必需工具与确定性预取](MEMORY_TOOL_RUNTIME.md)。验证包括 GTest、HTTP/SQLite 集成和独立安装包 consumer；模型提取质量、Linux 运行结果与端到端性能分别验证。

## #18：记忆来源、预算与异步契约

| 验收项 | 实现与验证 |
|---|---|
| L3 角色、来源、不确定性 | `L3Fact` 存储角色、证据、源记录 ID 和 uncertainty；编解码测试拒绝将 Response 证据归入用户陈述。旧记录在上下文编译时标为 unknown，不伪造角色。模型提取不等于独立事实验证。 |
| 分来源预算及可解释降级 | `ContextItem` 按 L0/L3/L4/observation/trusted/legacy/tool_result 计数；必需项先保留，可选项仅在显式允许时整条舍弃；保留丢弃与重复 ID。配置、预算单测及来源预算与配置单测覆盖。 |
| 一致的 deadline/取消/失败回填 | 同步/异步使用阶段总期限，Timeout 回填工具错误，Cancelled 结束 Turn；取消无回调的 executor 也能收口，晚到回调不重复完成。独立执行实例 ID 避免不同 Session 的模型 call_id 冲突；Gateway shutdown 集成覆盖悬挂 executor。 |
| L4 与注册表/版本/授权对齐 | 本轮工具计划从注册表取名称、schema、instruction，召回只提供候选；陈旧/未知候选排除并记录原因。声明资格与执行授权独立验证。规范声明、版本冲突、白名单与授权回归覆盖。 |
| PromptReady 与每次 LLM 调用观测 | observer 提供分来源字节/数量、重复 ID、工具/schema 字节、最终 max_tokens、finish_reason、usage；首轮和 follow-up 按 tool_round 区分。Turn 观测保留终态和已完成调用的累计用量；必调执行/预算失败不会抹掉此前调用的用量。Provider 没有返回的 usage 不估算。 |
| 单元与集成覆盖 | Persona/Skill、配置、L3 codec/storage、Gateway HTTP/流式、索引并发淘汰、SDK consumer 均纳入验证。 |

## 宿主与兼容性边界

- 参考 Gateway 装配现有事件循环上的 deadline scheduler，关闭 Runtime 时取消工具。直接使用 coordinator 的自定义宿主必须提供 scheduler 才能主动触发异步 deadline；不以阻塞等待或每请求线程模拟它。
- executor 的 Start/Cancel 必须快速返回；第三方资源的物理取消和 drain 仍由 executor 负责，框架保证的是调用终态与晚到结果抑制。
- 来源信封与角色校验不能证明模型提取文字的语义真实性，也不能单靠标签消除模型提示注入风险；未知来源保持未知。
- SDK DTO/接口新增字段，消费者需全量重编译。Windows 本地验证通过；Linux 由 PR CI 继续验证。
- L0 元数据重载、索引全局管理锁和逐条事实查重维持现有一致性语义；新鲜度、事务原子性和进一步性能方案统一在 #27 评估。

业务执行器、可信参数绑定和企业授权策略由消费者实现。上游回归验证公共运行时契约，跨平台构建由 Linux/Windows CI 验证。
