#include "async_beast_http_client.h"
#include "url_parser.h"

#include "logger_adapter.h"
#include "../shared_buffer.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <deque>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agent::net {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace {

class AsyncHttpOperationBase;

struct OriginKey {
    UrlScheme scheme = UrlScheme::Https;
    std::string host;
    std::uint16_t port = 0;

    bool operator==(const OriginKey&) const = default;
};

struct OriginKeyHash {
    std::size_t operator()(const OriginKey& key) const noexcept {
        auto seed = std::hash<std::string>{}(key.host);
        seed ^= std::hash<std::uint16_t>{}(key.port) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<int>{}(static_cast<int>(key.scheme)) + 0x9e3779b9U +
                (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

OriginKey MakeOriginKey(const ParsedUrl& url) {
    auto host = url.host;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return {url.scheme, std::move(host), url.port};
}

class PooledConnection {
public:
    PooledConnection(asio::io_context& context, OriginKey origin)
        : origin_(std::move(origin)), idle_timer_(context) {}
    virtual ~PooledConnection() = default;

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

    const OriginKey& origin() const noexcept { return origin_; }
    asio::steady_timer& idle_timer() noexcept { return idle_timer_; }
    virtual bool IsOpen() const noexcept = 0;
    virtual void Close() noexcept = 0;

private:
    OriginKey origin_;
    asio::steady_timer idle_timer_;
};

class PlainPooledConnection final : public PooledConnection {
public:
    PlainPooledConnection(asio::io_context& context, OriginKey origin)
        : PooledConnection(context, std::move(origin)), stream(context) {}

    bool IsOpen() const noexcept override { return stream.socket().is_open(); }

    void Close() noexcept override {
        beast::error_code ignored;
        idle_timer().cancel(ignored);
        stream.socket().cancel(ignored);
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        stream.socket().close(ignored);
    }

    beast::tcp_stream stream;
};

class TlsPooledConnection final : public PooledConnection {
public:
    TlsPooledConnection(asio::io_context& context,
                        OriginKey origin,
                        ssl::context& tls_context)
        : PooledConnection(context, std::move(origin)), stream(context, tls_context) {}

    bool IsOpen() const noexcept override {
        return beast::get_lowest_layer(stream).socket().is_open();
    }

    void Close() noexcept override {
        beast::error_code ignored;
        idle_timer().cancel(ignored);
        auto& lowest = beast::get_lowest_layer(stream);
        lowest.socket().cancel(ignored);
        lowest.socket().shutdown(tcp::socket::shutdown_both, ignored);
        lowest.socket().close(ignored);
    }

    beast::ssl_stream<beast::tcp_stream> stream;
};

http::request<http::string_body> BuildRequest(const ParsedUrl& url,
                                               const HttpClientRequest& request) {
    http::request<http::string_body> result;
    const auto verb = http::string_to_verb(request.method);
    if (verb == http::verb::unknown) {
        result.method_string(request.method);
    } else {
        result.method(verb);
    }
    result.target(url.target);
    result.version(11);
    result.keep_alive(true);
    result.set(http::field::host, url.port == 80 || url.port == 443
        ? url.host
        : url.host + ":" + std::to_string(url.port));
    result.set(http::field::user_agent, "agent-http-client/1.0");

    bool has_content_type = false;
    for (const auto& header : request.headers) {
        result.set(header.name, header.value);
        if (header.name.size() == 12) {
            constexpr std::string_view expected = "content-type";
            has_content_type = true;
            for (std::size_t i = 0; i < expected.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(header.name[i])) != expected[i]) {
                    has_content_type = false;
                    break;
                }
            }
        }
    }
    if (!request.body.empty() && !has_content_type) {
        result.set(http::field::content_type, "application/octet-stream");
    }
    result.body() = std::move(request.body);
    result.prepare_payload();
    return result;
}

HttpClientResponse ConvertResponse(http::response<http::string_body>& response) {
    HttpClientResponse result;
    result.status = static_cast<int>(response.result_int());
    for (const auto& field : response) {
        result.headers.push_back(HttpHeader{
            std::string(field.name_string()),
            std::string(field.value()),
        });
    }
    result.body = std::move(response.body());
    return result;
}

core::Status TransportError(std::string_view stage, const beast::error_code& error) {
    if (error == http::error::body_limit || error == http::error::header_limit)
        return core::Status::Error(core::ErrorCode::ResourceExhausted, "HTTP response exceeds limit");
    if (error == http::error::partial_message || error == http::error::unexpected_body ||
        error == http::error::bad_chunk || error == http::error::bad_chunk_extension) {
        return core::Status::Error(
            core::ErrorCode::DataLoss,
            std::string(stage) + ": incomplete or malformed HTTP message: " + error.message());
    }
    return core::Status::Error(
        core::ErrorCode::Unavailable,
        std::string(stage) + ": " + error.message());
}

}

