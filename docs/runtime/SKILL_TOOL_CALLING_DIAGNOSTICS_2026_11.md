# 工具欠调用诊断与分层验证

日期：2026-11-10。关联 [Issue #14](https://github.com/Orange20000922/AgentLoom/issues/14)。

## 已确认的问题与修复

1. `RequiredTools` 已有结果验收，但同步、异步及流式请求此前统一使用 `tool_choice=auto`。
   现在尚有未满足的必需工具时使用 `required`，全部满足后恢复 `auto`，防止 follow-up 被强制继续调用。
   Prefetch 已经满足的工具不再强制；Always、Recall 本身不代表必须执行。
   `required` 只保证至少调用一个工具，仍需按名称、授权、schema 和执行结果验收全部必需项。
2. 旧 14 例矩阵的文档、画像输入没有提供 schema 所需的 `doc_id` / `customer_id`。
   模型追问这些标识是合理的缺参澄清，不能只凭空 `tool_calls` 判为协议故障。
   强制未补参请求确实可以提高目标工具命中，但本次也观察到 `unknown`、`current_document` 等猜造标识。
3. 旧测试固定 `min_score=0`，虽按 0.5 评估判别分数，却将低分工具一起声明给模型。
   生产默认值为 0.78。测试现在默认读取 `skills.tool_memory_min_score/top_k`，显式 `wide` 模式保留旧实验。
4. 原测试仅统计正样本、只看是否包含目标工具，可能忽略额外调用；退出码只检查召回。
   现在负样本也实际请求模型，记录所有调用参数、额外调用、执行结果与 follow-up，路由或闭环失败返回非零。
5. 测试 executor 原先只返回 `ok=true`。新增明确的合成观察、合同分析、画像和知识库事实，
   用于区分测试证据不足导致的重复调用与首轮欠调用。报告标记 `fixture_version=2`。

这些修复复用 `LoggerAdapter`、`ILlmProtocol`、`BuildSkillTurnPlan`、`SkillToolCallCoordinator` 及现有构建脚本。
没有增加 DSML 兜底解析，也没有把模型正文转换成可执行调用。

## 诊断日志

参见 [配置系统](CONFIG_SYSTEM.md) 的 `llm.debug_payloads` / `debug_payload_max_bytes`。
客户端在 Debug 下记录编码请求、解码前原始响应、解码结果及错误；保留返回的 reasoning 和 finish_reason。
SSE 原始字节有界收集后输出，日志截断保留 UTF-8 首尾和省略计数。
真实测试工具自动写入 `<report.jsonl>.payload.log`，并以 case ID 和请求 ID 关联记录。

判断顺序：

- 原始 `tool_calls` 非空但解析后缺失：本地解析问题。
- 原始 `tool_calls` 空且正文出现 DSML：先检查当前请求是否声明了对应工具，再检查截断与具体语法。未声明工具时的正文伪调用不能证明合法工具调用被解析器丢失。
- 正文干净且缺少业务参数：先检查是否合理澄清。
- 调用错误工具或额外工具：检查实际声明集合、低分候选、宿主上下文与模型多步计划。
- `finish_reason=length`：保留原始证据，同时按已有协议校验返回截断错误。

reasoning 中的文字意图本身不能证明服务端解析失败；上游未返回该字段时只能记录缺失。

## 实验边界

请求模型保持配置中的 `deepseek-chat`，本次响应实际模型字段为 `deepseek-flash`，没有非空 reasoning。
因此这些结果不能视为旧报告模型版本的精确复现，也不能据此推断模型未思考工具。
各矩阵均为单次采样，不是稳定成功率或统计显著性结论；测试 executor 使用合成数据，不验证真实业务数据质量。

14 例保留现有输入。`bound` 组额外提供固定宿主标识 `contract-001` / `customer-001`，
不会改写原始用户输入。全量可见工具可能令模型补查其他信息；目标命中率与“只调用目标工具”的严格路由率分别统计。

历史 11 条对抗输入未保存在当前仓库中，本次根据文档覆盖项重建 11 条输入并明确标记。
覆盖长上下文、多任务、明确拒绝、恶意引用、歧义视觉指代、结构化回复及无工具负样本。
不得将它们宣称为历史输入的逐字复现。

## 本次结果

最终四组均使用生产默认召回阈值 0.78、补齐宿主标识和版本 2 合成证据。
“严格路由”要求正样本只调用目标工具，负样本不调用；它比“包含目标工具”更严格。

| 层级/输入 | 召回判别 | 正样本目标命中 | 严格路由（含负样本） | 已调用样本 follow-up |
|---|---:|---:|---:|---:|
| Recall / 原 14 例 | 13/14 | 10/11 | 13/14 | 10/10 |
| Always / 原 14 例 | 13/14 | 11/11 | 12/14 | 11/11 |
| Require / 原 14 例 | 13/14 | 11/11 | 14/14 | 11/11 |
| Recall / 重建 11 例对抗集 | 10/11 | 8/8 | 11/11 | 8/8 |

上述 53 条用例产生 93 个完整 HTTP 响应。逐项对照原始与解码后的 content、reasoning、调用 ID、
工具名和参数：0 处丢失/变化，0 条正文 DSML 候选，0 条日志截断，0 条非空 reasoning。
这支持“本轮未复现 DSML/本地解析丢失”，不能证明所有模型、Provider 或截断条件下都不存在泄漏。

保留的失败及后续方向：

- Recall 的 `kb_en`（英文询问如何应对压价）没有任何候选，发送请求的 `tools` 为空。
  原种子英语关键词只有 `objection`，此输入不包含该词；向量分数未过 0.78。
  应在下游种子补充英语压价语义/关键词并重新校准召回阈值，不能靠 DSML 或强制未声明工具修复。
- Always 的 `kb_en`、`kb_cn` 同时调用知识库和画像。工具全部可见且宿主上下文提供了客户 ID，
  模型把画像当成辅助查询。若业务禁止这种扩展，需要缩小 Always 集合或明确工作流允许的工具范围；
  若业务允许多步补充信息，应相应调整单目标验收，不能将所有额外查询都视为解析错误。
- 对抗集 `explicit_tool_refusal` 仍通过关键词召回视觉工具，暴露否定表达覆盖不足；
  模型只回答“收到”，没有调用。这是召回误注入，模型行为本轮正确。
- 四个测试进程均因上述召回或严格路由项返回非零，不能把目标命中或 follow-up 通过宣称为整个 E2E 全绿。

历史宽召回（阈值 0）未补参组中 Recall 为 4/11、Always 为 5/11、Require 为 11/11。
Require 的文档/画像调用出现无依据的占位 ID，因此提高调用率不等于业务参数正确。
补参后的宽召回组出现额外工具调用和多步计划，也说明旧 top-1 召回统计不能替代真实声明集合审计。

确定性测试共 137 项通过：LLM 79、HTTP/SSE 10、Required 同步/异步/流式 21、Gateway 工具调用 15、LLM 配置 12。
Gateway 主程序和真实模型测试工具均通过现有脚本构建。真实模型矩阵不计入这 137 项。

## 复现入口

通过现有 Windows 脚本在已配置的测试目录构建：

```powershell
./windows/scripts/build.ps1 -BuildDir build/your-tests -Target llm_tests,llm_streaming_tests,gateway_service_tests,skill_llm_e2e_test,config_tests -Jobs 4
./windows/scripts/test.ps1 -BuildDir build/your-tests -Include 'PayloadTraceTest|RequiredToolRuntimeTest|GatewayToolCalling|LlmStreamTransport|ConfigLlmSectionTest' -SkipPackage
```

真实模型配置需包含 `llm` 认证来源、TLS CA 和 `embedding` 模型；凭据保留在 ignored 本机配置。
报告目录需提前创建；每次使用不同报告名，避免覆盖原始证据。测试会调用收费 Provider。

```text
skill_llm_e2e_test <config.json> <report.jsonl> recall   bound matrix production
skill_llm_e2e_test <config.json> <report.jsonl> always   bound matrix production
skill_llm_e2e_test <config.json> <report.jsonl> required bound matrix production
skill_llm_e2e_test <config.json> <report.jsonl> recall   bound adversarial production
skill_llm_e2e_test <config.json> <report.jsonl> recall   unbound matrix wide
```

真实测试入口调用共享计划、客户端、执行器和结果协议；完整 Gateway 的同步/异步/流式生命周期由确定性回归单独覆盖。
它不是完整生产 Gateway 的真实模型端到端验收，也不替代下游对标识、权限及来源事实的校验。

## 后续受控实验：欠调用与额外调用

本节为同日后续实验，不能与前述未开启 thinking 的 53 条用例混算。
请求与响应模型均为 `deepseek-flash`，在同一 Provider 上显式切换 `thinking.type`。
复用生产协议编码/解析、工具计划和执行器；测试专用协议装饰器只注入 thinking 与固定的 reasoning_effort。
模型模式参考 [DeepSeek thinking mode](https://api-docs.deepseek.com/guides/thinking_mode/)。
原始首轮请求逐项比较确认，两组 thinking A/B 除开关外一致。

冻结的 8 条重点输入各重复 2 次，包含知识库、文档、画像与不应调用工具的输入；
温度 1、top_p 1、最大输出 8192，提供可信宿主标识。scoped 提示词明确：
标识只是参数，不代表访问请求；销售建议不默认补查画像/合同；证据足够即回答；缺少必需标识时澄清。

| 层级 / 提示词 / thinking | 严格路由 | 正样本目标命中 | 首轮非空 reasoning | follow-up 未通过 |
|---|---:|---:|---:|---:|
| Always / baseline / off | 15/16 | 12/12 | 0 | 1 |
| Always / baseline / on | 9/16 | 12/12 | 16 | 2 |
| Always / scoped / off | 16/16 | 12/12 | 0 | 0 |
| Always / scoped / on | 16/16 | 12/12 | 14 | 0 |
| Recall / scoped / off | 12/16 | 8/12 | 0 | 0 |
| Required / scoped / off | 16/16 | 12/12 | 0 | 0 |

严格路由不包含 follow-up 验收，二者分别统计；上述结果不是完整 E2E 全绿结论。
Required 组由测试夹具按预期目标设置必调工具，验证的是约束执行，不能证明生产系统能正确判定所有必调场景。

### 欠调用：先区分不可用与未选择

Recall 的失败集中在 `kb_en` 与 `hold_price_en`，各重复两次。
编码后的请求没有工具声明，也没有 tool_choice；不是模型在已声明目标工具中选择不调用。
同样输入在 Always/scoped 中目标命中，说明这批样本应优先修复召回覆盖。
现有关键词不覆盖这些英文表达，向量分数也未超过当前 0.78 阈值。

已完成的关键词对照中，补充正向与否定关键词将召回集合完全正确的输入由开发集 23/25、
留出集 9/20 提升到 25/25、19/20；仍有失败。这是当前工具种子的覆盖证据，
不能推论通用 Embedding 模型性能，也没有在此开展教师标注、模型训练或标定。
直接改写种子语言在当前阈值下未改变集合命中结果，不能据此认为单靠翻译种子就能解决。

### 额外调用：可见性与任务边界

baseline 中销售问题会附带画像查询，回答将其解释为个性化建议；可信客户 ID 又使该辅助查询具备参数。
thinking 开启组额外调用更多，但这只是小样本观测，不是 thinking 必然降低稳定性的结论。
保持相同工具集合，仅收紧任务范围后，两种模式均为 16/16 严格路由。
因此应优先明确业务是否允许辅助查询，并收缩 Always 集合；不宜将所有工具改为 Required。

### DSML：已复现正文异常，但未复现本地解析丢失

Recall/scoped 的空工具请求出现两次 DSML 正文及一次普通 `<tool_call>` 正文。
针对相同的两条漏召回输入，又各重复三次验证：

| 空工具请求对照 | 输入数 | DSML 正文 | 普通工具调用标记正文 | 合法工具调用 |
|---|---:|---:|---:|---:|
| scoped 原提示（主矩阵对应子集） | 4 | 2 | 1 | 0 |
| 删除提示中的具体函数名 | 6 | 2 | 1 | 0 |
| 保留函数名并追加仅可调用已声明工具的约束 | 6 | 3 | 0 | 0 |

后两组异常均出现在明确要求查询销售手册的 `hold_price_en`；去掉函数名后模型甚至自行编造函数名。
这否定了“只删除工具名或增加一句禁止标记就足以修复”的判断。
空声明与检索要求的冲突是本次异常的共同上下文；尚不能进一步区分模型原生输出与 Provider 转换的责任。

后续共 108 条用例、176 个 HTTP 响应，原始/解码字段差异为 0，日志截断为 0，API 错误为 0；
DSML 正文 7 次，另有普通工具调用标记 2 次，均发生在无工具声明的请求。
有合法工具声明的请求未复现正文 DSML，也未发现结构化 tool_calls 被本地解析器丢弃。
早期 93 个响应未发现 DSML 的记录仍成立，但不能再概括为整个排查过程未复现 DSML。

### 改进顺序与复现

1. 修补已确认的语言/否定关键词覆盖，保留漏召回与误召回反例；按实际声明集合判断调用责任。
2. 下游工具提示词明确允许的任务边界，按需使用 Recall，只有跨任务确实必需的能力才使用 Always。
3. Required 只用于可信工作流确定的必需步骤，并继续校验具体工具与参数；不能作为低置信召回的兜底。
4. 工具不可用时，宿主应提供明确的不可用路径，并使任务指令与当前能力一致。单句提示约束尚未解决本次异常，不能标为已修复。
5. 正文 DSML 保留为异常证据，不转换为可执行调用，尤其不能执行请求未声明的工具。

本轮没有将测试领域的销售提示词硬编码到生产通用运行时。生产修复有效性仍需下游真实工作流验收。
每组仅 2–3 次重复，且使用同一 Provider；顺序运行的采样结果不代表统计显著性或其他供应商表现。

实验定义为 `tools/skill_experiments/ablation_v1.json`；使用全新输出目录保存证据：

```text
python tools/run_skill_ablation.py llm --base-config <private-config.json> --output build/reports/tool-ablation-new
python tools/analyze_skill_ablation.py build/reports/tool-ablation-new
```

可通过 `--only llm_recall_no_named_tools_off llm_recall_availability_guard_off` 仅复现空声明对照。
脚本拒绝覆盖已有 JSONL；真实测试会访问收费 Provider。实验摘要分别保留路由失败、
工具声明缺失、正文标记、follow-up 失败与原始响应审计结果，不将它们合并成单一成功率。
