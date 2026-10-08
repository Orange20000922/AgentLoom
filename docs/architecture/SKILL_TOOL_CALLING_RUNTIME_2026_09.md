# Skill Tool-Calling 运行时（2026-09）

> 状态：当前实现 + 测试结论
>
> 范围：`src/skill/` 工具调用协议层、L4 工具记忆召回、Persona 异步工具调用收口
>
> 面向：下游 Application 策略物化层的复用（工具工作流 + grounding + 结构化结果回填）

## 1. 实现现状

`src/skill/` 是一层独立的 **LLM 工具调用协议运行时**，把「工具声明 → 调用解析 → 执行 → 结果回填」这一 OpenAI-compatible function-calling 协议独立成模块，与既有的 Skill Session 生命周期（`skill_session_manager`，面向媒体/视觉长任务）解耦。

| 文件 | 职责 |
|---|---|
| `skill_manifest.h` | `SkillManifest` / `SkillExecutorSpec` / `ExecutionMode` 与 `ValidateManifest()` |
| `skill_registry.h/.cpp` | `ISkillRegistry` + `InMemorySkillRegistry`，按 `skill_id@version` 版本化存储，`tool_name` 全局唯一 |
| `skill_prompt_compiler.h/.cpp` | `ParseToolCalls()`、`MakeToolResultMessage()`、`ISkillPromptCompiler` |
| `skill_executor.h/.cpp` | `ISkillExecutor` 工厂、`SkillInvocationService`、`SkillToolCallCoordinator`（同步/异步编排） |
| `skill_manifest_json.h` | manifest 字段映射（内联，供 registry 与配置段共用，避免重复） |
| `config/sections/skill_registry_config_section.cpp` | `skills` 配置段：内联 manifest 或目录加载（UTF-8 BOM 兼容、文件名 regex、相对路径解析） |

关键抽象均以 `I` 前缀抽象类呈现（`ISkillRegistry` / `ISkillExecutor` / `ISkillExecutorFactory` / `ISkillInvocationService` / `ISkillToolCallCoordinator` / `ISkillPromptCompiler`），当前生产实现为 `InMemory*` 系列，持久化/插件化（WASM/动态加载）作为后续按需接入点。

**L4 工具记忆**（`tool_memory_provider`）负责在 LLM 上下文里注入「该用哪些工具、怎么用」：向量召回（语义）+ 关键词正则（确定性）双通道，输出可注入 prompt 的工具指令块与工具定义。

## 2. 工作原理

### 2.1 工具调用协议（两轮）

```text
LLM 首轮（tool_round=0）
  -> 返回 tool_calls
  -> coordinator 解析 + 校验（必填参数、schema）
  -> 执行器 Start，经 Skill Session 生命周期执行
  -> 回调产生 SkillResult
  -> MakeToolResultMessage 编码为 role=tool 消息
  -> 回填 assistant(tool_calls) + tool 消息后，重投 LLM follow-up（tool_round=1）
```

要点：`ParseToolCalls` 校验必填参数并把 `call.tool_id` 归一化为真实 `skill_id`+`version`；`MakeToolResultMessage` 对成功回填 `result_json`、失败回填结构化 `{"error":{code,message}}`；prompt 编译产出 `<skill_tool_protocol>` 约束块 + 每工具 `<skill_tool>` 描述，强调「工具返回前不得声称外部结果」。

### 2.2 工具召回（关键词正则 + 向量）

`VectorToolMemoryProvider::Query` 组合两条通道：

- **向量通道**：query 编码 → 检索同 partition 的 `capability` 条目 → 按 `top_k`/`min_score` 取前若干，命中项从 `extra_metadata_json` 读 `tool_id`/`instruction`/`schema`。
- **关键词正则通道**：每个工具配置 `keywords`（正向触发）与 `negative_keywords`（否定排除），由 provider 按固定格式（逐词转义 + 竖线交替，对齐下游 `behavior_rule_config` 的 `QuoteMeta` 语义）编译成正则；命中正向词且未命中否定词时，确定性返回该工具。

两通道结果按 `tool_id` 去重、按 `priority`→`score` 排序后截断到 `top_k`，形成注入 prompt 的工具指令块与可调用工具定义。

### 2.3 异步收口

`persona_runtime` 在首轮 LLM 返回非空 `tool_calls` 且 `tool_round==0` 时，经 `ISkillToolCallCoordinator::ExecuteAsync` 异步执行工具并重投 follow-up，**释放 worker**；回调仅做状态更新与 continuation 投递。`SkillInvocationService` 把执行结果路由回 Skill Session 生命周期（终态 `Closed`/`Failed`），并维护 `active_executions_` 以支持取消。

