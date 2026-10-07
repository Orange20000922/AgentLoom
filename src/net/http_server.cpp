#include "http_server.h"

#include "boost/beast/http/dynamic_body.hpp"
#include "boost/beast/http/message.hpp"
#include "boost/beast/http/parser.hpp"
#include "exception.h"
#include "trace_context.h"
#include "websocket_session.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace net {

namespace {

std::size_t ResolveIoThreads(std::size_t requested) noexcept {
    if (requested > 0) {
        return requested;
    }
    const auto hardware = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1, hardware == 0 ? 1 : hardware);
}

std::string RemoteAddress(const tcp::socket& socket) {
    boost::system::error_code ec;
    auto endpoint = socket.remote_endpoint(ec);
    if (ec) {
        return {};
    }
    return endpoint.address().to_string(ec);
}

http::message_generator MakeStatusResponse(const BeastHttpRequest& request,
                                           http::status status,
                                           std::string body) {
    auto response = HttpResponse::Text(status, std::move(body));
    response.message.version(request.version());
    response.message.keep_alive(request.keep_alive());
    response.message.set(http::field::server, "AgentLoom");
    response.message.prepare_payload();
    return std::move(response.message);
}

http::message_generator MakeStatusResponse(unsigned version,
                                           bool keep_alive,
                                           http::status status,
                                           std::string body) {
    auto response = HttpResponse::Text(status, std::move(body));
    response.message.version(version);
    response.message.keep_alive(keep_alive);
    response.message.set(http::field::server, "AgentLoom");
    response.message.prepare_payload();
    return std::move(response.message);
}

http::status StatusFromAccessDecision(const AccessDecision& decision) noexcept {
    auto status = static_cast<http::status>(decision.http_status);
    if (status == http::status::unknown) {
        return http::status::forbidden;
    }
    return status;
}

} 

class HttpServer::Listener : public std::enable_shared_from_this<Listener> {
public:
    void OnAccept(beast::error_code ec, tcp::socket socket);

    Listener(HttpServer& server, tcp::endpoint endpoint)
        : server_(server),
          acceptor_(asio::make_strand(server.io_context_)) {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw core::AppException(core::Status::Error(core::ErrorCode::InternalError, ec.message()));
        }
#if !defined(_WIN32)
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            throw core::AppException(core::Status::Error(core::ErrorCode::InternalError, ec.message()));
        }
#endif
        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw core::AppException(core::Status::Error(core::ErrorCode::InternalError, ec.message()));
        }
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw core::AppException(core::Status::Error(core::ErrorCode::InternalError, ec.message()));
        }
    }

    void Run() {
        DoAccept();
    }

    void Stop() {
        beast::error_code ec;
        acceptor_.cancel(ec);
        acceptor_.close(ec);
    }

    std::uint16_t port() const noexcept {
        beast::error_code ec;
        auto endpoint = acceptor_.local_endpoint(ec);
        return ec ? 0 : endpoint.port();
    }

private:
    void DoAccept() {
        acceptor_.async_accept(
            asio::make_strand(server_.io_context_),
            beast::bind_front_handler(&Listener::OnAccept, shared_from_this()));
    }

    HttpServer& server_;
    tcp::acceptor acceptor_;
};


void HttpServer::HttpSession::DoRead() {
    request_ = {};
    request_parser_.emplace();
    request_parser_->body_limit(server_.options_.request_body_limit);
    buffer_.consume(buffer_.size());
    stream_.expires_after(server_.options_.request_timeout);

    http::async_read(
        stream_,
        buffer_,
        *request_parser_,
        beast::bind_front_handler(&HttpSession::OnRead, shared_from_this()));
}