struct AsyncBeastHttpClient::Impl final
    : public std::enable_shared_from_this<AsyncBeastHttpClient::Impl> {
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    explicit Impl(AsyncBeastHttpClientOptions client_options)
        : options(std::move(client_options)),
          guard(asio::make_work_guard(io_context)),
          logger(core::LoggerAdapter::ForModule("async-http-client")) {}

    core::Status StartThreads() {
        try {
            threads.reserve(options.io_thread_count);
            for (std::size_t i = 0; i < options.io_thread_count; ++i) {
                auto self = shared_from_this();
                threads.emplace_back([self] {
                    try {
                        self->io_context.run();
                    } catch (const std::exception& error) {
                        self->logger.error("异步 HTTP io_context 线程异常退出: {}", error.what());
                    } catch (...) {
                        self->logger.error("异步 HTTP io_context 线程发生未知异常");
                    }
                });
            }
            return core::Status::Ok();
        } catch (const std::exception& error) {
            guard.reset();
            io_context.stop();
            JoinThreads();
            return core::Status::Error(
                core::ErrorCode::InternalError,
                std::string("failed to start async HTTP IO threads: ") + error.what());
        }
    }

    core::Status Register(const std::shared_ptr<AsyncHttpOperationBase>& operation) {
        std::lock_guard lock(mutex);
        if (stopping) {
            return core::Status::Error(
                core::ErrorCode::Cancelled,
                "async HTTP client is shutting down");
        }
        operations.emplace(operation.get(), operation);
        return core::Status::Ok();
    }

    void Unregister(AsyncHttpOperationBase* operation) noexcept {
        std::lock_guard lock(mutex);
        operations.erase(operation);
    }

    template <typename Connection>
    std::shared_ptr<Connection> AcquireIdle(const OriginKey& origin) {
        // 仅在 io_context handler 中取得空闲连接；避免跨线程操作 Beast stream/timer。
        std::vector<std::shared_ptr<PooledConnection>> stale;
        std::shared_ptr<PooledConnection> selected;
        {
            std::lock_guard lock(mutex);
            auto found = idle_connections.find(origin);
            if (found != idle_connections.end()) {
                auto& connections = found->second;
                while (!connections.empty()) {
                    auto candidate = std::move(connections.back());
                    connections.pop_back();
                    --idle_connection_count;
                    if (candidate->IsOpen()) {
                        selected = std::move(candidate);
                        break;
                    }
                    stale.push_back(std::move(candidate));
                }
                if (connections.empty()) {
                    idle_connections.erase(found);
                }
            }
        }
        for (const auto& connection : stale) {
            connection->Close();
        }
        if (selected) {
            beast::error_code ignored;
            selected->idle_timer().cancel(ignored);
        }
        return std::dynamic_pointer_cast<Connection>(std::move(selected));
    }

    void ReleaseIdle(std::shared_ptr<PooledConnection> connection) {
        // 完整读取且双方允许 keep-alive 的连接才可回池；其余情况由调用方关闭。
        bool keep = false;
        {
            std::lock_guard lock(mutex);
            if (!stopping && options.enable_keep_alive && connection->IsOpen()) {
                auto& connections = idle_connections[connection->origin()];
                if (idle_connection_count < options.max_idle_connections &&
                    connections.size() < options.max_idle_connections_per_origin) {
                    connections.push_back(connection);
                    ++idle_connection_count;
                    keep = true;
                }
            }
        }
        if (!keep) {
            connection->Close();
            return;
        }

        connection->idle_timer().expires_after(options.idle_connection_timeout);
        auto weak_owner = weak_from_this();
        std::weak_ptr<PooledConnection> weak_connection = connection;
        connection->idle_timer().async_wait(
            [weak_owner, weak_connection](const beast::error_code& error) {
                if (error == asio::error::operation_aborted) {
                    return;
                }
                if (auto owner = weak_owner.lock()) {
                    if (auto idle = weak_connection.lock()) {
                        owner->ExpireIdle(std::move(idle));
                    }
                }
            });
    }

    void ExpireIdle(std::shared_ptr<PooledConnection> connection) noexcept {
        // timer 触发后按连接身份从分桶中移除，防止已被再次借出的连接被误关闭。
        bool removed = false;
        {
            std::lock_guard lock(mutex);
            auto found = idle_connections.find(connection->origin());
            if (found != idle_connections.end()) {
                auto& connections = found->second;
                const auto item = std::find(connections.begin(), connections.end(), connection);
                if (item != connections.end()) {
                    connections.erase(item);
                    --idle_connection_count;
                    removed = true;
                }
                if (connections.empty()) {
                    idle_connections.erase(found);
                }
            }
        }
        if (removed) {
            connection->Close();
        }
    }

    void Shutdown() noexcept;

    void JoinThreads() noexcept {
        const auto current = std::this_thread::get_id();
        for (auto& thread : threads) {
            if (!thread.joinable()) {
                continue;
            }
            if (thread.get_id() == current) {
                // 回调内触发关闭时不能等待当前线程自身；线程闭包持有 Impl，
                // 因而 detach 后资源仍会在 run() 返回时按 RAII 释放。
                thread.detach();
            } else {
                thread.join();
            }
        }
        threads.clear();
    }

    AsyncBeastHttpClientOptions options;
    asio::io_context io_context;
    WorkGuard guard;
    core::LoggerAdapter logger;
    core::BucketMemoryPool stream_memory_pool;

    std::mutex mutex;
    bool stopping = false;
    std::unordered_map<AsyncHttpOperationBase*, std::shared_ptr<AsyncHttpOperationBase>> operations;
    std::unordered_map<OriginKey,
                       std::deque<std::shared_ptr<PooledConnection>>,
                       OriginKeyHash> idle_connections;
    std::size_t idle_connection_count = 0;
    std::vector<std::thread> threads;
};

