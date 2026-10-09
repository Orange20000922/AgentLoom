#include "http_server.h"
#include "text_validation.h"

namespace net {

core::Result<std::shared_ptr<IServerEventStream>> HttpServer::HttpSession::BeginEventStream(
    SseStreamOptions options, EventStreamCloseCallback callback) {
    if (!options.outbound.max_items || !options.outbound.max_bytes || !options.max_event_bytes ||
        options.heartbeat_interval.count() <= 0 || options.idle_timeout <= options.heartbeat_interval ||
        options.max_duration.count() <= 0 || options.write_timeout.count() <= 0 || options.reconnect_delay_ms < 0)
        return core::Status::Error(core::ErrorCode::InvalidArgument, "invalid SSE stream limits or timeouts");
    if (response_started_.exchange(true))
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "HTTP response already started");
    sse_options_ = options;
    sse_close_callback_ = std::move(callback);
    sse_connection_id_ = lease_.connection_id();
    sse_queue_ = std::make_unique<BackpressureQueue<SharedBuffer>>(
        options.outbound, [](const SharedBuffer& buffer) { return buffer.size(); });
    auto self = shared_from_this();
    {
        std::lock_guard lock(server_.handler_mutex_);
        // 每次注册顺便回收失效弱引用，避免大量短 Turn 使连接索引无限增长。
        std::erase_if(server_.event_streams_, [](const auto& entry) { return entry.expired(); });
        server_.event_streams_.push_back(self);
    }
    asio::co_spawn(stream_.get_executor(), WriteEvents(), [self](std::exception_ptr error) {
        if (error) self->Close({ConnectionCloseReason::InternalError,
                               core::Status::Error(core::ErrorCode::InternalError, "SSE write coroutine failed"), {}});
    });
    asio::co_spawn(stream_.get_executor(), WatchEventPeer(), [self](std::exception_ptr error) {
        if (error) self->Close({ConnectionCloseReason::InternalError,
                               core::Status::Error(core::ErrorCode::InternalError, "SSE peer coroutine failed"), {}});
    });
    return std::static_pointer_cast<IServerEventStream>(self);
}

core::Status HttpServer::HttpSession::SendEvent(ServerSentEvent event) {
    if (!sse_queue_ || sse_closed_.load() || sse_finished_.load())
        return core::Status::Error(core::ErrorCode::Cancelled, "SSE stream is closed");
    if (event.event.find_first_of("\r\n") != std::string::npos ||
        event.id.find_first_of("\r\n") != std::string::npos || event.id.find('\0') != std::string::npos ||
        event.data.find('\r') != std::string::npos || !core::IsValidUtf8(event.data))
        return core::Status::Error(core::ErrorCode::InvalidArgument, "invalid SSE event fields");
    if (event.data.size() + event.id.size() + event.event.size() > sse_options_.max_event_bytes)
        return core::Status::Error(core::ErrorCode::ResourceExhausted, "SSE event exceeds limit");
    std::string encoded;
    if (!event.id.empty()) encoded += "id: " + event.id + "\n";
    if (!event.event.empty()) encoded += "event: " + event.event + "\n";
    std::size_t offset = 0;
    do {
        const auto end = event.data.find('\n', offset);
        encoded += "data: " + event.data.substr(offset, end == std::string::npos ? end : end - offset) + "\n";
        if (end == std::string::npos) break;
        offset = end + 1;
    } while (offset <= event.data.size());
    encoded += '\n';
    auto copy = SharedBuffer::SafeCopy(sse_memory_pool_, encoded, sse_options_.max_event_bytes);
    if (!copy.ok()) return copy.status();
    auto status = sse_queue_->TryPush(std::move(copy).value());
    if (!status.ok()) {
        AbortEvents(status);
        return status;
    }
    asio::post(stream_.get_executor(), [self = shared_from_this()] {
        if (!self->sse_options_.require_pong) self->sse_last_activity_ = std::chrono::steady_clock::now();
        self->sse_wakeup_.cancel();
    });
    return core::Status::Ok();
}

void HttpServer::HttpSession::FinishEvents() {
    sse_finished_.store(true);
    if (sse_queue_) sse_queue_->Close();
    asio::post(stream_.get_executor(), [self = shared_from_this()] { self->sse_wakeup_.cancel(); });
}

void HttpServer::HttpSession::AbortEvents(core::Status status) {
    CloseFromRequest({ConnectionCloseReason::ProtocolError, std::move(status), "SSE stream aborted"});
}

core::Status HttpServer::HttpSession::AcknowledgeHeartbeat() {
    if (!sse_queue_ || sse_closed_.load())
        return core::Status::Error(core::ErrorCode::NotFound, "SSE stream is not active");
    asio::post(stream_.get_executor(), [self = shared_from_this()] {
        self->sse_last_activity_ = std::chrono::steady_clock::now();
    });
    return core::Status::Ok();
}

