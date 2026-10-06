#include "websocket_session.h"

#include <algorithm>
#include <utility>

namespace net {

namespace {

WebSocketMessageKind WebSocketKindFromStream(const websocket::stream<beast::tcp_stream>& stream) noexcept {
    return stream.got_text() ? WebSocketMessageKind::Text : WebSocketMessageKind::Binary;
}

std::string_view CloseReasonName(ConnectionCloseReason reason) noexcept {
    switch (reason) {
    case ConnectionCloseReason::RemoteClosed: return "remote_closed";
    case ConnectionCloseReason::IdleTimeout: return "idle_timeout";
    case ConnectionCloseReason::ResponseTimeout: return "response_timeout";
    case ConnectionCloseReason::AccessDenied: return "access_denied";
    case ConnectionCloseReason::BackpressureLimit: return "backpressure_limit";
    case ConnectionCloseReason::ProtocolError: return "protocol_error";
    case ConnectionCloseReason::ServerShutdown: return "server_shutdown";
    case ConnectionCloseReason::InternalError: return "internal_error";
    }
    return "unknown";
}

core::Status MessageTooLargeStatus(std::string message = "websocket message is too large") {
    return core::Status::Error(core::ErrorCode::ResourceExhausted, std::move(message));
}

} // namespace

class WebSocketSession::WebSocketStreamRequest final : public IWebSocketStreamRequest {
public:
    WebSocketStreamRequest(std::shared_ptr<WebSocketSession> session, WebSocketMessage message)
        : session_(std::move(session)),
          message_(std::move(message)) {}

    const BeastHttpRequest& handshake_request() const noexcept override {
        return session_->request_;
    }

    const ConnectionContext& connection() const noexcept override {
        return session_->lease_.context();
    }

    WebSocketMessage& message() noexcept override {
        return message_;
    }

    const WebSocketMessage& message() const noexcept override {
        return message_;
    }

    core::RawMemoryPool& memory_pool() noexcept override {
        return session_->lease_.memory_pool();
    }

    core::ThreadPool* task_pool() const noexcept override {
        return session_->lease_.task_pool();
    }

    core::Status Send(WebSocketFrame frame) override {
        return session_->Send(std::move(frame));
    }