namespace {

class AsyncHttpOperationBase : public IAsyncHttpOperation,
                               public std::enable_shared_from_this<AsyncHttpOperationBase> {
public:
    AsyncHttpOperationBase(std::shared_ptr<AsyncBeastHttpClient::Impl> owner,
                           ParsedUrl url,
                           HttpClientRequest request,
                           IAsyncHttpClient::Callback callback)
        : owner_(std::move(owner)),
          url_(std::move(url)),
          request_(BuildRequest(url_, request)),
          timeout_(std::chrono::milliseconds(request.timeout_ms)),
          strand_(asio::make_strand(owner_->io_context)),
          resolver_(strand_),
          deadline_(strand_),
          callback_(std::move(callback)) {}

    void Start() noexcept {
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            if (self->completed_.load(std::memory_order_acquire)) {
                return;
            }
            // 句柄可能在 Start handler 被 IO 线程消费前取消。此时不得再启动
            // DNS/connect，否则取消与新建底层 operation 交错会拖延关闭收口。
            if (self->cancel_requested_.load(std::memory_order_acquire)) {
                self->Finish(core::Status::Error(
                    core::ErrorCode::Cancelled,
                    "async HTTP request cancelled before start"));
                return;
            }
            try {
                self->StartOnIoThread();
            } catch (const std::exception& error) {
                self->Finish(core::Status::Error(
                    core::ErrorCode::InternalError,
                    std::string("async HTTP start exception: ") + error.what()));
            } catch (...) {
                self->Finish(core::Status::Error(
                    core::ErrorCode::InternalError,
                    "async HTTP start exception: unknown error"));
            }
        });
    }

    void Cancel() noexcept override {
        if (cancel_requested_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            if (self->completed_.load(std::memory_order_acquire)) {
                return;
            }
            self->CloseTransport();
            self->Finish(core::Status::Error(
                core::ErrorCode::Cancelled,
                "async HTTP request cancelled"));
        });
    }

