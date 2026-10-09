# LLM 协议解耦与对话 Streaming 设计（2026-10）

> 阶段 1–4 已实现：公共接口、Chat Completions 协议、协程增量 HTTP 与 Gateway HTTP SSE。
> 当前接口、心跳/重连及验收边界见 [HTTP SSE 协议](LLM_HTTP_SSE_PROTOCOL.md)；阶段 5–6 仍是设计。

## 当前协议实现边界

`src/llm/llm_client.h` 统一声明公共 DTO、`ILlmClient` / `IAsyncLlmClient`、取消句柄、
通用响应校验选项、Fallback 和 PromptStore。保留既有 `Chat*` 类型名及
`ValidateChatCompletionRequest`；`openai_llm_client.h` 继续包含公共入口，旧消费者无需更名。
Persona、Memory、Document、Skill、CloudTask 和 LocalLlm 已直接依赖公共入口。

`src/llm/llm_protocol.h/.cpp` 提供 `ILlmProtocol` 与无请求级状态的 `ChatCompletionsProtocol`：

| 方法 | 当前职责 |
| --- | --- |
| `Endpoint()` | 提供相对 base URL 的 endpoint |
| `Capabilities()` | 声明线协议可表达的工具、图片、reasoning 字段能力；不证明 Provider/模型支持 |
| `ValidateRequest()` | 工具声明、批次关联及流式单 choice 约束 |
| `EncodeRequest()` | 返回拥有独立存储的完整请求 body，保留原 JSON 序列化参数与字段布局 |
| `DecodeResponse()` | 解析成功响应 body，保留 UTF-8、结束状态、工具调用、usage 与 token audit 校验 |
| `CreateStreamDecoder()` | 每次生成独立的 SSE/Chat Completions 聚合器及事件 sink |

协议接口不引用 HTTP DTO 或 I/O runtime。HTTP 客户端保留认证、超时、HTTP status 错误映射、
同步/异步传输、重试和取消。非 200 响应在客户端映射成原有 `core::Status`，不交给协议解码。
默认客户端仍选择 `ChatCompletionsProtocol`，注入示例：

```cpp
auto protocol = std::make_shared<const agent::llm::ChatCompletionsProtocol>();
agent::llm::OpenAiLlmClientOptions options;
options.base_url = provider_base_url;
options.api_key = provider_api_key;
options.protocol = protocol;  // 同步和异步 Create 均使用该字段。
```

共享对象必须保持逻辑不可变；所有方法不得保存传入视图。新协议的 decoder 必须为每个请求独立创建。
`reasoning_content` 字段仍原样透传，不转换成可见回答；其他协议的专属 continuation 尚未引入。
当前已实现 HTTP SSE 与已有两轮工具闭环；没有 Responses、DSML、托管会话或通用多轮工具预算实现。

验证入口：`llm_protocol_test.cpp` 固定请求字节与结果字段、校验拒绝、HTTP 状态分工及协议注入；
`llm_integration_e2e_test.cpp` 覆盖真实同步/异步 HTTP 的协议替换；安装包 consumer 覆盖新旧头文件共存及协议调用。
公共源码接口保持旧入口，`OpenAiLlmClientOptions` 增加协议字段，SDK 与消费者需一起重编译。


## 1. 现状与可复用模块

当前主链路为：

```text
Session admission / deferred lane
  → Emotion / Memory Lookup
  → Persona 构造 ChatCompletionRequest
  → 同步或异步 OpenAiLlmClient
  → 完整 ChatCompletionResponse
  → 可选工具执行与 follow-up
  → AI Emotion / Memory Admission / Session commit
  → 单次对话完成回调
```

已经可复用的边界：

- `IHttpClient` / `IAsyncHttpClient`、TLS、连接复用与取消句柄；
- `ILlmClient` / `IAsyncLlmClient` 及现有同步调用消费者；
- Session affinity、deferred lane、continuation 和现有线程池；
- `IMemoryContextProvider`、L0/L3 repository/index/maintenance；
- Skill Registry、ExecutorFactory、InvocationService、Coordinator 和 Skill Session；
- transport-neutral `IPersonaInteraction`、Gateway HTTP/WebSocket 路由和网络背压；
- `IPersonaInteraction::CancelTurn` 按 Session 接纳异步 Turn 取消，已有 LLM 句柄可主动取消；无句柄的 Provider 阶段等待回调收口；
- `core::Status` / `Result`、logger 及已有用量契约。

重构前的缺口（前两项已在当前实现中解决）：

