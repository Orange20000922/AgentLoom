#pragma once

// 兼容入口：旧消费者仍可从本头文件取得所有公共 DTO 与接口。
#include "llm_client.h"
#include "llm_protocol.h"
#include "../net/http_client/http_client.h"
#include "../net/http_client/retry_policy.h"

namespace agent::llm {

struct OpenAiLlmClientOptions {
    /// Base URL for the OpenAI-compatible API (e.g., "https://api.deepseek.com/v1").
    std::string base_url;
    /// API key for authentication.
    std::string api_key;
    /// Default model name if not specified in the request.
    std::string default_model = "deepseek-chat";
    /// Request timeout in milliseconds.
    int timeout_ms = 30000;
    /// Retry policy for transient failures.
    net::RetryPolicy retry_policy;
    /// Local OpenAI-compatible endpoints normally do not require a bearer key.
    bool require_api_key = true;
    CompletionResponseValidationOptions response_validation;
    // 默认 ChatCompletionsProtocol；同步/异步可以共享同一不可变协议或分别注入。
    std::shared_ptr<const ILlmProtocol> protocol;
    LlmStreamLimits stream_limits;
};

/// OpenAI-compatible LLM client.
///
/// 默认使用 ChatCompletionsProtocol；通过 options.protocol 注入其他线协议。
/// 本类只管理 HTTP envelope、状态映射与重试，不解释消息 JSON。
///
/// Thread-safe: the underlying `IHttpClient` is stateless and can be called
/// concurrently from multiple threads.
class OpenAiLlmClient : public ILlmClient {
public:
    /// 创建 OpenAI-compatible 客户端。
    /// @param options endpoint、认证、超时和重试配置。
    /// @param http_client 借用的 HTTP 客户端，生命周期必须长于返回对象。
    static core::Result<std::unique_ptr<OpenAiLlmClient>> Create(
        OpenAiLlmClientOptions options,
        net::IHttpClient& http_client);

    ~OpenAiLlmClient() override;

    core::Result<ChatCompletionResponse> Complete(
        const ChatCompletionRequest& req) override;

private:
    OpenAiLlmClient(OpenAiLlmClientOptions options, net::IHttpClient& http_client);

    core::Result<ChatCompletionResponse> ExecuteWithRetry(
        const ChatCompletionRequest& req);

    OpenAiLlmClientOptions options_;
    net::IHttpClient& http_client_;
};

/// OpenAI-compatible 真异步客户端；HTTP 等待和 retry backoff 均不占用业务线程池 worker。
class OpenAiAsyncLlmClient final : public IAsyncLlmClient, public IAsyncStreamingLlmClient {
public:
    struct Impl;

    static core::Result<std::unique_ptr<OpenAiAsyncLlmClient>> Create(
        OpenAiLlmClientOptions options,
        net::IAsyncHttpClient& http_client);
    ~OpenAiAsyncLlmClient() override;

    core::Result<std::shared_ptr<IAsyncLlmOperation>> CompleteAsync(
        ChatCompletionRequest request,
        Callback callback) override;
    core::Result<std::shared_ptr<IAsyncLlmOperation>> CompleteStreamingAsync(
        ChatCompletionRequest request, LlmEventSink sink, Callback callback) override;

    /// 幂等关闭：取消在途 completion，等待 callback 返回及捕获释放，再收口 retry runtime。
    /// 只能从 callback 外部的关闭线程调用；普通 Cancel 不执行系统生命周期等待。
    void Shutdown() noexcept;

private:
    explicit OpenAiAsyncLlmClient(std::shared_ptr<Impl> impl);
    core::Result<std::shared_ptr<IAsyncLlmOperation>> SubmitCompletion(
        ChatCompletionRequest request, LlmEventSink sink, Callback callback);
    std::shared_ptr<Impl> impl_;
};


}