protected:
    virtual void StartOnIoThread() = 0;
    virtual void CloseTransport() noexcept = 0;

    AsyncBeastHttpClient::Impl& owner() noexcept { return *owner_; }
    auto executor() const { return strand_; }
    bool completed() const noexcept { return completed_.load(std::memory_order_acquire); }

    void StartDeadline() {
        deadline_.expires_after(timeout_);
        auto self = shared_from_this();
        deadline_.async_wait([self](const beast::error_code& error) {
            if (error == asio::error::operation_aborted ||
                self->completed_.load(std::memory_order_acquire)) {
                return;
            }
            self->timed_out_.store(true, std::memory_order_release);
            self->CloseTransport();
            self->Finish(core::Status::Error(
                core::ErrorCode::Timeout,
                "async HTTP request timed out"));
        });
    }

    bool HandleTransportError(std::string_view stage, const beast::error_code& error) {
        if (!error) {
            return false;
        }
        if (completed_.load(std::memory_order_acquire)) {
            return true;
        }
        if (timed_out_.load(std::memory_order_acquire)) {
            Finish(core::Status::Error(
                core::ErrorCode::Timeout,
                std::string(stage) + ": timed out"));
        } else if (cancel_requested_.load(std::memory_order_acquire) ||
                   error == asio::error::operation_aborted) {
            Finish(core::Status::Error(
                core::ErrorCode::Cancelled,
                std::string(stage) + ": cancelled"));
        } else {
            Finish(TransportError(stage, error));
        }
        return true;
    }

    void Finish(core::Result<HttpClientResponse> result) noexcept {
        if (completed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        beast::error_code ignored;
        deadline_.cancel(ignored);
        resolver_.cancel();
        CloseTransport();

        auto owner = owner_;
        owner->Unregister(this);
        auto callback = std::move(callback_);
        try {
            callback(std::move(result));
        } catch (const std::exception& error) {
            owner->logger.error("异步 HTTP callback 抛出异常: {}", error.what());
        } catch (...) {
            owner->logger.error("异步 HTTP callback 抛出未知异常");
        }
    }

    const ParsedUrl& url() const noexcept { return url_; }
    tcp::resolver& resolver() noexcept { return resolver_; }
    http::request<http::string_body>& request() noexcept { return request_; }
    beast::flat_buffer& buffer() noexcept { return buffer_; }
    http::response<http::string_body>& response() noexcept { return response_; }

private:
    std::shared_ptr<AsyncBeastHttpClient::Impl> owner_;
    ParsedUrl url_;
    http::request<http::string_body> request_;
    std::chrono::milliseconds timeout_;
    asio::strand<asio::io_context::executor_type> strand_;
    tcp::resolver resolver_;
    asio::steady_timer deadline_;
    beast::flat_buffer buffer_;
    http::response<http::string_body> response_;
    IAsyncHttpClient::Callback callback_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> timed_out_{false};
    std::atomic<bool> completed_{false};
};

class PlainAsyncHttpOperation final : public AsyncHttpOperationBase {
public:
    PlainAsyncHttpOperation(std::shared_ptr<AsyncBeastHttpClient::Impl> owner,
                            ParsedUrl url,
                            HttpClientRequest request,
                            IAsyncHttpClient::Callback callback)
        : AsyncHttpOperationBase(owner, std::move(url), std::move(request), std::move(callback)) {}

private:
    void StartOnIoThread() override {
        // HTTP/1.1 一条连接同一时刻只服务一个请求；复用仅针对已归还的空闲连接。
        connection_ = owner().AcquireIdle<PlainPooledConnection>(MakeOriginKey(url()));
        if (!connection_) {
            connection_ = std::make_shared<PlainPooledConnection>(
                owner().io_context, MakeOriginKey(url()));
        } else {
            reused_ = true;
        }
        StartDeadline();
        if (reused_) {
            Write();
            return;
        }
        auto self = std::static_pointer_cast<PlainAsyncHttpOperation>(shared_from_this());
        resolver().async_resolve(
            url().host,
            std::to_string(url().port),
            [self](const beast::error_code& error, tcp::resolver::results_type endpoints) {
                if (self->HandleTransportError("DNS resolve", error)) {
                    return;
                }
                self->connection_->stream.async_connect(
                    endpoints,
                    [self](const beast::error_code& connect_error,
                           const tcp::resolver::results_type::endpoint_type&) {
                        if (self->HandleTransportError("TCP connect", connect_error)) {
                            return;
                        }
                        self->Write();
                    });
            });
    }