## 3. 测试结果

### 3.1 单元测试

`src/skill/` 相关单测 28 例全通过，覆盖 manifest 校验/版本化注册去重、调用解析与必填参数校验、执行器工厂按 type/reference 解析、调用服务生命周期路由、协调器同步/异步/超时取消，以及 `PersonaRuntimeTest` 的异步工具调用收口。

### 3.2 真实模型 E2E（deepseek 系列）

| 报告 | 用例 | 结论 |
|---|---|---|
| 基础工具调用 | 4/4 | 显式/隐式请求、无工具请求、缺参压力全通过 |
| L4 工具记忆注入 + follow-up | 4/4 | L4 命中注入、两轮 follow-up 收口 |
| 复杂/对抗性 | 11/11 | 长上下文、多任务、明确拒绝、prompt 注入、歧义指代、结构化约束全通过 |

关键结论：结构化 `input_schema` + grounding 提示能稳定压制「无中生有」；prompt 注入用例中模型按协议先调用工具等待真实结果而非直接声称。

### 3.3 多工具召回判别（4 工具 14 用例）

在 `vision.observe` / `document.analyze` / `kb.query` / `profile.read` 四工具判别矩阵（正样本 + 近邻负样本 + 无关负样本）上：

| 召回策略 | 召回通过 | top-1 准确率 | 误召回 |
|---|---|---|---|
| 纯向量 | 12/14 | 9/11 | 0 |
| + 关键词正则（正向） | 13/14 | 10/11 | 1 |
| + 否定排除词 | **14/14** | **11/11** | **0** |

结论：**关键词正则（正+负）+ 向量召回**的组合在多工具场景达到 100% 召回、0 误召回。向量召回解决语义泛化，关键词正则解决近义碰撞与否定排除，二者互补。

## 4. 优势与缺陷

### 4.1 优势

- **协议闭环**：工具声明/解析/执行/回填/follow-up 全链路成型，有单测 + 真实模型 E2E 背书，可直接作为下游的工具工作流基础。
- **grounding 有效**：`<skill_tool_protocol>` 约束 + 结构化 schema 能稳定抑制「编造工具结果」。
- **可扩展**：所有核心角色以 `I+` 抽象类呈现，manifest 即扩展契约；JSON 配置声明工具，C++ 只保留执行逻辑。
- **召回互补**：关键词正则（确定性）与向量召回（语义）互补，解决单一通道的近义碰撞/否定盲点。
- **异步不阻塞**：工具执行异步化，worker 全程释放。

### 4.2 缺陷与待办

- **真实 executor 未接入**：`ISkillExecutor` 目前只有测试实现，业务工具（查 RAG/图谱/画像）需下游实现并注册。
- **provenance 链路未落地**：`SkillResult.provenance_json` 字段已声明但未在 `MakeToolResultMessage` 中并入输出，`kb_version`/`evidence_state` 尚需接线。
- **manifest 并发/确认语义未执行**：`execution_mode`、`max_parallel_per_session`、`requires_confirmation` 目前仅声明与校验，运行时未消费。
- **LLM 工具路由未达标**：召回 100% 后，LLM 对已注入工具的选择仍为 8/11，存在欠调用与误路由，属工具描述/指令的 prompt 工程问题，与召回机制解耦。
- **关键词正则的否定需显式配置**：否定排除依赖人工维护 `negative_keywords`，覆盖不足时仍可能误触发。

## 5. 使用方法（下游接入指南）

下游接入分两档：**简单工具**用「Manifest 配置 + 自定义执行器（继承 `ISkillExecutor`）」，**长任务/多轮交互**直接接入 `ISkillSessionManager` 状态机写更复杂的逻辑。两档共享同一套 registry / factory / L4 工具记忆装配。

### 5.1 简单工具：Manifest 配置 + 自定义执行器

**第一步：写 manifest**（`skills` 配置段的 `manifests` 数组，或 `manifest_directory` 下的 `*.skill.json` 文件）。

```json
{
  "skill_id": "crm.lookup",
  "version": "1.0.0",
  "tool_name": "crm_lookup",
  "kind": "native",
  "description": "查询客户的 CRM 基本资料与最近跟进记录。",
  "input_schema": {
    "type": "object",
    "required": ["reason"],
    "properties": {
      "reason": { "type": "string", "description": "查询原因" },
      "customer_name": { "type": "string", "description": "客户姓名（可选）" }
    }
  },
  "output_schema": { "type": "object" },
  "prompt_instruction": "仅在需要了解客户背景、跟进历史时调用；不要用它推断成交结果。",
  "keywords": ["客户", "CRM", "跟进"],
  "negative_keywords": ["不查", "不用查"],
  "l4_payload": "用户想查询客户资料、跟进历史或当前状态时使用该工具。",
  "intent": "customer",
  "executor": { "type": "native", "reference": "crm.lookup" }
}
```

