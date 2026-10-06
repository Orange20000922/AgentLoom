#pragma once

#include "connection_pool.h"
#include "http_types.h"
#include "request_interfaces.h"
#include "shared_buffer.h"
#include "websocket_types.h"

#include "memory_pool.h"
#include "logger_adapter.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <chrono>
#include <memory>

namespace net {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

struct WebSocketSessionOptions {
    std::chrono::seconds request_timeout{30};
    std::size_t read_buffer_limit = 16 * 1024 * 1024;
    WebSocketOptions websocket;
    WebSocketReadTuning read_tuning;
    core::LoggerAdapter logger = core::LoggerAdapter::ForModule("net");
};

struct WebSocketSessionCallbacks {
    WebSocketMessageHandler message_handler;
    IWebSocketStreamHandler stream_handler;
    WebSocketAcceptHandler accept_handler;
    WebSocketCloseHandler close_handler;
};

class WebSocketSession final : public WebSocketSessionHandle,
                               public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(tcp::socket socket,
                     BeastHttpRequest request,
                     ConnectionLease lease,
                     WebSocketSessionOptions options,
                     WebSocketSessionCallbacks callbacks);

    ~WebSocketSession();
    const ConnectionContext& connection() const noexcept override;
    void Run();

    core::Status Send(WebSocketFrame frame) override;
    void Close(ConnectionCloseInfo close_info) override;

private:
    class WebSocketStreamRequest;

    void OnAccept(beast::error_code ec);
    void DoReadSome();
    void OnReadSome(beast::error_code ec, std::size_t bytes_transferred);
    void DoWrite();
    void OnWrite(beast::error_code ec, std::size_t);
    void DoClose(ConnectionCloseInfo close_info);
    void NotifyClose(const ConnectionCloseInfo& close_info);
    void LogClosed(const ConnectionCloseInfo& close_info) const noexcept;
    void OnControl(websocket::frame_type type, beast::string_view payload);
    void DispatchMessage(WebSocketMessage message);
    void DispatchReadError(core::Status status, std::size_t bytes_transferred, bool final_fragment);
    std::size_t InitialReadCapacity() const noexcept;
    std::size_t MaxReadCapacity() const noexcept;
    void ObserveRead(std::size_t requested_capacity,
                     std::size_t bytes_transferred,
                     bool final_fragment);

    websocket::stream<beast::tcp_stream> stream_;
    asio::steady_timer response_timer_;
    BeastHttpRequest request_;
    ConnectionLease lease_;
    WebSocketSessionOptions options_;
    WebSocketSessionCallbacks callbacks_;
    WebSocketOutboundQueue outbound_queue_;
    WebSocketMessageAssembler inbound_message_assembler_;
    core::BucketMemoryPool memory_pool_;
    SharedBuffer read_buffer_;
    std::size_t read_capacity_ = 0;
    std::size_t full_read_streak_ = 0;
    std::size_t current_message_bytes_ = 0;
    WebSocketMessageKind current_message_kind_ = WebSocketMessageKind::Binary;
    WebSocketFrame current_write_;
    bool writing_ = false;
    bool closing_ = false;
    bool discarding_oversized_message_ = false;
    std::atomic<bool> close_notified_{false};
    bool accepted_ = false;
    std::uint64_t received_messages_ = 0;
    std::uint64_t received_bytes_ = 0;
    std::chrono::steady_clock::time_point accepted_at_;
};

} // namespace net