    void Write() {
        auto self = std::static_pointer_cast<PlainAsyncHttpOperation>(shared_from_this());
        http::async_write(connection_->stream, request(),
            [self](const beast::error_code& error, std::size_t) {
                if (self->HandleTransportError("HTTP write", error)) {
                    return;
                }
                self->Read();
            });
    }

    void Read() {
        auto self = std::static_pointer_cast<PlainAsyncHttpOperation>(shared_from_this());
        http::async_read(connection_->stream, buffer(), response(),
            [self](const beast::error_code& error, std::size_t) {
                if (self->HandleTransportError("HTTP read", error)) {
                    return;
                }
                auto result = ConvertResponse(self->response());
                // Beast buffer 有残留表示可能已读入后续字节，不能把该连接交给下一个请求。
                if (self->request().keep_alive() && self->response().keep_alive() &&
                    self->buffer().size() == 0) {
                    self->owner().ReleaseIdle(std::move(self->connection_));
                }
                self->Finish(std::move(result));
            });
    }

    void CloseTransport() noexcept override {
        if (connection_) {
            connection_->Close();
            connection_.reset();
        }
    }

    std::shared_ptr<PlainPooledConnection> connection_;
    bool reused_ = false;
};

class TlsAsyncHttpOperation final : public AsyncHttpOperationBase {
public:
    TlsAsyncHttpOperation(std::shared_ptr<AsyncBeastHttpClient::Impl> owner,
                          ParsedUrl url,
                          HttpClientRequest request,
                          IAsyncHttpClient::Callback callback,
                          TlsContext& tls_context)
        : AsyncHttpOperationBase(owner, std::move(url), std::move(request), std::move(callback)),
          tls_context_(tls_context) {}

private:
    void StartOnIoThread() override {
        // TLS 连接的复用跳过 DNS、TCP connect 与 TLS handshake，但仍保持单请求串行。
        connection_ = owner().AcquireIdle<TlsPooledConnection>(MakeOriginKey(url()));
        if (!connection_) {
            connection_ = std::make_shared<TlsPooledConnection>(
                owner().io_context, MakeOriginKey(url()), tls_context_.asio_context());
        } else {
            reused_ = true;
        }
        StartDeadline();
        if (reused_) {
            Write();
            return;
        }
        if (auto status = tls_context_.PrepareConnection(
                connection_->stream.native_handle(), url().host);
            !status.ok()) {
            Finish(std::move(status));
            return;
        }

        auto self = std::static_pointer_cast<TlsAsyncHttpOperation>(shared_from_this());
        resolver().async_resolve(
            url().host,
            std::to_string(url().port),
            [self](const beast::error_code& error, tcp::resolver::results_type endpoints) {
                if (self->HandleTransportError("DNS resolve", error)) {
                    return;
                }
                beast::get_lowest_layer(self->connection_->stream).async_connect(
                    endpoints,
                    [self](const beast::error_code& connect_error,
                           const tcp::resolver::results_type::endpoint_type&) {
                        if (self->HandleTransportError("TCP connect", connect_error)) {
                            return;
                        }
                        self->Handshake();
                    });
            });
    }

    void Handshake() {
        auto self = std::static_pointer_cast<TlsAsyncHttpOperation>(shared_from_this());
        connection_->stream.async_handshake(ssl::stream_base::client,
            [self](const beast::error_code& error) {
                if (self->HandleTransportError("TLS handshake", error)) {
                    return;
                }
                self->Write();
            });
    }

