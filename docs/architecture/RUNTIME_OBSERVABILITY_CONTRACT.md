# Runtime 用量与 WS 连接日志契约

## Persona 用量

OpenAiLlmClient 按 OpenAI-compatible 响应解析并校验 usage；PersonaRuntime 的 ChatResponse 向公共交互接口返回 prompt_tokens、completion_tokens、total_tokens。
三个字段使用 64 位计数，表示本轮成功对话 LLM 调用累计值，工具调用首轮和 follow-up 均计入。
答案缓存直返没有调用对话 LLM，用量为零；L0 记忆命中仍然生成回复，不能归零。
未报告 usage 时沿用客户端零值，不估算；零值不证明 Provider 已报告零用量。
字段不包含情绪分析模型调用。失败仍返回 core::Status，本接口不能代替覆盖失败调用的完整计费事件。
消费者可用这些字段实现 usage 计量，但无需在每轮运行日志重复记录。

## WS 连接上下文

WebSocketSessionHandle 新增只读 connection() 与 connection_id()，accept 回调在握手成功时即可取得连接标识。
上下文复用 ConnectionLease，引用仅在句柄存活且连接未关闭时有效。跨回调保存时复制所需字段，不转移租约所有权。

## 网络层日志

HttpServerOptions::logger 允许静态链接消费者注入已有 LoggerAdapter；默认查找 net 模块，未注册时使用默认 logger。
WebSocketSession 直接写入消费者 sink，不要求独立日志进程，也不要求下游维护第二份连接统计。

| 事件 | 字段 |
| --- | --- |
| ws.opened | 握手成功；connection_id、remote_address |
| ws.closed | 至多一条；connection_id、remote_address、accepted、duration_ms、messages、bytes、固定 reason、整数 status_code |

持续时间从 WS 握手成功起算，握手失败时从 TCP 建连时间起算。
消息数按完整逻辑消息结束累计；字节数为成功读取的解压后 payload，错误回调同时返回的部分字节不计入。
正常对端关闭、空闲超时和服务停止为 info，协议及背压异常为 warn，内部故障为 error。
不记录 payload、target/query、Authorization、Cookie 或客户端关闭原因文本。

## 生命周期与 SDK 接入

保持现有 IO、租约、accept/close 回调和 HttpServer Stop 顺序。正常关闭在释放租约前记录，原子关闭标志防止重复。
Stop 停止 IO 后，剩余 Session 可能没有读回调；最终释放时补 server_shutdown 日志，析构不补用户 close 回调。
因此停止摘要可能在 HttpServer 销毁时写出，而非 Stop 返回前。日志输出异常不改变网络处理结果。

ChatResponse、HttpServerOptions、WebSocketSessionOptions 布局及 WebSocketSessionHandle 虚接口变化要求 SDK 与消费者一起重编译。
消费者自行实现的 WebSocketSessionHandle 需实现 connection()。
下游注入自己的模块 logger 后，移除重复的 ws.opened/ws.closed 格式化、首消息建连统计和连接表；业务拒绝、轮次结果、回包失败日志仍由业务适配层负责。

## 验证范围

GTest 覆盖同步/异步 Persona 用量、工具 follow-up 累计、缓存零用量、公共 PersonaInteraction 边界，以及真实 socket 的 accept ID、无消息连接、关闭级别、消息计数与日志脱敏。
本地网络并发验证不作为生产容量或日志开销的保证。
