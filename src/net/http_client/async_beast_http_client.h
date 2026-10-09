#pragma once

#include "http_client.h"
#include "../tls/tls_context.h"

#include <chrono>
#include <cstddef>
#include <memory>

namespace agent::net {

struct AsyncBeastHttpClientOptions {
    std::shared_ptr<TlsContext> tls_context;
    /// 仅驱动 DNS、socket、TLS 与 HTTP handler，不执行 Persona 业务任务。
    std::size_t io_thread_count = 1;
    /// 关闭时每次请求都建立新连接；开启时只缓存已完整读取的空闲连接。
    bool enable_keep_alive = true;
    /// 全部 origin 合计的空闲连接缓存上限，不限制正在执行的请求数。
    std::size_t max_idle_connections = 64;
    /// 单个 scheme/host/port 分桶的空闲连接缓存上限。
    std::size_t max_idle_connections_per_origin = 64;
    /// 空闲连接超过该时间未被借出即关闭。
    std::chrono::milliseconds idle_connection_timeout{30000};
};

/// 共享 io_context 的真正异步 HTTP/HTTPS 客户端；HTTP/1.1 连接按 origin 安全复用。
class AsyncBeastHttpClient final : public IAsyncHttpClient, public IAsyncStreamingHttpClient {
public:
    struct Impl;

    static core::Result<std::unique_ptr<AsyncBeastHttpClient>> Create(
        AsyncBeastHttpClientOptions options);
    ~AsyncBeastHttpClient() override;

    core::Result<std::shared_ptr<IAsyncHttpOperation>> ExecuteAsync(
        HttpClientRequest request,
        Callback callback) override;
    core::Result<std::shared_ptr<IAsyncHttpOperation>> ExecuteStreamingAsync(
        HttpClientRequest request, HttpStreamOptions options, HttpStreamCallbacks callbacks) override;

    /// 幂等关闭：拒绝新请求、取消在途请求、等待回调收口并回收 io_context 线程。
    void Shutdown() noexcept;

private:
    explicit AsyncBeastHttpClient(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> impl_;
};

}