    void Write() {
        auto self = std::static_pointer_cast<TlsAsyncHttpOperation>(shared_from_this());
        http::async_write(connection_->stream, request(),
            [self](const beast::error_code& error, std::size_t) {
                if (self->HandleTransportError("HTTPS write", error)) {
                    return;
                }
                self->Read();
            });
    }

    void Read() {
        auto self = std::static_pointer_cast<TlsAsyncHttpOperation>(shared_from_this());
        http::async_read(connection_->stream, buffer(), response(),
            [self](const beast::error_code& error, std::size_t) {
                if (self->HandleTransportError("HTTPS read", error)) {
                    return;
                }
                auto result = ConvertResponse(self->response());
                // 同明文连接，只在响应边界明确且双方允许 keep-alive 时归还连接。
                if (self->request().keep_alive() && self->response().keep_alive() &&
                    self->buffer().size() == 0) {
                    self->owner().ReleaseIdle(std::move(self->connection_));
                }
                self->Finish(std::move(result));
            });
    }

    void CloseTransport() noexcept override {
        if (connection_) {
            connection_->Close();
            connection_.reset();
        }
    }

    std::shared_ptr<TlsPooledConnection> connection_;
    TlsContext& tls_context_;
    bool reused_ = false;
};

}

namespace {

// 新的增量路径使用协程；仍登记在既有 operation 表，共享 TLS、连接池和取消收口。
template <typename Connection>
class StreamingHttpOperation final : public AsyncHttpOperationBase {
public:
    StreamingHttpOperation(std::shared_ptr<AsyncBeastHttpClient::Impl> owner, ParsedUrl url,
                           HttpClientRequest request, HttpStreamOptions options, HttpStreamCallbacks callbacks)
        : AsyncHttpOperationBase(owner, std::move(url), std::move(request),
            [complete = callbacks.on_complete](core::Result<HttpClientResponse> result) {
                complete(result.ok() ? core::Status::Ok() : result.status());
            }), options_(options), callbacks_(std::move(callbacks)), idle_timer_(executor()) {}

private:
    void StartOnIoThread() override {
        auto self = std::static_pointer_cast<StreamingHttpOperation>(shared_from_this());
        asio::co_spawn(executor(), Run(), [self](std::exception_ptr error) {
            if (error) {
                self->Finish(core::Status::Error(core::ErrorCode::InternalError,
                                                "HTTP streaming coroutine failed"));
            }
        });
    }

    void ArmIdle() {
        idle_timer_.expires_after(options_.idle_timeout);
        auto self = std::static_pointer_cast<StreamingHttpOperation>(shared_from_this());
        idle_timer_.async_wait([self](beast::error_code error) {
            if (error || self->completed()) return;
            self->Finish(core::Status::Error(core::ErrorCode::Timeout, "HTTP stream idle timeout"));
        });
    }

