# 独立子 Agent 运行时

`service/agent/agent_task_runtime.h` 提供独立任务执行环境，以及接入已有 Skill 的 `SubagentSkillExecutor`。它不调用 `PersonaInteraction::SubmitTurn`，不创建 Persona Session，不检索或写入人格记忆，也不推进情绪状态。

## 装配

宿主创建 `AgentTaskRuntime`，注入异步 LLM、业务工具 registry/factory、现有 deadline scheduler 和已启动的 `core::ThreadPool`。线程池可与其他工作共享，也可单独配置；每个任务不占用专属线程。LLM 传输及工具执行必须遵守原异步接口的快速返回契约。

```cpp
using namespace agent;
runtime::AgentTaskOptions options;
options.model = "your-model";
auto tasks = std::make_shared<runtime::AgentTaskRuntime>(
    async_llm, business_registry, business_factory,
    deadline_scheduler, continuation_pool, options);

auto manifest = runtime::SubagentSkillManifest();
// 两个 Register 均返回 core::Status，宿主必须检查。
auto registered = parent_registry->Register(manifest);
auto attached = parent_factory->RegisterReference(
    "native", "agent.spawn",
    std::make_shared<runtime::SubagentSkillExecutor>(tasks, trusted_binding));
```

`trusted_binding` 实现 `ISubagentContextProvider::Prepare`，从可信宿主状态提供允许的 Skill ID、工具 policy、紧凑业务快照、结果限额及任务时限。调用参数只接受 `task` 和 `timeout_ms`；工具权限、模型和运行预算不能由模型参数授予。绑定器需要快速返回；必要的外部查询应由业务工具异步执行。

主代理的 manifest 默认为 Always，但 Always 不表示必调。宿主仍决定是否注册这个工具。子任务注册表仅包含宿主允许的工具；即使原业务注册表有其他 Always 工具，也不会自动进入子任务。RequiredTools 指向未授权目录中的工具时，计划明确失败。预取、授权、schema、来源验收复用现有 Skill 契约。

宿主必须强持有 `tasks`。Skill executor 弱持有运行时，避免复用同一 factory 时形成 runtime → factory → executor → runtime 所有权环；宿主释放运行时会触发 Close。

## 身份与任务结果

业务 `session_id/user_uuid/persona_id/trace_id` 来自已授权的父工具调用，原样传给业务 policy/executor。这样 Application 可继续按父 Session/trace 查找该轮约束包。绑定器应保存不可变快照或确保原业务数据的保留期覆盖子任务。

每个任务另有 `run_id`，私有 transcript、工具目录和 `SkillSessionManager`。因此两个任务可同时使用同一业务 Session 的同名工具，而不会重用同一活动 Skill 执行实例。工具执行器仍收到全局唯一的 `execution_id`，取消必须按此 ID 定位资源。任务隔离不会让原本不支持并发的业务 executor 自动变得线程安全。

子代理内部声明 `complete_task({"summary": "..."})`。它是执行循环的终止控制工具，不登记到共享 registry；父代理不拥有该能力。完成调用必须独占该批调用，通过 schema 和必需工具验收后直接返回，不再执行 follow-up。纯文本结束返回 `FailedPrecondition`；完成工具与业务调用混合返回 `InvalidArgument`，该批不执行任何业务工具。默认禁止将内置委派工具再次授予子任务。

主代理收到标准 SkillResult，包含 run ID、摘要、模型请求数、业务工具调用数及 Provider 报告的累计 token。预取计入工具调用数，终止控制工具不计入。摘要是模型生成的工具结果，不能自动提升为可信约束或已验证事实。

## 预算与协程

执行体使用已有 `core::async::task`、`CallbackAwaiter` 和 `CancellableCallbackAwaiter`。模型和工具等待期间挂起；恢复后先投递到宿主池，再继续规划，避免把业务循环放到传输回调线程。没有逐任务线程、阻塞 future 等待或新增调度器。

默认上限为 8 个活动任务、16 次模型请求、64 次业务工具调用、256 KiB 内容上下文、32768 个累计 token、每次最多输出 2048 token、60 秒宿主最长时限。请求 timeout、可信绑定器 timeout 与宿主上限共同约束实际时限；计时从接纳开始，包含排队。主代理外层工具期限也可通过 Cancel 提前终止子任务。

上下文限额计算声明、消息正文、reasoning 与调用内容的 UTF-8 字节，不是线上 JSON 编码后的精确网络字节。累计 token 依赖 Provider usage，缺失 usage 不能作为硬费用控制；请求次数、上下文字节和总时限仍独立生效。触达 token 上限可能在收到本次响应之后被发现，不能追回已发生的费用。

## 生命周期与关闭

`Start` 返回失败表示未接纳，不回调；成功返回租约表示已接纳，completion 恰好一次，允许在 Start 返回前完成。丢弃租约即取消，因此直接调用者必须保留租约；Skill adapter 自动持有它。完成回调必须快速返回，不得等待当前执行体 drain。

超时、父取消、租约释放、运行时 Close 和正常完成共享一个终态出口。先决定对外终态，再通知底层取消，避免同步 Cancel 回调把 Timeout 改成 Cancelled。晚到结果不再触发用户回调。LLM 等待仍保留执行体和准入槽，直到底层完成回调返回并且协程收尾。

`Close()` 停止接纳并取消活动任务，不阻塞等待。宿主应在非 Worker 的关闭流程中等待 `ActiveTasks()==0`，再关闭传输、deadline 事件循环和 continuation pool。若 Provider 违反取消后回调契约，活动任务会保留，不能通过销毁悬挂协程帧强制清理。`ActiveTasks` 衡量执行协程，不代表第三方工具资源已经物理释放；业务 executor 仍需负责自身的 drain，原 coordinator 的取消契约没有改变。

任务共享状态沿用 core awaiter 的 `shared_ptr` 所有权方式。现有 ObjectPool 借用裸 owner，无法安全覆盖迟到回调，因此这里没有将协程控制块塞进该池；未新增裸内存操作或平台专属 API。

## 验证入口与边界

`AgentTaskRuntimeTest.*` 覆盖多轮闭环、父取消、租约取消、deadline/晚到、容量占用、输入/完成校验、Required 验收、同 Session 并发隔离和主 Skill coordinator 接入。`SingleWorkerBoundedLoadReleasesAllLeases` 使用单 Worker、8 个任务槽、256 次任务，记录耗时并检查全部回收；它是调度压力回归，不代表真实模型吞吐。

`GatewaySubagentE2E.HttpDelegationCommitsOnlyParentTurn` 通过真实 HTTP、认证和 Gateway，执行主代理委派、子代理两轮工具查询及主代理回答，检查只有父 Turn 写入记忆和历史。模型使用确定性夹具，真实模型规划质量和下游生产数据权限需另行验证。

当前 Skill coordinator 对同一响应中的多个 spawn 调用仍按现有顺序执行；独立 runtime 支持并发 Start，但此版本不新增父代理 fan-out/join、后台持久任务、递归委派或子任务流式正文。Application 的实际装配由下游选择，SDK 不自动打开委派。