    void Close(ConnectionCloseInfo close_info) override {
        session_->Close(std::move(close_info));
    }

private:
    std::shared_ptr<WebSocketSession> session_;
    WebSocketMessage message_;
};

WebSocketSession::WebSocketSession(tcp::socket socket,
                                   BeastHttpRequest request,
                                   ConnectionLease lease,
                                   WebSocketSessionOptions options,
                                   WebSocketSessionCallbacks callbacks)
    : stream_(std::move(socket)),
      response_timer_(stream_.get_executor()),
      request_(std::move(request)),
      lease_(std::move(lease)),
      options_(std::move(options)),
      callbacks_(std::move(callbacks)),
      outbound_queue_(MakeWebSocketOutboundQueue(options_.websocket)),
      inbound_message_assembler_(options_.websocket) {}

WebSocketSession::~WebSocketSession() {
    // Stop 停止 IO 后再释放 Session；没有读回调时也补齐一次关闭摘要。
    // 析构只记录，不调用用户回调，不改变原有 drain 和连接回收顺序。
    if (!close_notified_.exchange(true, std::memory_order_acq_rel)) {
        ConnectionCloseInfo info;
        info.reason = ConnectionCloseReason::ServerShutdown;
        LogClosed(info);
    }
}

const ConnectionContext& WebSocketSession::connection() const noexcept {
    return lease_.context();
}

void WebSocketSession::LogClosed(const ConnectionCloseInfo& info) const noexcept {
    const auto& context = connection();
    const auto started = accepted_ ? accepted_at_ : context.connected_at;
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const auto level = info.reason == ConnectionCloseReason::InternalError
        ? spdlog::level::err
        : (info.reason == ConnectionCloseReason::RemoteClosed ||
           info.reason == ConnectionCloseReason::IdleTimeout ||
           info.reason == ConnectionCloseReason::ServerShutdown)
            ? spdlog::level::info : spdlog::level::warn;
    // 仅记录计数、固定原因和错误码；对端关闭文本可能包含用户输入，不写 detail/target/凭据。
    try {
        if (auto* logger = options_.logger.get()) {
            logger->log(level,
                "event=ws.closed connection_id={} remote_address={} accepted={} duration_ms={}"
                " messages={} bytes={} reason={} status_code={}",
                context.connection_id, context.remote_address, accepted_, duration,
                received_messages_, received_bytes_, CloseReasonName(info.reason),
                static_cast<int>(info.status.code()));
        }
    } catch (...) {
        // 诊断输出失败不能中断网络关闭或析构；连接状态仍按原协议回收。
    }
}

std::size_t WebSocketSession::InitialReadCapacity() const noexcept {
    const auto configured = options_.read_tuning.initial_read_bytes;
    const auto bounded = configured == 0 ? std::size_t{1} : configured;
    return std::min(bounded, MaxReadCapacity());
}

std::size_t WebSocketSession::MaxReadCapacity() const noexcept {
    const auto configured = options_.read_tuning.max_read_bytes;
    const auto bounded = configured == 0 ? options_.websocket.max_frame_bytes : configured;
    return std::min(bounded, options_.websocket.max_frame_bytes);
}

void WebSocketSession::ObserveRead(std::size_t requested_capacity,
                                   std::size_t bytes_transferred,
                                   bool final_fragment) {
    if (final_fragment) {
        read_capacity_ = InitialReadCapacity();
        full_read_streak_ = 0;
        return;
    }

    if (bytes_transferred == 0 || bytes_transferred < requested_capacity) {
        full_read_streak_ = 0;
        return;
    }

    const auto threshold = std::max<std::size_t>(
        1,
        options_.read_tuning.growth_full_read_threshold);
    if (++full_read_streak_ < threshold) {
        return;
    }

    full_read_streak_ = 0;
    const auto max_capacity = MaxReadCapacity();
    if (read_capacity_ >= max_capacity) {
        return;
    }

    const auto doubled = read_capacity_ > max_capacity / 2
                             ? max_capacity
                             : read_capacity_ * 2;
    read_capacity_ = std::max(read_capacity_ + 1, std::min(doubled, max_capacity));
}

void WebSocketSession::Run() {
    auto compression = options_.websocket.CompressionOptions(true);
    stream_.set_option(compression);
    stream_.auto_fragment(true);
    stream_.read_message_max(options_.read_buffer_limit);
    stream_.write_buffer_bytes(16 * 1024);
    // Beast 在 stream 内保存控制回调，强捕获 Session 会形成所有权闭环。
    stream_.control_callback([weak_self = weak_from_this()](websocket::frame_type type, beast::string_view payload) {
        if (auto self = weak_self.lock()) {
            self->OnControl(type, payload);
        }
    });
    stream_.next_layer().expires_after(options_.request_timeout);
    stream_.async_accept(
        request_,
        beast::bind_front_handler(&WebSocketSession::OnAccept, shared_from_this()));
}

core::Status WebSocketSession::Send(WebSocketFrame frame) {
    WebSocketFrame owned_frame;
    owned_frame.kind = frame.kind;
    owned_frame.final_fragment = frame.final_fragment;
    owned_frame.compressed = frame.compressed;
    if (!frame.payload.empty()) {
        auto payload_result = SharedBuffer::Copy(memory_pool_, frame.payload.view());
        if (!payload_result.ok()) {
            return payload_result.status();
        }
        owned_frame.payload = std::move(payload_result).value();
    }

    auto status = outbound_queue_.TryPush(std::move(owned_frame));
    if (!status.ok()) {
        Close(ConnectionCloseInfo::Backpressure(status.message()));
        return status;
    }
    asio::post(stream_.get_executor(), [self = shared_from_this()] {
        self->DoWrite();
    });
    return core::Status::Ok();
}

void WebSocketSession::Close(ConnectionCloseInfo close_info) {
    asio::post(stream_.get_executor(), [self = shared_from_this(), close_info = std::move(close_info)]() mutable {
        self->DoClose(std::move(close_info));
    });
}

void WebSocketSession::OnAccept(beast::error_code ec) {
    stream_.next_layer().expires_never();
    if (ec) {
        NotifyClose({ConnectionCloseReason::ProtocolError,
                     core::Status::Error(core::ErrorCode::InvalidArgument, ec.message()),
                     ec.message()});
        return;
    }
    accepted_ = true;
    accepted_at_ = std::chrono::steady_clock::now();
    try {
        options_.logger.info("event=ws.opened connection_id={} remote_address={}",
                             connection_id(), connection().remote_address);
    } catch (...) {
        // 日志失败不影响已经完成的 WS 握手。
    }
    if (callbacks_.accept_handler) {
        callbacks_.accept_handler(*this);
    }
    lease_.Touch();
    DoReadSome();
}

void WebSocketSession::DoReadSome() {
    if (closing_) {
        return;
    }

    const auto max_frame_bytes = options_.websocket.max_frame_bytes;
    const auto max_message_bytes = options_.websocket.max_message_bytes;
    if (max_frame_bytes == 0 || max_message_bytes == 0) {
        DoClose({ConnectionCloseReason::InternalError,
                 core::Status::Error(core::ErrorCode::InvalidArgument, "websocket read limits must be positive"),
                 "websocket read limits must be positive"});
        return;
    }

    if (options_.websocket.idle_timeout.count() > 0) {
        stream_.next_layer().expires_after(options_.websocket.idle_timeout);
    } else {
        stream_.next_layer().expires_never();
    }

    const auto remaining_message_capacity =
        current_message_bytes_ < max_message_bytes ? max_message_bytes - current_message_bytes_ : std::size_t{0};
    if (read_capacity_ == 0) {
        read_capacity_ = InitialReadCapacity();
    }
    const auto read_capacity = discarding_oversized_message_
                                   ? MaxReadCapacity()
                                   : std::min(read_capacity_, std::max<std::size_t>(1, remaining_message_capacity));
    auto buffer_result = SharedBuffer::AllocateCapacity(memory_pool_, read_capacity);
    if (!buffer_result.ok()) {
        DoClose({ConnectionCloseReason::InternalError, buffer_result.status(), buffer_result.status().message()});
        return;
    }

    read_buffer_ = std::move(buffer_result).value();
    stream_.async_read_some(
        asio::buffer(read_buffer_.data(), read_buffer_.capacity()),
        beast::bind_front_handler(&WebSocketSession::OnReadSome, shared_from_this()));
}

void WebSocketSession::OnReadSome(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) {
        NotifyClose(ConnectionCloseInfo::Remote("websocket closed"));
        return;
    }
    if (ec == websocket::error::message_too_big) {
        DispatchReadError(MessageTooLargeStatus(ec.message()), 0, true);
        return;
    }
    if (ec == beast::error::timeout) {
        DoClose(ConnectionCloseInfo::IdleTimeout("websocket idle timeout"));
        return;
    }
    if (ec) {
        NotifyClose({ConnectionCloseReason::ProtocolError,
                     core::Status::Error(core::ErrorCode::InvalidArgument, ec.message()),
                     ec.message()});
        return;
    }