字段含义：`skill_id` 唯一标识（可含点号）、`tool_name` 是 LLM 实际调用的 function 名（仅 `[A-Za-z0-9_-]`，全局唯一，空则自动由 `skill_id` 生成）、`input_schema` 的 `required` 会被强制校验、`keywords`/`negative_keywords` 编译成正则触发（正+负）、`l4_payload` 是向量召回的种子语义描述（空则回退 `description`）、`intent` 派生 `memory_hash` 后缀、`executor` 决定执行器解析。

**第二步：实现 `ISkillExecutor`**（`src/skill/skill_executor.h`）。

```cpp
class CrmLookupExecutor final : public agent::skill::ISkillExecutor {
public:
    core::Result<agent::service::persona::SkillSessionSnapshot> Start(
        const agent::skill::SkillExecutionRequest& request,
        agent::skill::SkillExecutionCallbacks callbacks) override {
        // 解析入参、查 CRM、回填结果。
        agent::skill::SkillResult result;
        result.call_id = request.call.call_id;
        result.skill_id = request.call.skill_id;
        result.result_json = R"({"found":true,"customer":{...}})";
        if (callbacks.on_result) callbacks.on_result(std::move(result));
        return agent::service::persona::SkillSessionSnapshot{};  // 简单工具：无长会话，返回空快照即可
    }
    core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
};
```

**第三步：装配**（registry + factory + session manager + coordinator；与 `skill_llm_e2e_test` 一致）。

```cpp
auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
registry->RegisterJson(manifest_json);  // 或 Register(manifest)

auto factory = std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
factory->RegisterReference("native", "crm.lookup", std::make_shared<CrmLookupExecutor>());
// 若多个 native 工具共用一个执行器，可用 factory->Register("native", executor) 作兜底。

auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
auto invocation = std::make_shared<agent::skill::SkillInvocationService>(registry, factory, sessions);
auto coordinator = std::make_shared<agent::skill::SkillToolCallCoordinator>(registry, invocation);
```

生产主链路（`agent_gateway_server`）从 `config.skill_manifests` 注册 `skill_registry`，
`dependencies.tool_memory_provider` 由 L4 种子写入 + `VectorToolMemoryProvider` 构造
（空 `skills.tool_memory_sqlite_path` 则跳过），并注入 `skill_registry` / `skill_session_manager`。
参考 Gateway 在这两个依赖齐全时自动创建 `SkillInvocationService` / `SkillToolCallCoordinator`
并注入 Persona Runtime，同步与异步工具 follow-up 共享该闭环。

下游通过 `PersonaGatewayServerDependencies::skill_executor_factory` 注入已注册业务执行器的工厂；
也可直接提供 `skill_tool_coordinator` 覆盖默认装配。未提供工厂时使用空工厂，未注册执行器的调用
向 LLM 回填结构化错误，不代表已经实现 CRM、RAG 等业务工具。未装配 Registry 或 Skill Session
且未提供自定义 coordinator 时，保留无工具协调器的运行方式；孤立的 factory 会在启动时被拒绝。

```cpp
agent::service::gateway::PersonaGatewayServerDependencies dependencies;
dependencies.skill_registry = registry;
dependencies.skill_executor_factory = factory;  // 已注册业务执行器的工厂。
dependencies.skill_session_manager = sessions;
// 其余 Memory / Emotion / LLM 依赖按正常流程设置；Server 自动组装 coordinator。
```

### 5.2 复杂逻辑：直接接入 Skill 会话状态机

对需要多轮、流式观察、外部资源就绪、关闭 drain 的**长任务 skill**（视觉观察、媒体处理等），不写一次性 `ISkillExecutor`，而是**直接接入 `ISkillSessionManager` 状态机**。这不是 Media Skill 专属——任何需要精细控制生命周期的场景都这么接，Media Skill 只是一个现成例子。

```text
Idle -> Starting -> Ready -> Running -> WaitingInput -> Closing -> Closed
                     \-> Failed                    \-> Expired
```

关键接口：