| 边界 | 当前实现 | 影响 |
| --- | --- | --- |
| 公共 LLM DTO | 定义在 `openai_llm_client.h`，业务依赖具体协议头文件 | 协议变更扩散到 Persona、Skill、Memory、Document 等模块 |
| 协议处理 | endpoint、序列化、响应解析和校验绑定 OpenAI 客户端 | 同步与异步传输无法独立选择协议实现 |
| 出站 HTTP | callback 只返回完整响应体 | 无法降低首片段可见延迟 |
| 对话输出 | Persona 与公共交互接口只有最终 callback | 仅替换上游客户端不能完成端到端 streaming |
| Gateway stream 参数 | HTTP route 读取 `stream`，Service 未把它转为 Runtime 流式请求 | 参数存在不代表流式能力已实现 |
| 工具循环 | 至多执行一轮工具，follow-up 仅保留工具调用信息 | 完整输出 item 与 reasoning continuation 没有保存边界 |
| Session 历史 | 主要保存最终用户输入、回复与情绪元数据 | 无法完整重放工具中间过程或恢复 Provider continuation |

现有 [Streaming Architecture](STREAMING_ARCHITECTURE.md) 描述媒体/视觉输入，不能视为 LLM 对话 streaming 的实现或验收依据。

## 2. 设计目标

1. 业务模块依赖协议无关接口；Chat Completions、Responses 由不同协议实现承载。
2. 第一条流式主路径仍使用 Chat Completions 与本地上下文组装。
3. streaming 改善首片段延迟，同时保持同 Session 保序、取消收口和一次最终提交。
4. Memory 与 Skill 的业务职责维持独立；协议适配不决定事实存储或上下文托管策略。
5. 分阶段替换消费者，每阶段保留独立回归与回滚能力。

## 3. 候选四层边界

```text
业务用例：Persona / Skill / Memory / Document
                 ↓
协议无关 LLM 请求、结果、事件与 operation
                 ↓
ILlmProtocol：编码、解码、能力约束
     ChatCompletionsProtocol | ResponsesProtocol
                 ↓
HTTP 异步传输：完整响应或增量 body
```

### 3.1 公共类型与客户端接口

先将公共 DTO、`ILlmClient`、`IAsyncLlmClient`、取消句柄及通用校验选项移出具体客户端头文件。
建议公共入口为 `llm_client.h`，具体 HTTP 客户端和协议实现使用自己的头文件。
迁移初期允许保留旧类型名或兼容别名，避免把批量重命名与行为变更混在一起。

公共请求应能表达：

- 本轮输入消息或 item、生成预算、工具声明；
- 请求/Session/trace 标识；
- 输出模式及业务要求的能力；
- 可选且受 scope 约束的 Provider continuation。

公共结果应区分最终文本、工具调用/结果、有序输出项、引用、结束状态和用量。
reasoning continuation 等不可解释的 Provider 数据保留为受控的专属载荷，不转换成普通对话文本。
通用类型只定义业务需要的最小集合，并保留扩展边界，不镜像全部上游 JSON 字段。

使用有类型的 DTO / `std::variant` / `std::optional`；Provider 原始载荷应明确所有权与大小限制。
跨异步回调的数据必须自有或共享持有，不保存指向网络读取缓冲区的借用视图。

### 3.2 协议接口与多态

`ILlmProtocol` 候选职责：

- 给出 endpoint 与协议能力；
- 将公共请求编码为 Provider 请求；
- 校验请求是否可由当前协议和已配置模型承载；
- 将完整响应转换为公共结果；
- 为每个请求创建独立的流式 decoder。

HTTP 状态、传输错误、退避与连接生命周期由客户端/传输层处理；业务事实与 Session 提交由 Runtime 处理。
共享协议对象保持不可变，每个在途请求持有自己的 decoder，不能让多个 Session 共用可变解析状态。

能力按协议、Provider 和模型共同决定。配置声明能力并以集成测试验证；
不能从“OpenAI-compatible”或 `/responses` 可访问推导出持久状态、内置工具、strict schema 或 streaming 全部可用。
不支持的能力返回 `core::Status`，不静默退化为另一种行为。

### 3.3 HTTP 增量传输

优先为现有异步 HTTP 实现增加独立流式 capability interface，保留完整响应接口供现有消费者使用。
客户端运行时继续复用既有 I/O、TLS、连接管理和线程池。

增量路径需要区分响应头、body 片段和传输终态：

