# Chat Completions 与 Gateway HTTP SSE

## 实现范围

当前主路径是真实的上游 Chat Completions SSE → HTTP body 增量读取 → SSE framing →
逐请求 decoder → Persona 事件 sink → Gateway chunked SSE。普通 Complete/CompleteAsync
继续使用完整响应接口；不支持 streaming 的 Provider 必须明确拒绝请求。

新的 HTTP 增量读取、Gateway SSE 写入、对端断连探针与配额 Mock 流式路径使用
Boost.Asio `awaitable` / `co_spawn`。复用已有 io_context、TLS、HTTP 空闲连接池、
ConnectionLease、core 内存池与 BackpressureQueue。MSVC HTTP 客户端目标私有启用
`/bigobj`，以容纳 Beast HTTP/TLS 协程模板产生的 COMDAT 节。

## 客户端能力接口

- `IAsyncStreamingHttpClient::ExecuteStreamingAsync` 先发布响应头，再发布 body 片段，最后一次完成。
- `IAsyncStreamingLlmClient::CompleteStreamingAsync` 分离事件 sink 与最终生成结果；操作句柄继续支持 Cancel。
- `ILlmProtocol::CreateStreamDecoder` 每次生成独立创建解析器，共享协议对象没有请求级可变状态。
- `ChatRequest.stream/event_sink` 经 `IPersonaInteraction::SubmitTurn` 进入原有异步 Turn 主路径。

HTTP status 和 `Content-Type: text/event-stream` 在解码前校验；任意 HTTP body 分块都不等同
于 SSE 事件、JSON 或 UTF-8 字符边界。支持 BOM、LF/CRLF/CR、多行 data、注释与未知 SSE 字段。
EOF、HTTP 最后一个 chunk 和 `[DONE]` 的含义不同：HTTP 传输完整、finish_reason 和 `[DONE]`
都存在且聚合结果校验通过，才能得到成功生成。坏 HTTP chunk、坏 JSON、非法 UTF-8、
重复终态、终态后业务 data、截断事件及缺失终态均失败。

单次流式请求默认不自动重试，包括已发布首片段后的失败。完整读完且双方允许 keep-alive 的
出站连接可归还原空闲池；失败、取消或读取边界不明确的连接关闭。HTTP idle 超时观察收到的
body 活动，包括 Provider 心跳；总 deadline 不被心跳延长。

普通与流式聚合共享 `ChatCompletionsProtocol::DecodeResponse` 的最终校验入口。
usage 作为覆盖快照，不逐事件求和。工具参数只组装，完整 JSON 对象和最终响应校验通过后
才交给既有工具 coordinator 的 schema 校验与执行。

## Gateway 请求与事件

启动流式 Turn：

```http
POST /api/chat/message
Content-Type: application/json
X-Trace-Id: unique-request-id

{"sessionId":"session-id","message":"你好","stream":true}
```

响应使用 HTTP/1.1 chunked 和 `text/event-stream; charset=utf-8`，发送
`Cache-Control: no-cache, no-transform` 与 `X-Accel-Buffering: no`。
调用方应保留返回事件里的 `requestId`；重连必须使用它，不能把原 POST 再提交一次。

| SSE event | 含义 |
| --- | --- |
| TextDelta | 正文增量；reasoning_content 不转为可见文本 |
| ToolCallProgress | 工具声明与参数增量，尚不可执行 |
| ToolExecutionState | 工具批次开始、完成或失败 |
| UsageUpdated | 当前生成的 Provider usage 快照 |
| OutputItemCompleted | 当前输出项经最终校验后结束 |
| GenerationCompleted | 某次 Provider 生成结束，尚未确认 Turn 提交 |
| TurnCompleted | 工具 follow-up、AI Emotion、Memory Admission 策略和 Session commit 完成 |
| TurnFailed | 唯一失败/取消终态，`committed=false` |
| ping | 连接心跳，不属于业务 sequence |

业务事件包含 requestId、turnId、generationId、itemId 和整轮单调 sequence；
`id` 为 `requestId:sequence`。工具首轮与 follow-up 的 generationId 不同。
Turn 终态包含 requestId、turnId、sequence、committed 和原有结果/错误 envelope。
TurnCompleted 不再重复发送正文，消费者使用已经接收的 TextDelta 组装回复。

工具候选存在时，首轮文本暂存到生成最终校验完成；确认发生工具调用后丢弃过渡文本。
无工具调用时释放暂存文本，有工具调用时只展示 follow-up 的文本。
因此具备工具候选的请求不能保证首轮文本在生成完成前可见。
先支持已有两轮工具闭环；follow-up 继续请求工具或缺少 coordinator 时失败，避免假成功。

