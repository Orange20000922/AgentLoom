# HTTP SSE 流式实现验证与压测（2026-10）

## 结论与范围

2026-10-09 验证了 Chat Completions 上游真实增量、Persona 事件通道与
Gateway HTTP SSE。新网络路径使用 Boost.Asio 协程，共享已有 I/O、TLS、连接池、
core 内存池和有界队列。协议说明见 [HTTP SSE 契约](../architecture/LLM_HTTP_SSE_PROTOCOL.md)。

Fake Provider + CUDA MiniLM L0、Emotion 关闭的稳定阶梯完成 **570/570 Chat，零错误**。
300 并发的独立容量边界完成 256 Chat、44 次 HTTP 429，符合配置的 256 个活跃 SSE Turn 上限。
真实 DeepSeek HTTPS/SSE 单次兼容请求成功，不作为真实 Provider 容量数据。

本次没有受控普通响应/SSE A/B，不能据此计算性能提升百分比。Mock 将首个 delta 放在固定
2000ms 延迟的四分之一处，属于人工测试输入；不能将其 TTFT 当作实际模型速度。
响应只有两个短文本片段，本轮阶梯不代表长输出、长工具循环、Emotion 全链路或生产 SLO。

## 数据与来源

代码基线 `69dfef0eecdb5a43b486730bbc93ccd954efb3ce`，加本次未提交工作区。
原始报告目录：`build/reports/sse_20261009/`。`summary.json` 保存汇总和收集时工作区文件 SHA-256。
容量采样在最终的配置校验、背压错误保留、暂存文本上限与文件拆分之前执行；最终版本另外完成
对应回归，不把此前采样描述为最终每个文件版本的严格性能数据。

| 文件 | 分类 |
| --- | --- |
| cold_warmup.json | 诊断失败：包装器未转发 streaming capability，HTTP 500，零成功 Chat |
| prewarm_fixed.json | 修复后 CUDA 冷启动/实际推理预热，1/1 Chat 成功 |
| warm_c10.json / warm_c50.json / warm_c100.json / warm_c250.json | 完整成功阶梯 |
| boundary_c300.json | 256 成功、44 次 admission 拒绝的容量边界 |
| mock_metrics.json | Provider 指标，不能独立算 Gateway E2E |
| real_provider_smoke.log | 一次真实 HTTPS/SSE 兼容验收，不打印回复正文 |

较早失败包装器在 E2E 与生产组合根均修复；性能表排除 diagnostic 和冷启动样本。

## 环境与复现配置

| 项目 | 实际值 |
| --- | --- |
| OS / CPU / 物理内存 | Windows 11 10.0.26200 / Ryzen 9 8940HX / 16,962,326,528 bytes |
| GPU / driver / VRAM | RTX 5060 Laptop GPU / 592.01 / 8151 MiB |
| CMake / generator / MSVC | 4.3.3 / Visual Studio 18 2026 x64 / 14.50.35717（v145） |
| build / triplet | build/x64-Release-All-v145，Release / x64-windows |
| ONNX Runtime / CUDA | GPU 1.20.1；CUDA 12、cuDNN 9 runtime |
| Embedding / readiness | MiniLM 384 维；日志明确 provider=cuda；实际 Chat 预热成功后才采样 |
| Emotion | neutral；未启用独立模型进程，无 Emotion CUDA readiness 结论 |
| Redis | 用户启动的 WSL Linux Redis 7.0.15；独立 Linux install，压测不使用 Windows Redis |
| SQLite / L0 | 生产标准 CUDA L0；SQLite admission；auth 使用 SQLite |
| Provider | 既有 C++ quota Mock；2000ms、quota 500、8 个 HTTP IO 线程 |
| Gateway pools | HTTP IO 8；compute 2/queue 1024；IO 4/queue 2048；LLM continuation 8/queue 2048 |
| 出站 HTTP | IO 4；keep-alive 开启；idle pool 512/origin 512；idle 30s |
| SSE | 活跃/保留 Turn 256；队列 256 个事件/1 MiB；每 Turn 重放 128 事件/512 KiB；保留 30s |