    asio::awaitable<void> Run() {
        StartDeadline();
        const auto origin = MakeOriginKey(url());
        connection_ = owner().AcquireIdle<Connection>(origin);
        const bool reused = static_cast<bool>(connection_);
        if (!reused) {
            if constexpr (std::is_same_v<Connection, TlsPooledConnection>) {
                connection_ = std::make_shared<Connection>(owner().io_context, origin,
                                                          owner().options.tls_context->asio_context());
                if (auto status = owner().options.tls_context->PrepareConnection(
                        connection_->stream.native_handle(), url().host); !status.ok()) {
                    Finish(status);
                    co_return;
                }
            } else {
                connection_ = std::make_shared<Connection>(owner().io_context, origin);
            }
        }
        beast::error_code error;
        if (!reused) {
            auto endpoints = co_await resolver().async_resolve(url().host, std::to_string(url().port),
                asio::redirect_error(asio::use_awaitable, error));
            if (completed() || HandleTransportError("stream DNS resolve", error)) co_return;
            co_await beast::get_lowest_layer(connection_->stream).async_connect(endpoints,
                asio::redirect_error(asio::use_awaitable, error));
            if (completed() || HandleTransportError("stream TCP connect", error)) co_return;
            if constexpr (std::is_same_v<Connection, TlsPooledConnection>) {
                co_await connection_->stream.async_handshake(ssl::stream_base::client,
                    asio::redirect_error(asio::use_awaitable, error));
                if (completed() || HandleTransportError("stream TLS handshake", error)) co_return;
            }
        }
        co_await http::async_write(connection_->stream, request(),
            asio::redirect_error(asio::use_awaitable, error));
        if (completed() || HandleTransportError("stream HTTP write", error)) co_return;
        ArmIdle();
        http::response_parser<http::buffer_body> parser;
        parser.body_limit(options_.max_body_bytes);
        parser.header_limit(16 * 1024);
        co_await http::async_read_header(connection_->stream, buffer(), parser,
            asio::redirect_error(asio::use_awaitable, error));
        if (completed() || HandleTransportError("stream HTTP headers", error)) co_return;
        HttpClientResponse headers;
        headers.status = static_cast<int>(parser.get().result_int());
        for (const auto& field : parser.get())
            headers.headers.push_back({std::string(field.name_string()), std::string(field.value())});
        if (auto status = callbacks_.on_headers(headers); !status.ok()) {
            Finish(status);
            co_return;
        }
        auto allocated = ::net::SharedBuffer::AllocateCapacity(owner().stream_memory_pool,
                                                               options_.read_buffer_bytes);
        if (!allocated.ok()) {
            Finish(allocated.status());
            co_return;
        }
        auto chunk = std::move(allocated).value();
        while (!parser.is_done()) {
            // Beast buffer_body 必须借用可写裸缓冲区；池化块由协程局部 RAII 对象跨 await 持有。
            parser.get().body().data = chunk.char_data();
            parser.get().body().size = chunk.capacity();
            co_await http::async_read_some(connection_->stream, buffer(), parser,
                asio::redirect_error(asio::use_awaitable, error));
            if (completed()) co_return;
            const auto bytes = chunk.capacity() - parser.get().body().size;
            if (error == http::error::need_buffer) error.clear();
            if (HandleTransportError("stream HTTP body", error)) co_return;
            if (bytes) {
                ArmIdle();
                if (auto status = callbacks_.on_body({chunk.char_data(), bytes}); !status.ok()) {
                    Finish(status);
                    co_return;
                }
            }
        }
        beast::error_code ignored;
        idle_timer_.cancel(ignored);
        if (request().keep_alive() && parser.get().keep_alive() && buffer().size() == 0)
            owner().ReleaseIdle(std::move(connection_));
        Finish(HttpClientResponse{});
    }

    void CloseTransport() noexcept override {
        beast::error_code ignored;
        idle_timer_.cancel(ignored);
        // 取消时只关闭 socket；协程及底层 awaitable 仍持有 stream，不能提前销毁连接对象。
        if (connection_) connection_->Close();
    }

    HttpStreamOptions options_;
    HttpStreamCallbacks callbacks_;
    asio::steady_timer idle_timer_;
    std::shared_ptr<Connection> connection_;
};

}

void AsyncBeastHttpClient::Impl::Shutdown() noexcept {
    std::vector<std::shared_ptr<AsyncHttpOperationBase>> pending;
    std::vector<std::shared_ptr<PooledConnection>> idle;
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            return;
        }
        stopping = true;
        pending.reserve(operations.size());
        for (const auto& [_, operation] : operations) {
            pending.push_back(operation);
        }
        idle.reserve(idle_connection_count);
        for (auto& [_, connections] : idle_connections) {
            for (auto& connection : connections) {
                idle.push_back(std::move(connection));
            }
        }
        idle_connections.clear();
        idle_connection_count = 0;
    }

    for (const auto& connection : idle) {
        connection->Close();
    }
    for (const auto& operation : pending) {
        operation->Cancel();
    }
    pending.clear();
    guard.reset();
    JoinThreads();
}