- 先检查 HTTP status 与 content type，再进入协议 decoder；
- body 分块与 SSE 事件、JSON、UTF-8 字符边界无关；
- SSE framing 可作为内部复用组件，具体事件 payload 由协议实现解释；
- 明确单个事件、工具参数、整次生成和待发送队列的容量上限；
- EOF 不自动等于模型成功，缺少协议终态需返回错误；
- Cancel、deadline、连接关闭和解析失败竞争时，最终 completion 恰好一次。

重试策略必须结合是否已发布增量或执行副作用：在结果不确定时，不能沿用普通请求的自动重试。
首片段之后默认不透明重试；Provider 明确支持的恢复行为另立能力契约。

### 3.4 Runtime 与 Gateway 事件

建议在最终 completion 之外增加事件 sink，先覆盖以下业务事件：

| 事件 | 语义 |
| --- | --- |
| TextDelta | 某次生成、某个输出项的文本增量 |
| ToolCallProgress | 工具调用声明/参数组装进度；是否对外展示由 Gateway 决定 |
| ToolExecutionState | 本地工具开始、完成或失败 |
| UsageUpdated | Provider 报告的用量更新，不代表 Turn 已完成 |
| OutputItemCompleted | 单个输出项结束 |
| GenerationCompleted | 单次 Provider 生成结束，Runtime 内部事件 |

外部 `TurnCompleted` 只能在工具 follow-up、必要后处理和 Session commit 完成后发布。
Provider 的 `[DONE]`、单个输出结束、单次工具调用结束不能直接映射为 Turn 成功。
失败/取消也必须具有唯一 Turn 终态，使用统一 `core::Status`。

事件携带 request/turn/generation/item 标识及单调 sequence，区分多轮工具生成。
工具调用轮次中伴随的文本可能只是计划或过渡内容，需要决定直接展示、暂存或标记为 provisional；
不能把它与工具返回后形成的最终答案混为一个不可区分的文本流。

Gateway sink 通过现有有界队列转发，不能在模型 I/O callback 中阻塞等待慢客户端。
初期建议队列满时返回 `ResourceExhausted` 并取消在途生成；暂停/恢复读取可在传输接口支持后独立引入。
文本增量不能像视觉帧那样丢弃旧消息。

## 4. 提交、取消与工具调用不变量

### 4.1 流式可见与提交

增量是暂态输出，不能逐片段写入 L0、更新 recent history 或增加 turn count。
同 Session 的 deferred lane 从 admission 保持到 Turn 终态；首片段发出后不能提前放行下一 Turn。

生成结束后继续完成 AI Emotion 和必要后处理，再按冻结后的 Memory Admission 策略执行一次 commit。
现有异步主路径在 memory admission 失败后记录日志并继续提交，同步路径返回失败，
此差异需要在实施前明确统一目标；本文不自动改变其语义。

需记录 generation end 到 Turn commit 的延迟，否则 TTFT 的改善可能掩盖完成阶段等待。
如果用户已经看见部分文本但后处理失败，外部返回失败终态，并明确回复未完成提交。

### 4.2 客户端断开

建议交互对话默认取消生成，保留与前端连接解耦的后台任务作为独立用例。
取消传递给 HTTP operation、Runtime 和适用的工具执行器，最终释放 Session lane 与配额。
取消不撤销已发生的外部工具副作用；工具结果及执行幂等性另有持久化职责。

### 4.3 工具参数与循环

参数 delta 只用于组装，必须在完整调用结束、JSON/schema 校验通过后才执行工具。
同批调用 ID 与结果关联保持原样，失败回填结构化错误。
工具结果仍由现有 InvocationService / Coordinator 返回，协议实现负责线格式转换。

后续多轮工具循环应有最大轮数、总 deadline、累计 token/调用预算和幂等边界。
取消和预算耗尽必须有明确终态，不依赖模型自行停止。
多轮循环可独立于首期纯文本 streaming 验收，但事件模型应提前保留 generation 区分。

### 4.4 用量

保持 [Runtime Observability Contract](RUNTIME_OBSERVABILITY_CONTRACT.md) 的累计口径：
工具首轮和 follow-up 都计入成功 Turn 用量；答案缓存直返为零；未报告不估算。
增量 usage 可能是累计快照或补充字段，聚合器不能逐事件盲目求和。
只在确认 Provider reporting 语义后更新计量；reasoning/cached token 明细按能力保留。

## 5. Responses 与上下文策略的后续边界

### 5.1 协议选型裁决（2026-10，基于国产模型 Responses 支持调研）

阶段 1–2 的协议多态已落地，`ILlmProtocol` 是可注入接缝，协议选择由配置静态解析装配（按模型名正则匹配）
决定，不在请求级动态切换。在此基础上，三种协议的定位已从"后续边界"收敛为明确裁决：