公开输入为 `tools/persona_gateway_sse_benchmark.example.json`，环境专属副本为已忽略的
`tools/persona_gateway_sse_benchmark.local.json`；该副本只替换可达的 Redis host。
本次 Windows localhost 转发不可达，使用 WSL 直接可达地址；该地址属于实验环境，不是部署约束。
磁盘设备/文件系统类型未收集。构建目录启用 Local LLM/Media；既有 llama.cpp 预构建工具集警告
与本次 HTTP/SSE 路径无关，不作为 Local LLM ABI 已验收的结论。

复现入口：

```text
openai_compatible_quota_mock_server 18181 2000 500 8
persona_gateway_e2e_server tools/persona_gateway_sse_benchmark.local.json
python tools/gateway_concurrency_benchmark.py --stream --base-url http://127.0.0.1:18180 \
  --concurrency 100 --turns 2 --timeout 60 --include-records \
  --output build/reports/sse_20261009/warm_c100.json
```

10/50/100 并发各两轮，250/300 并发各一轮；chat-start barrier 开启，setup 并发、
无 think/ramp-up。各轮之间不重启 Gateway/Mock。所有模型路径在配置中使用相对路径。

## 稳定阶梯与边界

以下延迟单位为 ms，三元组依次为 P50 / P95 / P99。TTFT 从 Chat POST 开始到客户端解码
首个 TextDelta；generation end 到客户端解码最后一次 GenerationCompleted；Turn complete 到
客户端解码 TurnCompleted。各分位数直接计算，不用“P95 相减”推导阶段延迟。

| 并发 × 轮次 | 成功 / 请求 | 错误 | burst Chat/s | TTFT | Generation end | Turn complete |
| --- | --- | --- | --- | --- | --- | --- |
| 10 × 2 | 20/20 | 0 | 3.28 | 1165 / 1226 / 1226 | 2668 / 2742 / 2742 | 3012 / 3089 / 3090 |
| 50 × 2 | 100/100 | 0 | 13.20 | 1504 / 1832 / 1840 | 3018 / 3343 / 3351 | 3390 / 4120 / 4423 |
| 100 × 2 | 200/200 | 0 | 20.06 | 1721 / 2929 / 3022 | 3234 / 4443 / 4536 | 3816 / 5681 / 6041 |
| 250 × 1 | 250/250 | 0 | 26.92 | 3111 / 5092 / 5371 | 4626 / 6592 / 6885 | 7186 / 9205 / 9224 |
| 300 × 1（边界） | 256/300 | 44 × HTTP 429 | 28.77* | 3495 / 5615 / 5908 | 5006 / 7127 / 7419 | 7845 / 10050 / 10349 |

\* 原 `chatMeasurementWindow` 的吞吐计入完成的失败请求。300 并发的 28.77 是尝试完成吞吐，
不能当作成功 Chat 吞吐；按成功数计算约 24.55 Chat/s，成功比例 85.33%。成功样本的延迟不包含 44 个拒绝请求。
所有轮次 setup/close 均零错误；拒绝只发生在边界的 Chat admission。

| 并发 | Generation end → Turn complete P50/P95/P99 | RSS peak MiB | Private peak MiB | Threads peak |
| --- | --- | --- | --- | --- |
| 10 | 347 / 360 / 376 | 1572.6 | 2881.6 | 72 |
| 50 | 358 / 778 / 1093 | 1570.0 | 2895.8 | 72 |
| 100 | 714 / 1355 / 1510 | 1600.0 | 2934.0 | 72 |
| 250 | 2544 / 2589 / 2617 | 1616.8 | 2949.6 | 72 |
| 300（成功样本） | 2902 / 2985 / 3017 | 1631.9 | 2962.2 | 72 |

`private` 是 psutil 对 Windows private bytes 的采样，与 RSS 含义不同，不能相互替代。
按 0.5s 间隔采样，不保证捕获瞬时最高值。Turn complete 延迟包括 memory admission、commit
与写/网络可见等待；不是纯数据库时间。

## 后端阶段与瓶颈证据

原有 pipeline 字段的 P95：

