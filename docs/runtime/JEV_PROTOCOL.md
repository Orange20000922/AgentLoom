# Jev System One 协议与开发端点

## 类型化调用

`JevProtocol` 实现现有 LLM 协议接口，配合 `OpenAiAsyncLlmClient` 使用。调用方通过 `ChatCompletionRequest.jev` 提交一份共享的 `state_json` 和按名称组织的问题。state 可以是 JSON 字符串、数组或对象。首版支持非流式 choice，每题提供 1 至 255 个具名选项，可附带 instructions。请求级模型为空时使用客户端配置的默认模型。

成功响应从 `ChatCompletionResponse.jev_answers` 按题名读取，每题含 choice、confidence 和各选项的 probabilities。解码器检查题目和选项是否齐全、选择是否在声明集合内、概率是否在零到一之间，并对概率总和允许 0.01 的舍入误差。不完整或非法结果返回协议错误。超时、取消及 HTTP 错误沿用已有异步调用路径。

## 示例服务器

在配置中显式启用 `llm.enable_dev_routes`，聊天连接使用 `llm.openai`；Jev 转发使用 `llm.jev.endpoint` 指定完整上游 URL，从 `api_key_env` 或 `api_key_file` 取得服务端凭据。可配置请求超时与 CA。已有平铺的聊天配置仍可读取。

| 端点 | 行为 |
| --- | --- |
| `POST /v1/chat/completions` | 调用方先创建 SDK 会话，携带 `X-Session-Id`，以末条 user 文本执行完整 Persona Turn。可通过 `X-Persona-Id` 核对人格，通过 `X-Turn-Mode` 指定本轮模式。支持普通 JSON 和 SSE。 |
| `POST /v1/systemone` | 将客户端原始请求正文发送到配置的上游，保留上游 HTTP 状态、响应体、Content-Type 及 Retry-After。此路由绕过类型化 choice 解码，便于测试服务商协议。 |

开发端点沿用服务器身份校验。类型化编解码和本地上游转发已有自动化覆盖，其中限流响应可用本地代理冒烟测试复现。特定代理商的字段兼容性仍需用其 URL 和凭据联调。调用示例见[项目 README](../../README.md)。