core::Result<std::unique_ptr<AsyncBeastHttpClient>> AsyncBeastHttpClient::Create(
    AsyncBeastHttpClientOptions options) {
    if (options.io_thread_count == 0) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "async HTTP io_thread_count must be > 0");
    }
    if (options.enable_keep_alive &&
        (options.max_idle_connections == 0 ||
         options.max_idle_connections_per_origin == 0 ||
         options.idle_connection_timeout <= std::chrono::milliseconds::zero())) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "async HTTP keep-alive pool limits and idle timeout must be > 0");
    }

    try {
        auto impl = std::make_shared<Impl>(std::move(options));
        if (auto status = impl->StartThreads(); !status.ok()) {
            return status;
        }
        return std::unique_ptr<AsyncBeastHttpClient>(new AsyncBeastHttpClient(std::move(impl)));
    } catch (const std::exception& error) {
        return core::Status::Error(
            core::ErrorCode::InternalError,
            std::string("failed to create async HTTP client: ") + error.what());
    }
}

AsyncBeastHttpClient::AsyncBeastHttpClient(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

AsyncBeastHttpClient::~AsyncBeastHttpClient() {
    Shutdown();
}

core::Result<std::shared_ptr<IAsyncHttpOperation>> AsyncBeastHttpClient::ExecuteAsync(
    HttpClientRequest request,
    Callback callback) {
    if (!callback) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "async HTTP callback is required");
    }
    if (request.timeout_ms <= 0) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "timeout_ms must be > 0");
    }

    auto parsed = ParseUrl(request.url);
    if (!parsed) {
        return parsed.status();
    }
    auto url = std::move(parsed).value();
    if (url.scheme == UrlScheme::Https && !impl_->options.tls_context) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "https URL but client has no TLS context");
    }

    try {
        std::shared_ptr<AsyncHttpOperationBase> operation;
        if (url.scheme == UrlScheme::Https) {
            operation = std::make_shared<TlsAsyncHttpOperation>(
                impl_,
                std::move(url),
                std::move(request),
                std::move(callback),
                *impl_->options.tls_context);
        } else {
            operation = std::make_shared<PlainAsyncHttpOperation>(
                impl_,
                std::move(url),
                std::move(request),
                std::move(callback));
        }

        if (auto status = impl_->Register(operation); !status.ok()) {
            return status;
        }
        operation->Start();
        return std::static_pointer_cast<IAsyncHttpOperation>(std::move(operation));
    } catch (const std::exception& error) {
        return core::Status::Error(
            core::ErrorCode::InternalError,
            std::string("failed to submit async HTTP request: ") + error.what());
    }
}

core::Result<std::shared_ptr<IAsyncHttpOperation>> AsyncBeastHttpClient::ExecuteStreamingAsync(
    HttpClientRequest request, HttpStreamOptions options, HttpStreamCallbacks callbacks) {
    if (!callbacks.on_headers || !callbacks.on_body || !callbacks.on_complete ||
        request.timeout_ms <= 0 || !options.max_body_bytes || !options.read_buffer_bytes ||
        options.read_buffer_bytes > options.max_body_bytes || options.idle_timeout.count() <= 0)
        return core::Status::Error(core::ErrorCode::InvalidArgument, "invalid HTTP stream options or callbacks");
    auto parsed = ParseUrl(request.url);
    if (!parsed.ok()) return parsed.status();
    auto url = std::move(parsed).value();
    if (url.scheme == UrlScheme::Https && !impl_->options.tls_context)
        return core::Status::Error(core::ErrorCode::InvalidArgument, "https stream requires TLS context");
    try {
        std::shared_ptr<AsyncHttpOperationBase> operation;
        if (url.scheme == UrlScheme::Https)
            operation = std::make_shared<StreamingHttpOperation<TlsPooledConnection>>(
                impl_, std::move(url), std::move(request), options, std::move(callbacks));
        else
            operation = std::make_shared<StreamingHttpOperation<PlainPooledConnection>>(
                impl_, std::move(url), std::move(request), options, std::move(callbacks));
        if (auto status = impl_->Register(operation); !status.ok()) return status;
        operation->Start();
        return std::static_pointer_cast<IAsyncHttpOperation>(operation);
    } catch (const std::exception&) {
        return core::Status::Error(core::ErrorCode::InternalError, "failed to submit HTTP streaming coroutine");
    }
}

void AsyncBeastHttpClient::Shutdown() noexcept {
    if (impl_) {
        impl_->Shutdown();
    }
}

}
