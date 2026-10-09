#pragma once

#include "connection_pool.h"
#include "http_request_filter.h"
#include "http_types.h"
#include "logger_adapter.h"
#include "protocol_types.h"
#include "request_interfaces.h"
#include "static_file_handler.h"
#include "shared_buffer.h"
#include "websocket_types.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http/parser.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace net {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

struct HttpServerOptions {
    std::string address = "0.0.0.0";
    unsigned short port = 8080;
    std::size_t io_threads = 1;
    std::chrono::seconds request_timeout{30};
    std::chrono::seconds websocket_idle_timeout{60};
    std::size_t request_body_limit = 16 * 1024 * 1024;
    std::size_t websocket_read_buffer_limit = 16 * 1024 * 1024;
    ConnectionPoolOptions connection_pool;
    WebSocketOptions websocket;
    // 普通 WebSocket 路由的接收 fragment 调优策略。
    WebSocketReadTuning websocket_read_tuning;
    // 流式 WebSocket 路由的接收 fragment 调优策略。
    WebSocketReadTuning websocket_stream_read_tuning{
        256 * 1024,
        1024 * 1024,
        2};
    // Optional early HTTP filter. It runs after Beast parses the request and
    // before access control, route handlers, static files, or WebSocket upgrade.
    HttpRequestFilterOptions request_filter;
    std::optional<StaticFileOptions> static_files;
    // 网络层统一输出 WS 生命周期日志；静态链接消费者可注入自己的模块 logger。
    core::LoggerAdapter logger = core::LoggerAdapter::ForModule("net");
};

using HttpGeneratorCallback = std::function<void(http::message_generator)>;
using HttpGeneratorHandler = std::function<void(HttpRequest, HttpGeneratorCallback)>;
class HttpServer {
public:
    explicit HttpServer(HttpServerOptions options = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    core::Status Start();
    void Stop();

    void SetHttpHandler(HttpGeneratorHandler handler);
    // Typed request handlers own the response timing. Call Respond() once or
    // Close() explicitly; the server does not auto-generate a fallback response.
    void SetHttpRequestHandler(IHttpRequestHandler handler);
    void SetAccessController(HttpAccessController controller);
    void SetStaticFiles(StaticFileOptions options);
    // Registers WebSocket endpoints that receive one complete logical message per callback.
    // A message may retain multiple pooled fragments internally without coalescing copies.
    // For oversized input the handler receives a WebSocketMessage with !ok();
    // the connection is kept open unless a protocol/internal error occurs.
    void SetWebSocketHandler(std::string path, WebSocketMessageHandler handler);
    void SetWebSocketStreamHandler(std::string path, IWebSocketStreamHandler handler);
    void SetWebSocketAcceptHandler(WebSocketAcceptHandler handler);
    void SetWebSocketCloseHandler(WebSocketCloseHandler handler);

    bool running() const noexcept;
    std::uint16_t port() const noexcept;
    ConnectionPoolStats ConnectionStats() const;
    std::vector<ConnectionSnapshot> ConnectionSnapshots() const;

private:
    class Listener;
    class HttpSession;

    friend class Listener;
    friend class HttpSession;

    std::uint64_t NextConnectionId() noexcept;
    HttpGeneratorHandler HttpHandlerSnapshot() const;
    IHttpRequestHandler HttpRequestHandlerSnapshot() const;
    HttpAccessController AccessControllerSnapshot() const;
    std::shared_ptr<HttpRequestFilter> RequestFilterSnapshot() const;
    WebSocketMessageHandler WebSocketHandlerSnapshot(std::string_view path) const;
    IWebSocketStreamHandler WebSocketStreamHandlerSnapshot(std::string_view path) const;
    WebSocketAcceptHandler WebSocketAcceptHandlerSnapshot() const;
    WebSocketCloseHandler WebSocketCloseHandlerSnapshot() const;
    std::shared_ptr<StaticFileHandler> StaticFileHandlerSnapshot() const;

    HttpServerOptions options_;
    asio::io_context io_context_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::shared_ptr<Listener> listener_;
    std::vector<std::jthread> io_threads_;
    mutable std::mutex handler_mutex_;
    HttpGeneratorHandler http_handler_;
    IHttpRequestHandler http_request_handler_;
    HttpAccessController access_controller_;
    std::shared_ptr<HttpRequestFilter> request_filter_;
    std::string websocket_path_ = "/ws";
    WebSocketMessageHandler websocket_handler_;
    std::string websocket_stream_path_ = "/ws";
    IWebSocketStreamHandler websocket_stream_handler_;
    WebSocketAcceptHandler websocket_accept_handler_;
    WebSocketCloseHandler websocket_close_handler_;
    std::shared_ptr<StaticFileHandler> static_file_handler_;
    std::vector<std::weak_ptr<HttpSession>> event_streams_;
    ConnectionPool connection_pool_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_connection_id_{1};
    std::uint16_t bound_port_ = 0;
};
class HttpServer::HttpSession : public IServerEventStream,
                               public std::enable_shared_from_this<HttpSession> {
public:
    class HttpServerRequest;

    bool TryUpgradeWebSocket();
    core::Status SendFromRequest(http::message_generator response);
    void CloseFromRequest(ConnectionCloseInfo close_info);
    const ConnectionContext& connection() const noexcept;
    core::RawMemoryPool& memory_pool() noexcept;
    core::ThreadPool* task_pool() const noexcept;
    core::Result<std::shared_ptr<IServerEventStream>> BeginEventStream(
        SseStreamOptions options, EventStreamCloseCallback callback);
    core::Status SendEvent(ServerSentEvent event) override;
    void FinishEvents() override;
    void AbortEvents(core::Status status) override;
    core::Status AcknowledgeHeartbeat() override;
    std::uint64_t stream_connection_id() const noexcept override { return sse_connection_id_; }
    BackpressureStats EventQueueStats() const override;
    void NotifyEventStreamClosed(core::Status status);

    HttpSession(HttpServer& server, tcp::socket socket, ConnectionLease lease)
        : server_(server),
          stream_(std::move(socket)),
          lease_(std::move(lease)) {}

    void Run() {
        DoRead();
    }

private:
    friend class HttpServer;
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t);
    void OnAccessDecision(AccessDecision decision);
    void Send(http::message_generator message);
    void OnWrite(bool keep_alive, beast::error_code ec, std::size_t);
    void Close(ConnectionCloseInfo close_info);
    asio::awaitable<void> WriteEvents();
    asio::awaitable<void> WatchEventPeer();