void HttpServer::HttpSession::OnRead(beast::error_code ec, std::size_t) {
    if (ec == http::error::end_of_stream) {
        Close(ConnectionCloseInfo::Remote());
        return;
    }
    if (ec == beast::error::timeout) {
        Close(ConnectionCloseInfo::Timeout("http request timeout"));
        return;
    }
    if (ec == http::error::body_limit) {
        if (request_parser_) {
            const auto& parsed = request_parser_->get();
            auto response = MakeStatusResponse(
                parsed.version(), false, http::status::payload_too_large, "payload too large");
            request_parser_.reset();
            Send(std::move(response));
        } else {
            Close({ConnectionCloseReason::ProtocolError,
                   core::Status::Error(core::ErrorCode::ResourceExhausted,
                                       "HTTP request body exceeds configured limit"),
                   "HTTP request body exceeds configured limit"});
        }
        return;
    }
    if (ec) {
        Close({ConnectionCloseReason::ProtocolError,
               core::Status::Error(core::ErrorCode::InvalidArgument, ec.message()),
               ec.message()});
        return;
    }

    request_ = request_parser_->release();
    request_parser_.reset();
    stream_.expires_never();
    std::string trace_id;
    auto trace_header = request_.find("X-Trace-Id");
    if (trace_header != request_.end()) {
        trace_id = std::string(trace_header->value());
    } else {
        trace_id = core::GenerateTraceId();
    }

    core::TraceContext trace_ctx;
    trace_ctx.trace_id = std::move(trace_id);
    core::TraceScope trace_scope(trace_ctx);

    if (request_.body().size() > server_.options_.request_body_limit) {
        Send(MakeStatusResponse(request_, http::status::payload_too_large, "payload too large"));
        return;
    }

    if (auto request_filter = server_.RequestFilterSnapshot()) {
        auto decision = request_filter->Check(request_);
        if (!decision.allowed) {
            Send(MakeStatusResponse(request_, decision.status, std::move(decision.reason)));
            return;
        }
    }

    auto access_controller = server_.AccessControllerSnapshot();
    if (access_controller) {
        access_controller(request_, lease_.context(), [self = shared_from_this()](AccessDecision decision) {
            asio::post(self->stream_.get_executor(), [self, decision = std::move(decision)]() mutable {
                self->OnAccessDecision(std::move(decision));
            });
        });
        return;
    }

    OnAccessDecision(AccessDecision::Allow());
}
// This function is called after the access control decision is made for an incoming HTTP request. It takes the AccessDecision object as a parameter, which indicates whether the request is allowed or denied, along with an optional reason and HTTP status code. If the decision is to deny access, it sends an appropriate HTTP response back to the client with the specified status and reason. If access is allowed, it proceeds to handle the request by checking for WebSocket upgrade, invoking the appropriate request handler, or serving static files based on the server's configuration.
/**
 * @brief Handles the access control decision for an incoming HTTP request.
 * @param decision The access control decision.
 */
void HttpServer::HttpSession::OnAccessDecision(AccessDecision decision) {
    if (!decision.allowed()) {
        auto body = decision.reason.empty() ? std::string("access denied") : decision.reason;
        Send(MakeStatusResponse(request_, StatusFromAccessDecision(decision), std::move(body)));
        return;
    }

    if (websocket::is_upgrade(request_) && TryUpgradeWebSocket()) {
        return;
    }

    auto request_handler = server_.HttpRequestHandlerSnapshot();
    if (request_handler) {
        std::shared_ptr<IHttpRequest> request =
            std::make_shared<HttpServerRequest>(shared_from_this(), std::move(request_));
        request_handler(std::move(request));
        return;
    }

    auto handler = server_.HttpHandlerSnapshot();
    if (handler) {
        auto request = HttpRequest::FromBeast(std::move(request_), lease_.context());
        handler(std::move(request), HttpGeneratorCallback([self = shared_from_this()](http::message_generator response) mutable {
            asio::post(self->stream_.get_executor(), [self, response = std::move(response)]() mutable {
                self->Send(std::move(response));
            });
        }));
        return;
    }

    if (auto static_files = server_.StaticFileHandlerSnapshot()) {
        Send(static_files->Handle(request_));
        return;
    }

    Send(MakeStatusResponse(request_, http::status::not_found, "not found"));
}

void HttpServer::HttpSession::Send(http::message_generator message) {
    const auto keep_alive = message.keep_alive();
    stream_.expires_after(server_.options_.request_timeout);
    beast::async_write(
        stream_,
        std::move(message),
        beast::bind_front_handler(&HttpSession::OnWrite, shared_from_this(), keep_alive));
}

void HttpServer::HttpSession::OnWrite(bool keep_alive, beast::error_code ec, std::size_t) {
    if (ec == beast::error::timeout) {
        Close(ConnectionCloseInfo::Timeout("http response timeout"));
        return;
    }
    if (ec) {
        Close({ConnectionCloseReason::InternalError,
               core::Status::Error(core::ErrorCode::InternalError, ec.message()),
               ec.message()});
        return;
    }
    lease_.Touch();

    if (!keep_alive) {
        Close(ConnectionCloseInfo::Remote("http keep-alive disabled"));
        return;
    }
    DoRead();
}

void HttpServer::HttpSession::Close(ConnectionCloseInfo close_info) {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    stream_.socket().close(ec);
    lease_.Close(std::move(close_info));
}