    lease_.Touch();
    received_bytes_ += bytes_transferred;
    if (stream_.is_message_done()) ++received_messages_;

    const auto requested_capacity = read_buffer_.capacity();
    auto resize_status = read_buffer_.resize(bytes_transferred);
    if (!resize_status.ok()) {
        DoClose({ConnectionCloseReason::InternalError, resize_status, resize_status.message()});
        return;
    }

    const auto max_message_bytes = options_.websocket.max_message_bytes;
    const auto final_fragment = stream_.is_message_done();
    ObserveRead(requested_capacity, bytes_transferred, final_fragment);
    const auto next_message_bytes = current_message_bytes_ + bytes_transferred;

    if (discarding_oversized_message_) {
        current_message_bytes_ = next_message_bytes;
        if (final_fragment) {
            current_message_bytes_ = 0;
            current_message_kind_ = WebSocketMessageKind::Binary;
            discarding_oversized_message_ = false;
            inbound_message_assembler_.Reset();
        }
        DoReadSome();
        return;
    }

    if (current_message_bytes_ == 0) {
        current_message_kind_ = WebSocketKindFromStream(stream_);
    }

    if (bytes_transferred > max_message_bytes - current_message_bytes_ ||
        (next_message_bytes == max_message_bytes && !final_fragment)) {
        DispatchReadError(MessageTooLargeStatus(), next_message_bytes, final_fragment);
        if (!final_fragment) {
            discarding_oversized_message_ = true;
            current_message_bytes_ = next_message_bytes;
        } else {
            current_message_bytes_ = 0;
            current_message_kind_ = WebSocketMessageKind::Binary;
        }
        DoReadSome();
        return;
    }