BackpressureStats HttpServer::HttpSession::EventQueueStats() const {
    return sse_queue_ ? sse_queue_->Stats() : BackpressureStats{};
}

void HttpServer::HttpSession::NotifyEventStreamClosed(core::Status status) {
    if (!sse_queue_ || sse_closed_.exchange(true)) return;
    const auto stats = sse_queue_->Stats();
    sse_queue_->Close(true);
    sse_wakeup_.cancel();
    // 关闭时记录含最终事件在内的准确峰值；TurnCompleted 内的快照尚不含自身排队开销。
    server_.options_.logger.info("sse.closed connection_id={} status_code={} peak_items={} peak_bytes={} rejected_items={}",
                                 sse_connection_id_, static_cast<int>(status.code()), stats.peak_queued_items,
                                 stats.peak_queued_bytes, stats.rejected_items);
    auto callback = std::move(sse_close_callback_);
    if (callback) {
        try { callback(std::move(status)); }
        catch (...) { server_.options_.logger.error("SSE close callback failed"); }
    }
}

asio::awaitable<void> HttpServer::HttpSession::WriteEvents() {
    sse_started_at_ = sse_last_activity_ = std::chrono::steady_clock::now();
    http::response<http::empty_body> header{http::status::ok, 11};
    header.set(http::field::content_type, "text/event-stream; charset=utf-8");
    header.set(http::field::cache_control, "no-cache, no-transform");
    header.set("X-Accel-Buffering", "no");
    header.keep_alive(false);
    header.chunked(true);
    http::response_serializer<http::empty_body> serializer{header};
    beast::error_code error;
    stream_.expires_after(sse_options_.write_timeout);
    co_await http::async_write_header(stream_, serializer, asio::redirect_error(asio::use_awaitable, error));
    if (error || sse_closed_.load()) {
        Close({ConnectionCloseReason::RemoteClosed, core::Status::Error(core::ErrorCode::Unavailable,
                                                                       "SSE header write failed"), {}});
        co_return;
    }
    bool initial = true;
    while (!sse_closed_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - sse_started_at_ >= sse_options_.max_duration || now - sse_last_activity_ >= sse_options_.idle_timeout) {
            Close(ConnectionCloseInfo::Timeout("SSE idle or duration timeout"));
            co_return;
        }
        auto queued = sse_queue_->TryPop();
        SharedBuffer chunk;
        if (queued.ok()) {
            chunk = std::move(queued).value();
        } else if (sse_finished_.load()) {
            co_await asio::async_write(stream_, http::make_chunk_last(),
                                      asio::redirect_error(asio::use_awaitable, error));
            NotifyEventStreamClosed(error ? core::Status::Error(core::ErrorCode::Unavailable, "SSE terminal write failed")
                                          : core::Status::Ok());
            Close(ConnectionCloseInfo::Remote("SSE response completed"));
            co_return;
        } else {
            std::string heartbeat;
            if (initial) {
                heartbeat = "retry: " + std::to_string(sse_options_.reconnect_delay_ms) + "\n\n";
                initial = false;
            } else {
                stream_.expires_never();
                sse_wakeup_.expires_after(sse_options_.heartbeat_interval);
                co_await sse_wakeup_.async_wait(asio::redirect_error(asio::use_awaitable, error));
                if (error == asio::error::operation_aborted) continue;
                if (error || sse_closed_.load()) co_return;
                heartbeat = "event: ping\ndata: {\"connectionId\":" + std::to_string(sse_connection_id_) + "}\n\n";
            }
            auto allocated = SharedBuffer::Copy(sse_memory_pool_, heartbeat);
            if (!allocated.ok()) { AbortEvents(allocated.status()); co_return; }
            chunk = std::move(allocated).value();
        }
        stream_.expires_after(sse_options_.write_timeout);
        co_await asio::async_write(stream_, http::make_chunk(asio::buffer(chunk.char_data(), chunk.size())),
                                  asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            Close({ConnectionCloseReason::RemoteClosed, core::Status::Error(core::ErrorCode::Unavailable,
                                                                           "SSE chunk write failed"), {}});
            co_return;
        }
        lease_.Touch();
    }
}

asio::awaitable<void> HttpServer::HttpSession::WatchEventPeer() {
    auto allocated = SharedBuffer::Allocate(sse_memory_pool_, 1);
    if (!allocated.ok()) { AbortEvents(allocated.status()); co_return; }
    auto probe = std::move(allocated).value();
    beast::error_code error;
    // SSE 期间不接纳 HTTP pipeline；独立读探针尽早观察 FIN/RST，不等待下一次心跳写入。
    co_await stream_.socket().async_read_some(asio::buffer(probe.char_data(), 1),
                                            asio::redirect_error(asio::use_awaitable, error));
    if (sse_closed_.load()) co_return;
    Close({ConnectionCloseReason::RemoteClosed,
           core::Status::Error(core::ErrorCode::Cancelled, "SSE client disconnected or sent pipelined data"), {}});
}

}