增量不写 L0/recent history、不增加 turn count。原有 Session deferred lane 保持到 Turn 终态。
Memory Admission 沿用已选择的异步策略：失败记录日志后继续提交；同步路径原有失败策略保留。
可见文本之后 AI Emotion 等必需后处理失败，发送 TurnFailed 并说明尚未提交。
流式请求绕过答案直返缓存，保证来源是上游真实增量。

## 心跳、idle 与重连

SSE 是单向协议。默认发 `ping` 心跳事件；可在请求中使用 `requirePong=true`，
或在部署配置中设置 require_pong。需要回复时，在独立连接调用：

```http
POST /api/chat/stream/pong
Content-Type: application/json

{"connectionId":123}
```

pong 按已认证用户校验活跃连接。ping 发送不刷新 pong idle；只有有效 pong 刷新。
默认不要求 pong 时业务发布刷新生成 idle。所有流均有总持续时间和写超时。

断连默认取消原 Turn 与上游 operation，释放 lane 和配额；已经执行的工具副作用不撤销。
短期重连只重放保留事件和原 Turn 的终态：

```http
GET /api/chat/stream/unique-request-id
Last-Event-ID: unique-request-id:3
```

重连按用户校验，不能接管其他用户的流；请求仍活跃且已连接时拒绝第二个消费者。
允许断开后读取已经发布的后缀与取消终态，不重新运行 completion/工具。
游标不存在或已淘汰返回明确错误；缺失游标且重放前缀已淘汰也失败，不静默返回不完整文本。
终态缓存有 TTL/容量上限；容量紧张时优先淘汰最旧终态，不淘汰在途 Turn。
重放缓存是进程内短期数据，不提供跨重启恢复；多实例部署需保持路由 affinity。

浏览器初始 POST 应使用 fetch 流式读取；原生 EventSource 不能直接发送该 POST。
`retry` 字段只用于恢复 GET 的建议延迟，不能据此重发初始 POST。

## 容量与配置

| 默认上限/超时 | 值 |
| --- | --- |
| 上游单 SSE 事件 | 256 KiB |
| 单次生成全部输入字节（含注释） | 16 MiB |
| 每工具 arguments / 工具数量 | 1 MiB / 64 |
| 工具首轮暂存文本 | 4096 个 delta / 1 MiB |
| Gateway 单事件 | 256 KiB |
| Gateway 待写队列 | 256 个事件 / 1 MiB |
| 短期重放 | 64 个 Turn；每 Turn 128 个事件 / 512 KiB |
| 重放保留时间 | 30 秒；容量不足可能提前淘汰终态 |
| ping / idle / 总持续时间 / 单次写超时 | 15 秒 / 60 秒 / 300 秒 / 30 秒 |

SDK 可以使用 `OpenAiLlmClientOptions::stream_limits`、`HttpStreamOptions` 和
`PersonaGatewayServerOptions::streaming` 设置边界。生产配置 `persona_gateway.streaming` 支持
max_pending_events/max_pending_bytes/max_event_bytes、max_replay_turns/max_replay_events/
max_replay_bytes/replay_retention_ms、heartbeat_interval_ms/idle_timeout_ms/max_duration_ms/
write_timeout_ms/reconnect_delay_ms/require_pong。

队列满返回 ResourceExhausted，关闭该 SSE 连接并取消在途生成，终态可在短期重放中读取。
失败前未接纳的事件不进入重放缓存；文本不能丢弃旧消息继续生成。
TurnCompleted 提供发布前的 streamQueue 快照；网络 `sse.closed` 日志记录包含最终事件的准确峰值。

## 验证与限制

decoder、真实 socket HTTP 边界、Gateway 全链路及工具两轮测试分别位于
`tests/llm/llm_stream_decoder_test.cpp`、`llm_stream_transport_test.cpp`、
`tests/service/persona_gateway_streaming_test.cpp`、`persona_gateway_tool_calling_test.cpp`。
keep-alive/idle 复用测试在 `async_beast_http_client_test.cpp`，重放/owner/游标边界在
`chat_sse_registry_test.cpp`。

原 `gateway_concurrency_benchmark.py --stream` 与 C++ 配额 Mock 支持流式压测。
`llm_smoke_test <config> <prompt> <max_tokens> --stream` 验证真实 Provider TLS/SSE，不打印正文。
配置示例见 `tools/persona_gateway_sse_benchmark.example.json`；环境专属配置使用已忽略的 local 副本。

验收数据见 [HTTP SSE 验证与压测报告](../performance/HTTP_SSE_STREAMING_REPORT_2026_10.md)。
暂未实现 Responses、跨重启恢复、透明续写和多于已有两轮的工具循环。
