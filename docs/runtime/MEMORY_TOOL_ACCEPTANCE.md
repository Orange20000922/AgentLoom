# 记忆与工具运行时验收

更新日期：2026-10-29。本文列出 #18、#24 的验收条件与测试映射。运行时契约见 [记忆上下文、必需工具与确定性预取](MEMORY_TOOL_RUNTIME.md)。验证包括 GTest、HTTP/SQLite 集成和独立安装包 consumer；模型提取质量、Linux 运行结果与端到端性能分别验证。

## #18：记忆来源、预算与异步契约

| 验收项 | 实现与验证 |
|---|---|
| L3 角色、来源、不确定性 | `L3Fact` 存储角色、证据、源记录 ID 和 uncertainty；编解码测试拒绝将 Response 证据归入用户陈述。旧记录在上下文编译时标为 unknown，不伪造角色。模型提取不等于独立事实验证。 |
| 分来源预算及可解释降级 | `ContextItem` 按 L0/L3/L4/observation/trusted/legacy/tool_result 计数；必需项先保留，可选项仅在显式允许时整条舍弃；保留丢弃与重复 ID。配置、预算单测及三路径强制上下文/结果超限集成覆盖。 |
| 一致的 deadline/取消/失败回填 | 同步/异步使用阶段总期限，Timeout 回填工具错误，Cancelled 结束 Turn；取消无回调的 executor 也能收口，晚到回调不重复完成。独立执行实例 ID 避免不同 Session 的模型 call_id 冲突；Gateway shutdown 集成覆盖悬挂 executor。 |
| L4 与注册表/版本/授权对齐 | 本轮工具计划从注册表取名称、schema、instruction，召回只提供候选；陈旧/未知候选排除并记录原因。声明资格与执行授权独立验证。规范声明、版本冲突、白名单与授权回归覆盖。 |
| PromptReady 与每次 LLM 调用观测 | observer 提供分来源字节/数量、重复 ID、工具/schema 字节、最终 max_tokens、finish_reason、usage；首轮和 follow-up 按 tool_round 区分。Turn 观测保留终态和已完成调用的累计用量；必调执行/预算失败不会抹掉此前调用的用量。Provider 没有返回的 usage 不估算。 |
| 单元与集成覆盖 | Persona/Skill、配置、L3 codec/storage、Gateway HTTP/流式、索引并发淘汰、SDK consumer 均纳入验证。 |

## #24：默认声明、强制上下文与必调

显式注册为只读查询的 RAG 或确定性工具在选入本轮工具集合后直接预取。其他必调工具由模型发起调用；未满足时返回失败终态，无额外提示重试。

| 验收项 | 实现与验证 |
|---|---|
| Always 在空召回/故障时存在 | 注册表 Always 项独立合并；无 provider、空召回和召回 Unavailable 集成覆盖，故障原因保留至 observer/结果。 |
| top_k 不淘汰强制工具 | top_k 只作用于召回 provider；`top_k=1` 双命中测试验证规范 Always 项仍加入计划；同工具再次召回不会复制声明。 |
| 白名单与授权不豁免 | 本轮规范声明构成 allowed_tool_names；既有未声明调用拒绝回归继续执行；三路径预取被 Authorize 拒绝时不执行、不生成、不提交。 |
| Required 未调用进入失败终态 | 同步、异步、流式均断言 FailedPrecondition、零 admission、零 Session commit；流式临时文本不会成为可见答案。 |
| Required 执行错误可见 | 必需结果使用版本化信封验收，保留错误码；测试断言无 follow-up、零 commit，失败 observer 仍能读取首轮用量。 |
| 预取绕过模型判断 | 仅 `read_only + execution=prefetch` 允许；参数由可信 binder 提供并经 schema 校验；三路径测试断言 executor 先执行、LLM 只生成一次且请求已包含结果。 |
| 上下文与结果预算 | 必需来源超限或 max_prompt_characters 超限明确失败，结果超 max_tool_result_bytes 保留 tool_result 来源；三路径集成覆盖，不静默截断。 |
| 三路径一致及观测 | Prompt/LLM/Turn observer 和成功 ChatResponse 保留 recalled/declared/calls/required/satisfied/degraded；日志输出工具集合、终态及原因，能区分无召回、未调用、必调未满足、拒绝和执行失败。 |

## CI 测试选择

Linux 与 Windows workflow 使用相同的构建目标和 `ctest -L ci` 过滤。来源编解码通过
`memory_fact_codec_tests` 执行，索引管理器通过 `vector_index_manager_tests` 执行，
两者均不依赖真实 embedding 模型。`context_plan_benchmark` 提供无性能阈值的 CI smoke。
Persona、Gateway 和配置回归继续使用既有测试目标。

CTest discovery 的实际清单包含 22 个新增测试定义、34 个展开实例，全部具有 `ci` 标签，
且不匹配 workflow 的 Redis 排除规则。34 个实例及 `ContextPlanBenchmark.Smoke` 共 35 项验证通过；
两个独立目标与 smoke 的完整选择共 12 项通过。

## 宿主与兼容性边界

- 参考 Gateway 装配现有事件循环上的 deadline scheduler，关闭 Runtime 时取消工具。直接使用 coordinator 的自定义宿主必须提供 scheduler 才能主动触发异步 deadline；不以阻塞等待或每请求线程模拟它。
- executor 的 Start/Cancel 必须快速返回；第三方资源的物理取消和 drain 仍由 executor 负责，框架保证的是调用终态与晚到结果抑制。
- 来源信封与角色校验不能证明模型提取文字的语义真实性，也不能单靠标签消除模型提示注入风险；未知来源保持未知。
- SDK DTO/接口新增字段，消费者需全量重编译。Windows 本地验证通过；Linux 由 PR CI 继续验证。
- L0 元数据重载、索引全局管理锁和逐条事实查重维持现有一致性语义；新鲜度、事务原子性和进一步性能方案统一在 #27 评估。

业务执行器、可信参数绑定和企业授权策略由消费者实现。上游回归验证公共运行时契约，跨平台构建由 Linux/Windows CI 验证。