    current_message_bytes_ = next_message_bytes;

    auto append_status = inbound_message_assembler_.AppendFragment(
        current_message_kind_,
        final_fragment,
        false,
        std::move(read_buffer_));
    if (!append_status.ok()) {
        DispatchReadError(append_status, next_message_bytes, final_fragment);
        if (!final_fragment) {
            discarding_oversized_message_ = true;
        }
        current_message_bytes_ = final_fragment ? 0 : next_message_bytes;
        if (final_fragment) {
            current_message_kind_ = WebSocketMessageKind::Binary;
        }
        DoReadSome();
        return;
    }

    if (!final_fragment) {
        DoReadSome();
        return;
    }

    auto message_result = inbound_message_assembler_.TakeMessage();
    current_message_bytes_ = 0;
    current_message_kind_ = WebSocketMessageKind::Binary;
    if (!message_result.ok()) {
        DispatchReadError(message_result.status(), next_message_bytes, true);
        DoReadSome();
        return;
    }

    DispatchMessage(std::move(message_result).value());
    DoReadSome();
}

void WebSocketSession::DoWrite() {
    if (writing_ || closing_) {
        return;
    }

    auto frame_result = outbound_queue_.TryPop();
    if (!frame_result.ok()) {
        return;
    }

    auto frame = std::move(frame_result).value();
    current_write_ = std::move(frame);
    stream_.text(current_write_.kind == WebSocketMessageKind::Text);
    writing_ = true;
    if (options_.websocket.response_timeout.count() > 0) {
        response_timer_.expires_after(options_.websocket.response_timeout);
        response_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (!ec && self->writing_ && !self->closing_) {
                self->DoClose(ConnectionCloseInfo::Timeout("websocket response timeout"));
            }
        });
    }
    stream_.async_write(
        asio::buffer(current_write_.payload.data(), current_write_.payload.size()),
        beast::bind_front_handler(&WebSocketSession::OnWrite, shared_from_this()));
}

void WebSocketSession::OnWrite(beast::error_code ec, std::size_t) {
    beast::error_code timer_ec;
    response_timer_.cancel(timer_ec);
    writing_ = false;
    current_write_.reset();
    if (ec) {
        NotifyClose({ConnectionCloseReason::InternalError,
                     core::Status::Error(core::ErrorCode::InternalError, ec.message()),
                     ec.message()});
        return;
    }
    lease_.Touch();
    DoWrite();
}

void WebSocketSession::DoClose(ConnectionCloseInfo close_info) {
    if (closing_) {
        return;
    }
    closing_ = true;
    stream_.next_layer().expires_never();
    beast::error_code timer_ec;
    response_timer_.cancel(timer_ec);
    NotifyClose(close_info);
    websocket::close_reason reason;
    reason.reason = close_info.detail;
    stream_.async_close(reason, [self = shared_from_this()](beast::error_code) {});
}

void WebSocketSession::NotifyClose(const ConnectionCloseInfo& close_info) {
    if (close_notified_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto enriched = close_info;
    enriched.connection_id = lease_.context().connection_id;
    enriched.target = std::string(request_.target());
    LogClosed(enriched);
    lease_.Close(enriched);
    if (callbacks_.close_handler) {
        callbacks_.close_handler(enriched);
    }
}

void WebSocketSession::OnControl(websocket::frame_type type, beast::string_view payload) {
    lease_.Touch();
    if (type == websocket::frame_type::close) {
        NotifyClose(ConnectionCloseInfo::Remote(std::string(payload)));
    }
}

void WebSocketSession::DispatchMessage(WebSocketMessage message) {
    if (callbacks_.stream_handler) {
        callbacks_.stream_handler(std::make_shared<WebSocketStreamRequest>(shared_from_this(), std::move(message)));
    } else if (callbacks_.message_handler) {
        callbacks_.message_handler(*this, std::move(message));
    }
}

void WebSocketSession::DispatchReadError(core::Status status, std::size_t bytes_transferred, bool final_fragment) {
    inbound_message_assembler_.Reset();
    WebSocketMessage message;
    message.kind = current_message_kind_;
    message.final_fragment = final_fragment;
    message.status = std::move(status);
    message.total_bytes = bytes_transferred;
    DispatchMessage(std::move(message));
}

} // namespace net