**Chat Completions —— 默认/兜底协议。** 覆盖 OpenAI-compatible 全生态与本地 `LocalLlmChatClient`（语义对齐但不
走 HTTP）。Tool-calling 两轮闭环已验证。没有特殊理由的 Provider 都落到这里。

**无状态 Responses —— 条件触发的并行协议实现，不是顺序排队的备选。** 触发条件是 Provider/模型级的：
某条 Provider 接的是需要 reasoning continuation 和 item 化输出的模型，Chat Completions 承载不了，就给该
client 实例注入 `ResponsesProtocol`。业务模块不感知，因为它们只依赖协议无关 DTO。

国产模型 Responses API 支持矩阵（经查证，作为无状态 Responses 值得做的旁证）：

| Provider | Chat Completions | 无状态 Responses | 有状态 Responses |
| --- | --- | --- | --- |
| DeepSeek | ✅ | ✅ | — |
| Kimi（月之暗面） | ✅ | ✅（`previous_response_id` 固定 null、`store` 固定 false） | ❌ 明确只做无状态子集 |
| 豆包（火山方舟） | ✅ | ✅（板块最全：迁移/深度思考/多模态/工具调用/结构化输出/上下文编辑） | 待实测 |
| 通义千问（百炼） | ✅ | ✅（`previous_response_id` 官方建议手动组装历史） | ❌ 建议手动组装 |
| 智谱 GLM | ✅（`/paas/v4/chat/completions`，有 `tool_stream`） | ❌ 暂无 Responses 端点 | ❌ |

有状态 Responses（`previous_response_id` / `store`）被 Kimi/通义集体只做无状态子集，基本只有 OpenAI 自家在推。
这给有状态 Responses 的否决提供了市场旁证。

各厂商 Responses 实现存在方言落差，验证设计约定 §3.2"不能从 OpenAI-compatible 或 `/responses` 可访问推导出
持久状态、strict schema、streaming 全部可用"：Kimi 的 `tool_choice` 只支持 `auto`、`custom` 工具只认
`apply_patch`、图片只收 data URL、`temperature`/`top_p` 请求体不暴露、显式缓存断点直接 400；通义 Chat Completions
路径下 `tools` 与 `stream=True` 有兼容限制。这些方言差异应由配置声明 + 集成测试验证，不写死在协议实现里。
`LlmProtocolCapabilities` 目前是协议自己声明线格式能力，将来可能要拆成"协议能力 ∧ Provider 覆盖"两层。

**有状态 Responses —— 架构否决，不是条件触发。** 它的冲突不在协议层，而在上下文事实源：把对话状态外移到
Provider，绕过 `fencing_token` / `runtime_revision` / idle cleanup 整套可回收投影模型，制造第二个不受
Runtime 控制的上下文事实源。协议解耦解决不了这个——可以很容易加一个 `StatefulResponsesProtocol`，但它引入
的第二事实源问题依然存在。在"上下文完全控制 + 可回收 Runtime"的需求下（见两级 Session 架构
`SESSION_ARCHITECTURE.md` §2.1"两级 Session 不是两个平级事实源"），它被从路由策略里排除。加上生态层面只有
OpenAI 自家在推，绑死单一 Provider，不做。

### 5.2 无状态 Responses 的设计后果

无状态 Responses 落地后会逼出一个既有欠账：**输出 item / reasoning continuation 的持久化边界**。每次请求要
把上轮的 output item 回传进 `input`，这意味着 Session 历史要能存这些不透明载荷并受大小约束。这不是协议层的活，
是 Session 持久化契约的活（见 `SESSION_PERSISTENCE_CONTRACTS_2026_08.md`），建议在阶段 5 动工前先评审。

Reasoning item 是多轮工具循环的正解。Chat Completions 下 DeepSeek 的 `reasoning_content` 只能透传、无法回传
续接；Responses 的 item 模型里 reasoning 是结构化输出项，多轮工具循环时可以随 `input` 回传。这对接 §4.3 的
多轮循环预算是绕不开的载体。

事件模型上，Responses 的 SSE 事件（`response.output_text.delta`、`response.output_item.done`、
`response.completed`）与 §3.4 规划的事件 sink（`TextDelta` / `OutputItemCompleted` / `GenerationCompleted`）
天然映射，比 Chat Completions 从 `choices[].delta` 凑更干净。阶段 3 的 SSE framing 是共享组件，Responses
decoder 落地后事件侧能复用。

### 5.3 阶段 6 的状态