    HttpServer& server_;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> request_parser_;
    BeastHttpRequest request_;
    ConnectionLease lease_;
    std::atomic<bool> response_started_{false};
    SseStreamOptions sse_options_;
    EventStreamCloseCallback sse_close_callback_;
    // 关闭租约会释放其内存池；SSE 自有池确保挂起的写协程仍持有有效缓冲区。
    core::BucketMemoryPool sse_memory_pool_{0, 4};
    std::unique_ptr<BackpressureQueue<SharedBuffer>> sse_queue_;
    asio::steady_timer sse_wakeup_{stream_.get_executor()};
    std::atomic<bool> sse_finished_{false};
    std::atomic<bool> sse_closed_{false};
    std::uint64_t sse_connection_id_ = 0;
    std::chrono::steady_clock::time_point sse_started_at_;
    std::chrono::steady_clock::time_point sse_last_activity_;
};

class HttpServer::HttpSession::HttpServerRequest final : public IHttpRequest {
public:
    HttpServerRequest(std::shared_ptr<HttpSession> session, BeastHttpRequest message)
        : session_(std::move(session)),
          message_(std::move(message)) {}

    const BeastHttpRequest& message() const noexcept override {
        return message_;
    }

    const ConnectionContext& connection() const noexcept override {
        return session_->connection();
    }

    core::RawMemoryPool& memory_pool() noexcept override {
        return session_->memory_pool();
    }

    core::ThreadPool* task_pool() const noexcept override {
        return session_->task_pool();
    }

    core::Status Respond(http::message_generator response) override {
        return session_->SendFromRequest(std::move(response));
    }

    void Close(ConnectionCloseInfo close_info) override {
        session_->CloseFromRequest(std::move(close_info));
    }
    core::Result<std::shared_ptr<IServerEventStream>> BeginEventStream(
        SseStreamOptions options, EventStreamCloseCallback callback) override {
        return session_->BeginEventStream(std::move(options), std::move(callback));
    }

private:
    std::shared_ptr<HttpSession> session_;
    BeastHttpRequest message_;
};

} // namespace net
