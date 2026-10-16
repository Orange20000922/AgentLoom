# Skill 工具契约与可信 Turn 输入

更新日期：2026-10-16。本文记录 [#18](https://github.com/Orange20000922/AgentLoom/issues/18)
的第一批公共协议实现及剩余工作；不表示整个记忆治理 Issue 已完成。

## 分支审计与本批范围

分支基于 `1415826`，最初两个提交分别补充输入 schema 校验（`af33ca1`）以及
provenance 回填和本轮工具集合（`aac2267`）。继续审计发现未提交实现中有指定初始化成员顺序
导致的编译错误、重复写入 turn ID、同步超时取消持锁调用外部执行器、空工具集合语义不明确、
输出缺少字节限额和消费者验证接口等问题。本批统一修正并加入回归。

| 能力 | 本批实现 | 边界 |
|---|---|---|
| 输入契约 | 唯一调用 ID、UTF-8、调用数量/大小、注册版本与 schema 校验 | 使用明确的 schema 子集；未知断言拒绝注册 |
| 调用授权 | 本轮声明集合与 `ISkillToolPolicy::Authorize` 均须通过 | 关键词/向量召回本身不授予企业业务权限 |
| 输出契约 | schema、身份、UTF-8 JSON、大小限制与来源冲突校验 | 企业审批、scope、版本真实性由消费者验证 |
| 实际 tool 消息 | 旧内联格式可携带 provenance；可选版本化信封 | 默认保持旧字段布局，供消费者渐进迁移 |
| 主动事件 | 显式 `proactive_trigger`，参考课堂可信入口已适配 | HTTP/WS 普通消息 DTO 不从用户 JSON 读取该字段 |
| L0/recent 去重 | `turn_id` 排除近期记录，重叠邻接记录去重 | 旧记录/旧快照兼容完整问答文本匹配 |
| 输入上限 | 总 Unicode 码点上限与独立 L0 UTF-8 字节上限 | 超限明确失败；没有静默截断或按来源自动降级 |
| 工具轮次 | 同步/异步/流式均最多执行一轮工具加一次 follow-up | 第二次工具请求明确失败，不提交最终问答 |
| 同步超时 | 超时返回工具错误并 Cancel；取消前释放完成锁 | 异步 deadline/取消仍见下方待办 |

## Schema 子集与调用绑定

支持 `type`（含类型数组）、`enum`、`const`、`required`、`properties`、
`additionalProperties`（布尔或子 schema）、`items`、`minItems`、`maxItems`、
`minLength`、`maxLength`、`minimum`、`maximum`。允许 `title`、`description`、
`default`、`examples`、`$schema`、`$id`、`deprecated`、`readOnly`、`writeOnly` 注解。
注解不启用对应 draft 的其他能力；例如 `$ref`、`pattern`、`oneOf` 不支持并明确拒绝。

注册时检查整棵 schema，包括当前调用没有使用的属性。字符串长度按 UTF-8 Unicode 码点计数。
单个 schema 最多 64 KiB、嵌套深度最多 32；单轮最多 32 次调用，参数最多 64 KiB，
调用 ID 和 function name 最多各 256 字节。调用 ID 不能为空、重复、包含非法控制字符或非法 UTF-8。

工具通过 registry 解析到固定 `skill_id/version`。仍兼容旧 provider 将 skill ID 直接作为
function name 的协议；注册表禁止重复 function name，消费者授权接口拿到解析后的版本。
本批复用原 manifest/registry/coordinator；仓库没有可复用的完整 JSON Schema validator，
因此保留现有校验器并明确限定子集，避免宣称支持未实现的 draft。

## 消费者授权与来源验证

消费者实现 `ISkillToolPolicy`：

- `Authorize(context, call, manifest)`：执行前根据可信身份和业务上下文判断可否使用。
- `ValidateResult(context, call, manifest, result)`：公共输出 schema/大小通过后，检查来源、发布版本与引用。

返回 `core::Status`；拒绝转换为对应调用的工具错误，执行器不得运行或结果不得作为成功回填。
接口异常转换为 `InternalError`。接口实现必须线程安全，且不能阻塞等待外部服务；异步知识查询
继续由业务 executor 负责。`SkillToolCallContext` 不包含企业 tenant/scope 的固定语义，消费者可在
注入的 policy 中从 session/user/trace 解析自己的可信上下文。

通过 `PersonaRuntimeOptions::skill_tool_policy` 注入，`max_tool_result_bytes` 默认 64 KiB，
`tool_result_format` 默认 `LegacyInline`。Runtime 同步/异步路径均传递这些选项。
每轮声明的 function name 显式传入 `allowed_tool_names`：空集合拒绝全部；
仅旧的直接 coordinator 调用方可用 `nullopt` 保持未限定集合的兼容行为。

输出 ID 若为空按原调用补齐，若指向其他调用/Skill 则拒绝并保留原 `tool_call_id`。
先检查原始 result/provenance 总字节，再检查最终序列化消息；超限返回 `ResourceExhausted`。
错误控制消息保留身份，极小的业务 payload 配额可能小于固定错误信封本身。

## 结果格式

`LegacyInline` 保留原结果顶层字段；独立 `provenance_json` 为对象时并入 `provenance`。
原结果为数组或标量且另带来源时，包装为 `{result, provenance}`。
独立来源无效、不是对象或与结果里已有的来源冲突时，回填 `DataLoss`，不回填原业务内容。

选择 `EnvelopeV1` 后，Agent 实际收到：

```json
{
  "schema_version": "agentloom.tool-result.v1",
  "call_id": "call-1",
  "skill_id": "kb.query",
  "status": "success",
  "result": {"items": []},
  "provenance": {"source_id": "doc-1", "version": "v1"}
}
```

公共 `status` 为 `success`、`empty`、`error`。`empty` 指执行成功且 `result_json` 为空字符串；
显式 `{}` 或 `[]` 仍是消费者有意返回的成功 JSON。业务的无命中、不适用、过期、证据不足等
由 `result` 中的版本化领域字段表达。错误仅含 `error.code/message`，不携带被拒绝的业务结果。
独立来源在信封顶层，原来已经写在业务 result 内的来源仍保留。

## 主动事件、记忆与预算

普通 `[system_event]`、`[proactive]` 等文本保持 user 来源。合法主动发言由参考 Gateway 的
课堂主动请求/轮询 adapter 填写 `ChatGatewayRequest::proactive_trigger`，传至
`ChatRequest::proactive_trigger`。没有删除原主动事件调度或新建调度器。

新提交问答以 session + 提交序号生成 `ConversationTurn::turn_id`，随 L0 metadata 写入。
近期历史同时传完整问答和 ID；L0 命中和邻接记录均先做 owner/有效期/近期过滤，再合并去重。
`BuildL0RecallContext` 是无 I/O 的实际装配函数，adapter 和确定性测试共用，无额外 Session 容器。

`persona_gateway.runtime_max_prompt_characters` 默认 131072，计数包含消息正文、推理文本、
工具调用、消息 parts 和 tools 定义。首轮和 follow-up 发送前均检查，不是 token 或模型窗口估算。
`l0_memory.max_payload_bytes` 默认 65536，限制 L0 payload 的 UTF-8 字节。两者超限返回
`ResourceExhausted`，不静默丢弃部分证据；产品应据此选择明确的重试/降级策略。

公共结构增加字段且函数符号发生变化，安装新 SDK 后消费者必须全量重编译；不能混用旧对象文件。

## #18 仍需继续的工作

- 异步 coordinator 尚无统一 deadline/operation cancellation；外部 executor 必须自己设置 deadline 并可靠完成回调。
  本批修复同步取消死锁，没有新建线程/计时器或用阻塞等待模拟异步超时。
- L0/L3/L4/Skill observation 的引用数据边界仍需整体升级；已有记忆仍可能作为 system 中的原始材料。
  可信主动事件修复不能替代这一项，也不能视为已消除模型层提示注入风险。
- L4 的召回文本/schema 与 registry 规范声明的统一、主动 Skill 启动授权仍待实现；本批 policy 覆盖 LLM 工具执行。
- L3 事实的用户/模型角色区分、证据与不确定性、历史事实迁移仍待实现。
- 分来源预算、必要上下文保留、按来源降级和每次调用的分来源观测尚未实现。
- failure 的 finish_reason/usage 保真、固定模型真实回归、下游企业跨层契约及 Overview 联调需后续完成。

本批 PR 仅关联 #18，不能自动关闭它；企业 scope/审批事实仍由下游 Application #16 跟踪。

## 验证方式

复用 GTest 的 Persona/Skill、Gateway HTTP/流式工具回归、Config 和 semantic cache 测试目标，
并在安装包 consumer 中直接链接公共 schema/来源信封和消费者授权接口。
本地测试使用合成 LLM/知识内容和真实本地 HTTP/SQLite，不调用真实云模型。
具体通过数量与运行环境记录于 PR，测试 XML 由构建目录留档。