阶段 6（托管上下文）原为"后续边界"。鉴于 §5.1 已将有状态 Responses 判为架构否决，阶段 6 从"后续边界"改为
"当前裁决不做"，保留本文作为否决记录。若未来 Provider 生态发生根本变化（多厂商广泛支持有状态 Responses 且
提供可靠的数据保留/删除/权限变更契约），可重新评审。

## 6. 分阶段迁移与验收

| 阶段 | 主要变更 | 必要验证 |
| --- | --- | --- |
| 0：参考 Gateway 接线（已实现） | factory/coordinator 注入及默认组装 | 同步/异步工具 follow-up、未注册错误、真实 HTTP 闭环 |
| 1：公共接口抽离（已实现） | 公共 DTO/client/operation 与具体 Provider 头文件分离 | 现有客户端、Persona、Memory、Document、Skill 行为回归及 SDK consumer 构建 |
| 2：协议实现抽离（已实现） | Chat Completions 编解码改为协议多态，同步/异步共用 | 请求线格式、校验、错误、UTF-8、工具关联和用量回归 |
| 3：出站流式基础（已实现） | 协程增量 HTTP、SSE framing、Chat Completions decoder | 任意分块、跨 UTF-8、CRLF/多行事件、损坏输入、缺失终态、取消、deadline |
| 4：端到端 streaming（已实现） | Persona/Interaction sink、Gateway HTTP SSE、有界重放 | 首片段可见、背压、断开/重连、工具前文本暂存、同 Session 保序、单次 commit |
| 5：Responses 适配 | 新协议实现、output item/reasoning/工具结果映射 | 实际 Provider 能力、完整与流式协议一致性、旧协议回归 |
| 6：托管上下文 | 状态策略、恢复契约及可选托管检索 | A/B 质量、旧证据失效、缓存补齐、切换恢复和工具副作用 |

阶段 3 与 4 共同构成 streaming 可交付能力，不能仅完成 decoder 就宣称端到端已支持。
普通 Complete 与 streaming 应共享聚合与校验逻辑；对不支持流式的 Provider，可保留原完整接口，
但不能把完整答案人为切片标称为上游 streaming。

阶段 0 的回归位于 `tests/service/persona_gateway_tool_calling_test.cpp`，覆盖默认装配、
自定义 coordinator 优先级、未注册执行器的结构化错误、依赖不完整拒绝，以及真实 HTTP 请求闭环。
这些用例使用测试 LLM 和执行器，验证 Gateway 接线；不构成真实模型工具选择能力的验收。

首期 Gateway transport 建议优先复用 WebSocket 对话通道。
HTTP SSE 若同时需要，应单独核实入站 Server 的分块发送与关闭能力，并加入相应测试和排期。
输出模式应显式表达：未支持 streaming 时请求返回可解释错误，避免参数被忽略。

性能验收记录 TTFT、generation end、Turn complete 的 P50/P95/P99、吞吐、取消收口时间、
慢消费者队列峰值及内存占用。复用现有 Fake LLM/延迟服务扩展流式测试输入，真实 Provider 作为兼容验收。
新增压测遵守仓库既有性能报告边界，不把 Mock 延迟或单次实验当成生产容量保证。

## 7. 设计阶段的决策记录

阶段 3–4 已冻结：HTTP SSE 为首期 transport；sink 与 completion 分离；断连取消、有界队列
失败收口、工具首轮文本暂存；Memory Admission 保留既有异步策略；先覆盖已有两轮工具闭环。
下列条目保留原设计讨论脉络，实际协议以 [HTTP SSE 协议](LLM_HTTP_SSE_PROTOCOL.md) 为准。

1. 公共 LLM 模型先抽出已有类型并保留别名，还是同时引入有序 item？建议先抽离，随后按 streaming 必需项扩展。
2. 事件 sink 与最终 completion 是否分开？建议分开，以区分可见增量与已提交 Turn。
3. 首期输出 transport 采用 WebSocket，还是同时实现 HTTP SSE？建议先覆盖已有 WebSocket 对话路径。
4. 客户端断开后的默认策略与慢消费者策略？建议取消交互生成，并使用有界队列失败收口。
5. 工具首轮文本是否展示，AI Emotion/Memory Admission 哪些属于提交前必需步骤？需按产品语义冻结。
6. 多轮工具循环是否与首期 streaming 同时交付？建议先保证已有两轮闭环可流式运行，再独立扩展循环预算。
7. Responses 的上游会话状态是否独立验收？建议保持独立，避免协议切换同时改变上下文来源。

阶段 1–4 的实际接口见文首及 HTTP SSE 协议；上游状态与 Responses 决策仍需独立评审。