core::Status HttpServer::HttpSession::SendFromRequest(http::message_generator response) {
    asio::post(stream_.get_executor(), [self = shared_from_this(), response = std::move(response)]() mutable {
        self->Send(std::move(response));
    });
    return core::Status::Ok();
}

void HttpServer::HttpSession::CloseFromRequest(ConnectionCloseInfo close_info) {
    asio::post(stream_.get_executor(), [self = shared_from_this(), close_info = std::move(close_info)]() mutable {
        self->Close(std::move(close_info));
    });
}

const ConnectionContext& HttpServer::HttpSession::connection() const noexcept {
    return lease_.context();
}

core::RawMemoryPool& HttpServer::HttpSession::memory_pool() noexcept {
    return lease_.memory_pool();
}

core::ThreadPool* HttpServer::HttpSession::task_pool() const noexcept {
    return lease_.task_pool();
}

void HttpServer::Listener::OnAccept(beast::error_code ec, tcp::socket socket) {
    if (!server_.running_.load(std::memory_order_acquire)) {
        return;
    }
    if (!ec) {
        ConnectionContext connection{server_.NextConnectionId(), RemoteAddress(socket)};
        auto lease_result = server_.connection_pool_.Acquire(ProtocolConnectionKind::Http, std::move(connection));
        if (lease_result.ok()) {
            std::make_shared<HttpSession>(server_, std::move(socket), std::move(lease_result).value())->Run();
        } else {
            beast::error_code close_ec;
            socket.shutdown(tcp::socket::shutdown_both, close_ec);
            socket.close(close_ec);
        }
    }
    DoAccept();
}

bool HttpServer::HttpSession::TryUpgradeWebSocket() {
    const auto target = std::string_view(request_.target().data(), request_.target().size());
    auto handler = server_.WebSocketHandlerSnapshot(target);
    auto stream_handler = server_.WebSocketStreamHandlerSnapshot(target);
    if (!handler && !stream_handler) {
        return false;
    }

    auto status = lease_.SetKind(ProtocolConnectionKind::WebSocket);
    if (!status.ok()) {
        Send(MakeStatusResponse(request_, http::status::service_unavailable, status.message()));
        return true;
    }

    WebSocketSessionOptions options;
    options.request_timeout = server_.options_.request_timeout;
    options.logger = server_.options_.logger;
    options.read_buffer_limit = server_.options_.websocket_read_buffer_limit;
    options.websocket = server_.options_.websocket;
    options.read_tuning = stream_handler
                              ? server_.options_.websocket_stream_read_tuning
                              : server_.options_.websocket_read_tuning;
    options.websocket.idle_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(server_.options_.websocket_idle_timeout);

    WebSocketSessionCallbacks callbacks;
    callbacks.message_handler = std::move(handler);
    callbacks.stream_handler = std::move(stream_handler);
    callbacks.accept_handler = server_.WebSocketAcceptHandlerSnapshot();
    callbacks.close_handler = server_.WebSocketCloseHandlerSnapshot();

    std::make_shared<WebSocketSession>(
        stream_.release_socket(),
        std::move(request_),
        std::move(lease_),
        std::move(options),
        std::move(callbacks))
        ->Run();
    return true;
}

HttpServer::HttpServer(HttpServerOptions options)
    : options_(std::move(options)),
      io_context_(static_cast<int>(ResolveIoThreads(options_.io_threads))),
      connection_pool_(options_.connection_pool) {
    if (options_.static_files) {
        static_file_handler_ = std::make_shared<StaticFileHandler>(*options_.static_files);
    }
    if (options_.request_filter.enabled) {
        request_filter_ = std::make_shared<HttpRequestFilter>(options_.request_filter);
    }
}

HttpServer::~HttpServer() {
    Stop();
}

core::Status HttpServer::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return core::Status::Ok();
    }

    try {
        auto address = asio::ip::make_address(options_.address);
        auto endpoint = tcp::endpoint(address, options_.port);
        work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io_context_.get_executor());
        listener_ = std::make_shared<Listener>(*this, endpoint);
        bound_port_ = listener_->port();
        listener_->Run();

        const auto thread_count = ResolveIoThreads(options_.io_threads);
        io_threads_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            io_threads_.emplace_back([this](std::stop_token) {
                io_context_.run();
            });
        }
    } catch (const core::AppException& e) {
        running_.store(false, std::memory_order_release);
        return e.status();
    } catch (const std::exception& e) {
        running_.store(false, std::memory_order_release);
        return core::Status::Error(core::ErrorCode::InternalError, e.what());
    }

    return core::Status::Ok();
}

void HttpServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (listener_) {
        asio::post(io_context_, [listener = listener_] {
            listener->Stop();
        });
    }
    if (work_guard_) {
        work_guard_->reset();
    }
    io_context_.stop();
    for (auto& thread : io_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    io_threads_.clear();
    listener_.reset();
    connection_pool_.CloseAll(ConnectionCloseInfo::Shutdown("http server stopped"));
}
/**
 * @brief Sets the HTTP handler for the server.
 * @param handler The HTTP handler to set.
 */
void HttpServer::SetHttpHandler(HttpGeneratorHandler handler) {
    std::lock_guard lock(handler_mutex_);
    http_handler_ = std::move(handler);
}
/**
 * @brief Sets the HTTP request handler for the server.
 * @param handler The HTTP request handler to set.
 */
void HttpServer::SetHttpRequestHandler(IHttpRequestHandler handler) {
    std::lock_guard lock(handler_mutex_);
    http_request_handler_ = std::move(handler);
}

void HttpServer::SetAccessController(HttpAccessController controller) {
    std::lock_guard lock(handler_mutex_);
    access_controller_ = std::move(controller);
}

void HttpServer::SetStaticFiles(StaticFileOptions options) {
    std::lock_guard lock(handler_mutex_);
    options_.static_files = options;
    static_file_handler_ = std::make_shared<StaticFileHandler>(std::move(options));
}

void HttpServer::SetWebSocketHandler(std::string path, WebSocketMessageHandler handler) {
    std::lock_guard lock(handler_mutex_);
    websocket_path_ = std::move(path);
    websocket_handler_ = std::move(handler);
}

void HttpServer::SetWebSocketStreamHandler(std::string path, IWebSocketStreamHandler handler) {
    std::lock_guard lock(handler_mutex_);
    websocket_stream_path_ = std::move(path);
    websocket_stream_handler_ = std::move(handler);
}

void HttpServer::SetWebSocketAcceptHandler(WebSocketAcceptHandler handler) {
    std::lock_guard lock(handler_mutex_);
    websocket_accept_handler_ = std::move(handler);
}

void HttpServer::SetWebSocketCloseHandler(WebSocketCloseHandler handler) {
    std::lock_guard lock(handler_mutex_);
    websocket_close_handler_ = std::move(handler);
}

bool HttpServer::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::uint16_t HttpServer::port() const noexcept {
    return bound_port_;
}

ConnectionPoolStats HttpServer::ConnectionStats() const {
    return connection_pool_.Stats();
}

std::vector<ConnectionSnapshot> HttpServer::ConnectionSnapshots() const {
    return connection_pool_.Snapshots();
}

std::uint64_t HttpServer::NextConnectionId() noexcept {
    return next_connection_id_.fetch_add(1, std::memory_order_relaxed);
}

HttpGeneratorHandler HttpServer::HttpHandlerSnapshot() const {
    std::lock_guard lock(handler_mutex_);
    return http_handler_;
}

IHttpRequestHandler HttpServer::HttpRequestHandlerSnapshot() const {
    std::lock_guard lock(handler_mutex_);
    return http_request_handler_;
}

HttpAccessController HttpServer::AccessControllerSnapshot() const {
    std::lock_guard lock(handler_mutex_);
    return access_controller_;
}

std::shared_ptr<HttpRequestFilter> HttpServer::RequestFilterSnapshot() const {
    std::lock_guard lock(handler_mutex_);
    return request_filter_;
}

WebSocketMessageHandler HttpServer::WebSocketHandlerSnapshot(std::string_view path) const {
    std::lock_guard lock(handler_mutex_);
    return path == websocket_path_ ? websocket_handler_ : WebSocketMessageHandler{};
}

IWebSocketStreamHandler HttpServer::WebSocketStreamHandlerSnapshot(std::string_view path) const {
    std::lock_guard lock(handler_mutex_);
    return path == websocket_stream_path_ ? websocket_stream_handler_ : IWebSocketStreamHandler{};
}

WebSocketAcceptHandler HttpServer::WebSocketAcceptHandlerSnapshot() const {
    std::lock_guard lock(handler_mutex_);
    return websocket_accept_handler_;
}

WebSocketCloseHandler HttpServer::WebSocketCloseHandlerSnapshot() const {
    std::lock_guard lock(handler_mutex_);
    return websocket_close_handler_;
}

std::shared_ptr<StaticFileHandler> HttpServer::StaticFileHandlerSnapshot() const {
    std::lock_guard lock(handler_mutex_);
    return static_file_handler_;
}

} // namespace net