| 并发 | backendTotal | computeStage | Memory context | LLM total | IO stage |
| --- | --- | --- | --- | --- | --- |
| 10 | 2733 | 713 | 713 | 2022 | 2022 |
| 50 | 3198 | 1178 | 1178 | 2029 | 2029 |
| 100 | 3532 | 1505 | 1505 | 2028 | 2028 |
| 250 | 6366 | 4349 | 4348 | 2025 | 2025 |
| 300（成功样本） | 6872 | 4849 | 4849 | 2029 | 2029 |

computeQueueWait、ioQueueWait、callbackToResponse P95 均为 0ms，这是现有埋点及毫秒取整
口径，不说明没有排队或提交等待。`backendTotal` 不覆盖全部后续 Admission/commit。
Provider 固定延迟稳定在约 2.03s，而 Memory context 与 generation-to-commit 随并发增长；
本轮不支持“SSE 写队列是主要瓶颈”的判断，后续应独立细分 Admission/SQLite/commit。

网络关闭日志观察到队列峰值 **3 个事件 / 1021 bytes，拒绝 0**。TurnCompleted 内的队列快照
在终态自身入队之前采样，不能覆盖最后一个事件；准确峰值以 sse.closed 日志为准。
Mock 总完成 827（包含预热和边界），peakInflight 128、peakQueued 1、拒绝 0、采样结束 inflight/queued 0。

## 边界与正确性验证

decoder 覆盖每个字节切分点、UTF-8/emoji、CRLF、BOM、多行 data、坏 JSON/UTF-8、usage
整数溢出/快照、工具参数聚合、容量上限、缺失终态与终态后数据。真实 socket 测试覆盖
坏 HTTP chunk、截断 body、错误 Content-Type/status、首片段早于 Provider end、
Cancel/deadline/Shutdown 最终 callback 恰好一次，以及失败流没有透明重试。

Gateway E2E 覆盖同 Session 保序、单次 Memory admission/commit、工具前文本隐藏和两轮
generation、客户端断连取消、Last-Event-ID 后缀重放且无新的 Provider 请求、
AI 后处理失败 committed=false、选定的 Memory Admission 失败策略。
队列溢出返回 ResourceExhausted、取消上游且不提交；心跳/pong/idle 和服务停止分别有真实 socket 测试。
keep-alive 测试验证普通与流式请求共享连接，idle 回收后重新连接。

单次 Gateway 断连→上游关闭回归观察约 **0.129ms**；这是 loopback 小样本，不是压力条件下的
P50/P95/P99。心跳、坏输入与断连测试是边界正确性证据；本轮未测真实慢网络长期驻留、
长文本饱和吞吐、断连风暴、TLS 握手容量或满负载取消延迟分布。
Memory 隔离 probe 本次未开启；不能从零 transport 错误推导多租户语义正确率。

测试 XML 保存于 `build/reports/sse_*final*.xml`。最终回归覆盖 LLM 67、流式传输 9、HTTP 38、
Net 37、配置 40、Persona 62、Gateway 86 通过；Gateway 另外两个默认 localhost Redis 用例跳过。
压测解析器 3 项测试校验失败样本分类。独立 SDK consumer 验证新头文件、decoder 工厂与链接依赖。

真实 Provider：`deepseek-chat`，短合成输入，3 个文本 delta，正文 6 bytes，
TTFT 807ms、总耗时 877ms、usage 10/3/13、finish_reason=stop。
CA 验证开启；回复正文、密钥和私有 prompt 不进入报告。一次成功不证明工具方言或持续容量。

## 收尾与后续验收

本次启动的 Gateway/Mock 已结束；未发现新增 crash dump。采样结束先保存 Provider metrics，
再停止进程；该操作不替代 graceful Shutdown 正确性测试。用户 WSL Redis 保持运行。
Windows Redis 测试实例按用户要求关闭，本次有效压测全部连接 Linux Redis。

后续验收优先项：在一致配置下做普通响应/SSE A/B；扩展长文本与慢读输入；
采集压力条件的取消收口分布；对 Memory Admission/commit 独立埋点；
按实际 Provider 工具方言验收两轮输出。不存在跨重启重放、透明续写或 Responses 能力承诺。
