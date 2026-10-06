#include "http_server.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

// 捕获真实网络层的日志；条件变量保证测试等到关闭事件写入后再断言。
class WsLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    bool WaitForClose() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 4s, [&] {
            for (const auto& line : lines_) {
                if (line.find("event=ws.closed") != std::string::npos) return true;
            }
            return false;
        });
    }

    std::vector<std::string> Lines() {
        std::lock_guard lock(mutex_);
        return lines_;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        const auto level = spdlog::level::to_string_view(message.level);
        lines_.push_back(std::string(level.data(), level.size()) + " " +
                         std::string(message.payload.data(), message.payload.size()));
        changed_.notify_all();
    }
    void flush_() override {}

private:
    std::condition_variable changed_;
    std::vector<std::string> lines_;
};

struct LoggingServerOptions {
    std::shared_ptr<WsLogSink> sink = std::make_shared<WsLogSink>();
    net::HttpServerOptions options;

    LoggingServerOptions() {
        options.address = "127.0.0.1";
        options.port = 0;
        options.io_threads = 1;
        options.logger = core::LoggerAdapter(std::make_shared<spdlog::logger>("ws-test", sink));
    }
};

void Connect(beast::websocket::stream<tcp::socket>& ws, asio::io_context& io,
             std::uint16_t port) {
    tcp::resolver resolver(io);
    asio::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(port)));
    ws.set_option(beast::websocket::stream_base::decorator([](auto& request) {
        request.set(beast::http::field::authorization, "Bearer SECRET_AUTH");
    }));
    ws.handshake("127.0.0.1", "/ws");
}

// 验证 accept 时已可取 ID，无消息连接也有成对日志；正常超时/停止保持 info。
class WsNetworkLoggingTest : public ::testing::TestWithParam<net::ConnectionCloseReason> {};

TEST_P(WsNetworkLoggingTest, LogsNoMessageConnectionOnceWithStableAcceptId) {
    LoggingServerOptions captured;
    captured.options.websocket_idle_timeout = 1s;
    auto server = std::make_unique<net::HttpServer>(captured.options);
    std::promise<net::ConnectionContext> accepted;
    auto future = accepted.get_future();
    server->SetWebSocketHandler("/ws", [](net::WebSocketSessionHandle&, net::WebSocketMessage) {});
    server->SetWebSocketAcceptHandler([&](net::WebSocketSessionHandle& session) {
        EXPECT_EQ(session.connection_id(), session.connection().connection_id);
        accepted.set_value(session.connection());
    });
    ASSERT_TRUE(server->Start().ok());
    asio::io_context io;
    beast::websocket::stream<tcp::socket> ws(io);
    Connect(ws, io, server->port());
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto context = future.get();
    ASSERT_NE(context.connection_id, 0u);
    ASSERT_FALSE(context.remote_address.empty());

    beast::error_code ec;
    std::string reason;
    if (GetParam() == net::ConnectionCloseReason::RemoteClosed) {
        beast::websocket::close_reason close;
        close.code = beast::websocket::close_code::normal;
        close.reason = "SECRET_CLOSE";
        ws.close(close, ec);
        ASSERT_FALSE(ec) << ec.message();
        reason = "remote_closed";
    } else if (GetParam() == net::ConnectionCloseReason::IdleTimeout) {
        reason = "idle_timeout";
    } else {
        server->Stop();
        server.reset();
        reason = "server_shutdown";
    }
    ASSERT_TRUE(captured.sink->WaitForClose());
    const auto lines = captured.sink->Lines();
    ASSERT_EQ(lines.size(), 2u);
    const auto id = "connection_id=" + std::to_string(context.connection_id);
    EXPECT_NE(lines[0].find("info event=ws.opened " + id), std::string::npos);
    EXPECT_NE(lines[1].find("info event=ws.closed " + id), std::string::npos);
    EXPECT_NE(lines[1].find("remote_address=" + context.remote_address), std::string::npos);
    EXPECT_NE(lines[1].find("messages=0 bytes=0 reason=" + reason), std::string::npos);
    EXPECT_EQ(lines[1].find("SECRET"), std::string::npos);
    ws.next_layer().close(ec);
    if (server) server->Stop();
}

INSTANTIATE_TEST_SUITE_P(ExpectedClose, WsNetworkLoggingTest, ::testing::Values(
    net::ConnectionCloseReason::RemoteClosed, net::ConnectionCloseReason::IdleTimeout,
    net::ConnectionCloseReason::ServerShutdown));

TEST(WsNetworkLogging, CountsMessagesAndBytesWithoutLoggingContent) {
    LoggingServerOptions captured;
    net::HttpServer server(captured.options);
    std::promise<std::uint64_t> accepted;
    auto accepted_future = accepted.get_future();
    std::promise<void> received;
    auto received_future = received.get_future();
    std::uint64_t received_id = 0;
    int messages = 0;
    server.SetWebSocketAcceptHandler([&](net::WebSocketSessionHandle& session) {
        accepted.set_value(session.connection_id());
    });
    server.SetWebSocketStreamHandler("/ws", [&](auto request) {
        received_id = request->connection().connection_id;
        if (request->message().final_fragment && ++messages == 2) received.set_value();
    });
    ASSERT_TRUE(server.Start().ok());
    asio::io_context io;
    beast::websocket::stream<tcp::socket> ws(io);
    Connect(ws, io, server.port());
    ASSERT_EQ(accepted_future.wait_for(2s), std::future_status::ready);
    const auto id = accepted_future.get();
    ws.text(true);
    const std::string message = "SECRET_MESSAGE";
    ws.write(asio::buffer(message));
    ws.write(asio::buffer(message));
    ASSERT_EQ(received_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(received_id, id);
    beast::error_code ec;
    ws.close(beast::websocket::close_code::normal, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(captured.sink->WaitForClose());
    const auto lines = captured.sink->Lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[1].find("messages=2 bytes=" + std::to_string(message.size() * 2)),
              std::string::npos);
    for (const auto& line : lines) {
        EXPECT_EQ(line.find("SECRET"), std::string::npos);
        EXPECT_EQ(line.find('\n'), std::string::npos);
    }
    server.Stop();
}

}