| 方法 | 语义 |
|---|---|
| `Start(req)` | 创建有限会话（execution_id/skill/session/user/arguments） |
| `MarkReady(...)` | 标记外部资源已就绪 |
| `RecordObservation(obs)` | 推送一条 observation（`should_inject_prompt` 控制是否注入） |
| `BeginClosing` / `CompleteClosing` | 进入关闭流程 / 完成 drain |
| `MarkFailed` / `Stop` | 失败终态 / 请求关闭 |
| `Get` / `CleanupExpired` | 快照查询 / 超时清理 |

**典型例子（Media Skill）**：`SkillVisionEventSink`（`src/service/persona/skill_vision_event_sink.h`）继承一个领域事件接口 `media::IVisionEventSink`，把外部视觉事件桥接进状态机：

```cpp
class SkillVisionEventSink final : public media::IVisionEventSink {
    std::shared_ptr<ISkillSessionManager> manager_;
    SkillVisionEventSinkOptions options_;
public:
    core::Status Publish(media::VisionEvent event) override {
        // 1. 查会话；不存在则按 auto_start_missing_session 自动 Start
        auto current = manager_->Get(event.session_id, options_.skill_id);
        if (!current.value().has_value()) {
            manager_->Start(/* skill_id / session_id / trace_id / source="vision_event" */);
        }
        // 2. 事件到达即 MarkReady（mark_ready_on_event）
        manager_->MarkReady(event.session_id, options_.skill_id,
                            "vision event stream ready", event.trace_id);
        // 3. 每条事件转成 observation；confidence/stale 决定 should_inject_prompt
        SkillObservation obs;
        obs.summary = event.SummaryText();
        obs.confidence = ObservationConfidence(event);
        obs.stale = event.rate_limited || event.duplicate;
        obs.should_inject_prompt = obs.confidence >= options_.min_prompt_confidence && !obs.stale;
        return manager_->RecordObservation(obs);
    }
};
```

这个模式的本质是「**领域事件源 → 状态机适配器**」：继承领域接口（`IVisionEventSink`）+ 注入 `ISkillSessionManager` + 把领域事件映射成状态机调用。视觉只是其中一种；媒体帧推理的 `MediaInferenceExecution`（`src/service/persona/media_inference_execution.h`）也走同一个 `ISkillSessionManager`，用 `AdmitFrame`/`BeginClosing`/`ObserveTerminal` 驱动 `Running→SealingInput→ReplayingSpool→Aggregating→Closed/Failed`。

两档的边界：**简单工具走 5.1**（一次性结果，`SkillInvocationService` 内部已把执行路由回状态机终态）；**需要长会话生命周期、流式观察或外部事件驱动的复杂逻辑，直接接入 `ISkillSessionManager` 状态机**。二者共享同一 registry/factory/L4 装配，manifest 无需区分。

### 5.3 有状态 Skill 的静态注册

需要在 C++ 中实现复杂状态推进、但仍希望由通用 Gateway 统一启动和停止的 Skill，使用
`stateful_skill_registry.h` 提供的宏注册工厂：

```cpp
class PolicyMaterializationExecution final
    : public agent::service::persona::IStatefulSkillExecution {
public:
    explicit PolicyMaterializationExecution(
        agent::service::persona::StatefulSkillExecutionContext context)
        : context_(std::move(context)) {}

    core::Status Start() override;
    core::Status Stop(const agent::service::persona::SkillSessionStopRequest& request) override;

private:
    agent::service::persona::StatefulSkillExecutionContext context_;
};

REGISTER_STATEFUL_SKILL(PolicyMaterializationExecution,
                        "policy.materialize",
                        "1.0.0");
```

宏只在静态初始化阶段登记 `skill_id`、版本和创建函数，不创建运行时资源。Application
组合根创建 `StatefulSkillExecutionRouter` 并共享现有 `ISkillSessionManager`；每次启动时
router 创建一个独立 execution，并将 `Start`/`Stop` 转交给它。execution 使用 manager
发布 `MarkReady`、`RecordObservation`、`BeginClosing`、`CompleteClosing` 或 `MarkFailed`。

注册项位于静态库时，最终可执行目标必须对包含注册对象的库使用 whole-archive，否则链接器
可能裁掉没有普通符号引用的静态 registrar。宏注册器与 `ISkillRegistry` 分工不同：前者
创建有状态 C++ execution，后者保存 LLM function-call manifest、schema 和工具描述。

Persona 会根据结构化 L4 命中动态触发已注册 Skill，并通过 `ISkillSessionManager::List`
注入当前会话的非终态 Skill snapshot；业务代码不应再依赖 `vision.observe` 等固定字符串。
