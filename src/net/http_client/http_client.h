#pragma once

#include "result.h"

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agent::net {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpClientRequest {
    std::string method = "POST";
    /// 完整 URL，包含 scheme、host、可选 port 与 path。
    /// 例如："https://api.deepseek.com/v1/chat/completions"、
    ///       "http://127.0.0.1:8080/echo"。
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    std::int32_t timeout_ms = 60000;
};

struct HttpClientResponse {
    int status = 0;
    std::vector<HttpHeader> headers;
    std::string body;
};

/// 出站 HTTP/HTTPS 客户端接口。
///
/// 每次调用执行一个请求；具体实现启用 HTTP keep-alive 后可以复用空闲连接。
/// 不自动跟随重定向，调用方负责重试、退避和合理的 `timeout_ms`。
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /// 执行单个请求。DNS、连接、TLS 握手、超时、响应格式错误等传输失败返回非 ok Status；
    /// 任意 HTTP 响应（包括 4xx/5xx）返回 Ok，具体状态写入 `status`。
    virtual core::Result<HttpClientResponse> Execute(const HttpClientRequest& req) = 0;
};

class IAsyncHttpOperation {
public:
    virtual ~IAsyncHttpOperation() = default;
    /// 幂等取消；最终回调仍恰好调用一次并返回 Cancelled。
    virtual void Cancel() noexcept = 0;
};

class IAsyncHttpClient {
public:
    using Callback = std::function<void(core::Result<HttpClientResponse>)>;

    virtual ~IAsyncHttpClient() = default;
    /// 发起真正的异步出站请求；返回句柄只用于主动取消，丢弃句柄不会取消请求。
    virtual core::Result<std::shared_ptr<IAsyncHttpOperation>> ExecuteAsync(
        HttpClientRequest request,
        Callback callback) = 0;
};

struct HttpStreamOptions {
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    std::size_t read_buffer_bytes = 16 * 1024;
    std::chrono::milliseconds idle_timeout{30000};
};

struct HttpStreamCallbacks {
    // 响应头先于任何 body；失败即关闭传输。视图只在 callback 期间有效。
    std::function<core::Status(const HttpClientResponse&)> on_headers;
    std::function<core::Status(std::string_view)> on_body;
    std::function<void(core::Status)> on_complete;
};

class IAsyncStreamingHttpClient {
public:
    virtual ~IAsyncStreamingHttpClient() = default;
    virtual core::Result<std::shared_ptr<IAsyncHttpOperation>> ExecuteStreamingAsync(
        HttpClientRequest request, HttpStreamOptions options, HttpStreamCallbacks callbacks) = 0;
};

}
