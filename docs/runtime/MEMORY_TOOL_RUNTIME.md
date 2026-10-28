# 记忆上下文、必需工具与确定性预取

本文定义 #18/#24 的公共运行时契约。记忆性能、缓存新鲜度、索引锁粒度与压缩补偿的后续评估统一由 [#27](https://github.com/Orange20000922/AgentLoom/issues/27) 跟踪。

控制状态通过 `shared_ptr` 与在途回调共享生命周期。core 内存池提供字节块接口；小型控制对象使用标准共享分配，以保证控制对象可存活至最后一个回调结束。业务 payload 沿用所属模块的内存管理。

## 工具选择与必调语义

工具声明由注册表的规范 manifest 生成，L4 只选择候选；召回内容不能自行替换工具名称、输入 schema 或 instruction。带版本的候选必须匹配版本，未知或陈旧条目记录降级原因后排除。没有版本且注册表存在多个版本时明确失败，不按容器迭代顺序选择版本。

Manifest 的工具暴露与执行配置：

```json
{
  "exposure": "always",
  "execution": "prefetch",
  "read_only": true
}
```

- `exposure` 默认为 `recall`；`always` 独立于召回、`top_k` 和可选工具淘汰，每轮进入声明集合。
- `execution` 默认为 `model`。`prefetch` 仅允许显式 `read_only` 的查询工具；被召回或由 Always/必需策略选中后，在首轮 LLM 请求前执行。
- 预取复用 executor、输入/输出 schema、调用授权、结果来源验证和大小限制。宿主通过 `ISkillToolPolicy::BindPrefetchArguments` 绑定可信查询参数，缺少 binder 时失败；不自动从用户文本拼装租户、权限或业务标识。
- `ISkillToolPolicy::RequiredTools` 决定本轮必需的 `skill_id/version`。它是可信运行时策略，不从用户 JSON 或模型回复读取；必需项被加入本轮声明集合。
- `model` 必需工具仍由模型发起。模型未调用、授权拒绝、执行失败或结果校验失败时，Turn 返回错误，不提交正常问答，不进入正常答案缓存。不额外提示模型重试。
- 选中的预取项同样要求成功；预取完成后发送合法的 assistant/tool 消息关联，并在同一次生成请求中提供工具结果。

“始终声明”“始终注入数据”和“必须执行”分别由 manifest、可信上下文 provider、Turn policy 表达。它们不授予业务权限。只注册 Always 不能保证模型调用。

模型工具调用最多执行一轮，随后生成一次 follow-up。预取在首轮生成前完成。工具声明统一使用协议无关的 `llm::ChatCompletionRequest::Tool`。

## 上下文与预算

`ContextItem` 保留来源、记录 ID、内容、provenance 和是否必需。来源包括 L0、L3、L4、Skill observation、Trusted 和兼容旧 provider 的 Legacy。

工具执行结果另有 `tool_result` 总来源配额；协议消息必须与调用成对，因此超限明确失败，不能按可选降级丢掉某条 tool 消息。

`ITrustedContextProvider` 仅提供宿主已准备好的紧凑业务快照；外部查询由预取 executor 执行。其上下文为必需项。历史记忆与工具观察按引用数据编码，使用 JSON serializer 转义；旧来源不会因进入 system message 自动变成用户事实。模型对结构化引用数据的遵守仍需要真实模型评估，序列化标签不是安全隔离机制。

每来源默认 UTF-8 内容预算 64 KiB，总请求继续受 `max_prompt_characters` 的 Unicode 码点上限约束。产品可通过 `PersonaRuntimeOptions::context_budgets` 或配置指定：

```json
{
  "persona_gateway": {
    "context_budgets": {
      "l0": {"max_bytes": 65536, "allow_drop_optional": false},
      "l3": {"max_bytes": 16384, "allow_drop_optional": true},
      "l4": {"max_bytes": 32768, "allow_drop_optional": false},
      "observation": {"max_bytes": 8192, "allow_drop_optional": true},
      "trusted": {"max_bytes": 16384, "allow_drop_optional": false}
    }
  }
}
```

先保留必需项，然后按 provider 顺序选择可选项；只允许整项丢弃，不截断证据。必需项超限返回带来源的 `ResourceExhausted`。可选项默认也硬失败，只有显式允许才降级。预算报告包含各来源输入/保留字节、项数、丢弃 ID 和重复 ID。总请求预算还会覆盖消息、工具 schema 及工具结果，不能用分来源上限绕过。

## L3 来源兼容

新压缩输出为对象数组：`text/source_role/uncertainty/record_index/evidence`。来源为 `user_statement`、`model_suggestion` 或 `hypothesis`；角色和原文证据必须对应 Input/Response。模型建议不能用 Response 证据登记成用户陈述。

存储元数据使用 `agentloom.l3-fact.v1`，保留来源角色、源记录 ID、不确定性和 `model_extracted` 标记。该标记表示模型提取，事实真实性仍需业务层验证。旧字符串事实按 `unknown` 来源读取，存储布局保持兼容。

## deadline、取消与存储

异步工具阶段可注入 `ISkillDeadlineScheduler`，参考 Gateway 复用 HTTP 的 Asio 事件循环和现有线程池，不增加调度线程。deadline/显式取消收敛为一次完成；调用 executor 的 Cancel 时不持状态锁，晚到结果不触发 follow-up 或重复完成。自定义宿主应显式装配 scheduler，并在停止事件循环与 completion pool 前关闭 Runtime。

同步与异步使用总工具阶段期限。超时对未完成的调用回填 `Timeout` 工具错误；可选工具可交给 follow-up 解释，必需工具验收直接失败。显式取消返回 `Cancelled`，不继续生成。executor 的 `Start`/`Cancel` 仍须快速返回，不能在接口内阻塞外部 I/O；取消通知不等于第三方资源已物理释放，业务执行器仍负责 drain。

执行器以 `SkillExecutionRequest::execution_id` 跟踪可取消资源；`call.call_id` 只关联模型协议消息。不同 Session 可能复用模型调用 ID，不能将其当作全局资源 ID。

L4 使用异步查询入口；生产 Vector provider 装配 batching 生命周期。L3 的同步检索交给已有 I/O pool，避免直接占住 L0 完成回调。异步 L0 admission 队列拒绝时保持 best-effort 写入失败策略，不回退同步写入绕过背压。

索引查询持有共享租约，LRU 淘汰只移除缓存所有权。全局管理锁、数据库新鲜度检查、L0 元数据重载和逐条事实查重共同维持现有读写语义。Redis/SQLite 操作使用阻塞 API，其线程隔离、缓存和批处理方案在 #27 中评估。

## 观测与验证

`IPersonaRuntimeObserver` 提供 PromptReady、LlmCompleted 和 TurnCompleted 三类观测点，包含分来源预算、工具集合、schema 字节、最终 max_tokens、finish_reason 和 usage。成功响应保留逐次调用记录；工具或预算校验失败时，TurnCompleted 保留终态与已完成调用的累计用量。

回归测试覆盖规范声明、陈旧召回、Always、可信参数绑定、预算降级、来源角色、deadline/取消/晚到，以及同步/异步/流式预取和必调失败。SDK 公共结构与接口变化后，消费者应全量重编译。

2026-10-29 Windows VS2026/v145 Release 验证：

| 验证项 | 结果 |
|---|---|
| Persona/Skill/预算/取消 | 90 项通过 |
| Gateway 单元与 HTTP/流式集成 | 110 项通过，2 项 Redis 环境测试跳过 |
| Semantic Cache | 21 项通过，3 项 Redis 环境测试跳过 |
| L3 存储与来源编解码 | 8 项通过 |
| 配置 | 41 项通过 |
| 索引与 embedding coordinator | 14 项通过 |
| 独立安装包 consumer | 配置、编译、链接与运行通过 |

SDK 和参考 Gateway 服务入口构建通过。`context_plan_benchmark` 的 3000 次装配样本中，每次 48 条、6144 字节内容，P50/P95/P99 分别约为 115/152/182 微秒。该基准测量来源装配与 JSON 序列化开销；端到端延迟、Linux 运行结果及真实模型质量需分别验证。
