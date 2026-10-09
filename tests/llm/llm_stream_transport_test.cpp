#include "async_beast_http_client.h"
#include "openai_llm_client.h"
#include "../../src/net/http_server.h"
#include "../../src/net/sse.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <future>
#include <sstream>
#include <thread>

namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace llm = agent::llm;
namespace outbound = agent::net;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string Delta(std::string text, Json finish = nullptr) {
    return "data: " + Json{{"id", "fake-generation"}, {"model", "fake"},
        {"choices", Json::array({{{"index", 0}, {"delta", {{"content", text}}}, {"finish_reason", finish}}})}}.dump() + "\n\n";
}

std::string WireChunk(const std::string& body) {
    std::ostringstream stream;
    stream << std::hex << body.size() << "\r\n" << body << "\r\n";
    return stream.str();
}

struct WirePart { std::string bytes; std::chrono::milliseconds delay{0}; };

// 测试专用原始 HTTP 服务：可注入坏 chunk，生产 HttpServer 不暴露 Mock 入口。
class RawServer {
public:
    explicit RawServer(std::vector<WirePart> parts) : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
                                                   parts_(std::move(parts)) {
        port_ = acceptor_.local_endpoint().port();
        asio::co_spawn(io_, Serve(), [](std::exception_ptr) {});
        worker_ = std::thread([this] { io_.run(); });
    }
    ~RawServer() { io_.stop(); if (worker_.joinable()) worker_.join(); }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::atomic<int> requests{0};
private:
    asio::awaitable<void> Serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        boost::beast::flat_buffer buffer;
        http::request<http::string_body> request;
        co_await http::async_read(socket, buffer, request, asio::use_awaitable);
        ++requests;
        asio::steady_timer timer(co_await asio::this_coro::executor);
        for (const auto& part : parts_) {
            if (part.delay.count()) {
                timer.expires_after(part.delay);
                co_await timer.async_wait(asio::use_awaitable);
            }
            co_await asio::async_write(socket, asio::buffer(part.bytes), asio::use_awaitable);
        }
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
    }
    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::vector<WirePart> parts_;
    std::thread worker_;
    unsigned short port_ = 0;
};

const std::string kHeaders = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";

struct Clients {
    explicit Clients(std::string url, int timeout = 1000) {
        outbound::AsyncBeastHttpClientOptions http_options;
        http_options.io_thread_count = 3;
        auto created = outbound::AsyncBeastHttpClient::Create(http_options);
        EXPECT_TRUE(created.ok());
        http = std::move(created).value();
        llm::OpenAiLlmClientOptions options;
        options.base_url = std::move(url);
        options.require_api_key = false;
        options.timeout_ms = timeout;
        options.retry_policy.max_retries = 3;
        auto client = llm::OpenAiAsyncLlmClient::Create(options, *http);
        EXPECT_TRUE(client.ok());
        llm = std::move(client).value();
    }
    std::unique_ptr<outbound::AsyncBeastHttpClient> http;
    std::unique_ptr<llm::OpenAiAsyncLlmClient> llm;
};

TEST(LlmStreamTransportTest, EmitsFirstDeltaBeforeProviderEndAcrossRealHttpChunks) {
    const auto body = Delta("中文🙂");
    std::vector<WirePart> parts{{kHeaders}};
    for (char value : body) parts.push_back({WireChunk(std::string(1, value))});
    parts.push_back({WireChunk(Delta(" reply", "stop") + "data: [DONE]\n\n") + "0\r\n\r\n", 120ms});
    RawServer server(std::move(parts));
    Clients clients(server.Url());
    std::promise<void> first;
    auto first_future = first.get_future();
    std::promise<core::Result<llm::ChatCompletionResponse>> complete;
    auto future = complete.get_future();
    std::atomic<int> deltas{0};
    auto submitted = clients.llm->CompleteStreamingAsync({}, [&](const auto& event) {
        if (event.kind == llm::LlmStreamEventKind::TextDelta && ++deltas == 1) first.set_value();
        return core::Status::Ok();
    }, [&](auto result) { complete.set_value(std::move(result)); });
    ASSERT_TRUE(submitted.ok());
    ASSERT_EQ(first_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.wait_for(0ms), std::future_status::timeout);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().content, "中文🙂 reply");
    EXPECT_EQ(deltas.load(), 2);
}

TEST(LlmStreamTransportTest, RejectsMalformedChunkTruncatedHttpAndIncompleteSseWithoutRetry) {
    const std::vector<std::string> broken{
        "xyz\r\nnot-a-chunk\r\n", "20\r\nshort", WireChunk(Delta("partial")) + "0\r\n\r\n",
        WireChunk("data: {broken}\n\n") + "0\r\n\r\n",
    };
    for (const auto& body : broken) {
        RawServer server({{kHeaders + body}});
        Clients clients(server.Url());
        std::promise<core::Status> completion;
        auto future = completion.get_future();
        std::atomic<int> count{0};
        auto result = clients.llm->CompleteStreamingAsync({}, [](const auto&) { return core::Status::Ok(); },
            [&](auto response) { ++count; completion.set_value(response.status()); });
        ASSERT_TRUE(result.ok());
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(future.get().code(), core::ErrorCode::DataLoss);
        clients.http->Shutdown();
        EXPECT_EQ(count.load(), 1);
        EXPECT_EQ(server.requests.load(), 1);
    }
}

TEST(LlmStreamTransportTest, ValidatesStatusAndContentTypeBeforeAnyDelta) {
    for (const int status : {200, 401, 403, 429, 503}) {
        RawServer server({{"HTTP/1.1 " + std::to_string(status) + " Error\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"}});
        Clients clients(server.Url());
        std::promise<core::Status> completion;
        auto future = completion.get_future();
        std::atomic<int> events{0};
        auto submitted = clients.llm->CompleteStreamingAsync({}, [&](const auto&) { ++events; return core::Status::Ok(); },
            [&](auto response) { completion.set_value(response.status()); });
        ASSERT_TRUE(submitted.ok());
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        const auto code = future.get().code();
        EXPECT_EQ(code, status == 200 ? core::ErrorCode::DataLoss : status == 429 ? core::ErrorCode::ResourceExhausted :
                       status == 503 ? core::ErrorCode::Unavailable : core::ErrorCode::PermissionDenied);
        EXPECT_EQ(events.load(), 0);
        EXPECT_EQ(server.requests.load(), 1);
    }
}

TEST(LlmStreamTransportTest, CancellationDeadlineAndShutdownCompleteExactlyOnce) {
    for (int round = 0; round < 12; ++round) {
        RawServer server({{kHeaders + WireChunk(Delta("first"))},
                          {WireChunk(Delta("last", "stop") + "data: [DONE]\n\n") + "0\r\n\r\n", 200ms}});
        Clients clients(server.Url(), round % 3 == 0 ? 15 : 1000);
        std::promise<void> first;
        auto first_future = first.get_future();
        std::promise<core::Status> completion;
        auto future = completion.get_future();
        std::atomic<int> count{0};
        auto submitted = clients.llm->CompleteStreamingAsync({}, [&](const auto& event) {
            if (event.kind == llm::LlmStreamEventKind::TextDelta) first.set_value();
            return core::Status::Ok();
        }, [&](auto response) { if (++count == 1) completion.set_value(response.status()); });
        ASSERT_TRUE(submitted.ok());
        ASSERT_EQ(first_future.wait_for(2s), std::future_status::ready);
        if (round % 3 == 1) { submitted.value()->Cancel(); submitted.value()->Cancel(); }
        if (round % 3 == 2) clients.llm->Shutdown();
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(future.get().code(), round % 3 == 0 ? core::ErrorCode::Timeout : core::ErrorCode::Cancelled);
        clients.http->Shutdown();
        EXPECT_EQ(count.load(), 1);
    }
}

TEST(LlmStreamTransportTest, HttpIdleTimeoutAndSinkBackpressurePropagate) {
    for (const bool reject : {false, true}) {
        RawServer server({{kHeaders + WireChunk(Delta("first"))}, {"0\r\n\r\n", 200ms}});
        Clients clients(server.Url());
        outbound::HttpClientRequest request;
        request.url = server.Url();
        outbound::HttpStreamOptions options;
        options.idle_timeout = 15ms;
        std::promise<core::Status> completion;
        auto future = completion.get_future();
        auto submitted = clients.http->ExecuteStreamingAsync(request, options, {
            .on_headers = [](const auto&) { return core::Status::Ok(); },
            .on_body = [reject](auto) { return reject ? core::Status::Error(core::ErrorCode::ResourceExhausted, "queue full")
                                                    : core::Status::Ok(); },
            .on_complete = [&](auto status) { completion.set_value(status); },
        });
        ASSERT_TRUE(submitted.ok());
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(future.get().code(), reject ? core::ErrorCode::ResourceExhausted : core::ErrorCode::Timeout);
    }
}

TEST(LlmStreamTransportTest, CoroutineSseServerSendsTypedEventsAndObservesDisconnect) {
    ::net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 2;
    ::net::HttpServer server(options);
    std::promise<core::Status> closed;
    auto closed_future = closed.get_future();
    server.SetHttpRequestHandler([&](auto request) {
        auto stream = request->BeginEventStream({}, [&](auto status) { closed.set_value(status); });
        ASSERT_TRUE(stream.ok());
        ASSERT_TRUE(stream.value()->SendEvent({"TextDelta", "first\nsecond", "turn:1"}).ok());
    });
    ASSERT_TRUE(server.Start().ok());
    Clients clients("http://127.0.0.1:" + std::to_string(server.port()));
    std::promise<void> received;
    auto received_future = received.get_future();
    std::atomic<bool> saw{false};
    agent::net::SseDecoder decoder([&](const auto& event) {
        if (event.event == "TextDelta" && !saw.exchange(true)) {
            EXPECT_EQ(event.data, "first\nsecond");
            EXPECT_EQ(event.id, "turn:1");
            received.set_value();
        }
        return core::Status::Ok();
    });
    std::promise<core::Status> completion;
    outbound::HttpClientRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port());
    auto submitted = clients.http->ExecuteStreamingAsync(request, {}, {
        .on_headers = [](const auto& response) { EXPECT_EQ(response.status, 200); return core::Status::Ok(); },
        .on_body = [&](auto bytes) {
            return decoder.Feed(bytes);
        },
        .on_complete = [&](auto status) { completion.set_value(status); },
    });
    ASSERT_TRUE(submitted.ok());
    ASSERT_EQ(received_future.wait_for(2s), std::future_status::ready);
    submitted.value()->Cancel();
    ASSERT_EQ(closed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(closed_future.get().code(), core::ErrorCode::Cancelled);
    server.Stop();
}

TEST(LlmStreamTransportTest, ServerBackpressureIsBoundedAndCloseReportsResourceExhaustedOnce) {
    ::net::HttpServerOptions options;
    options.address = "127.0.0.1"; options.port = 0;
    ::net::HttpServer server(options);
    std::promise<core::Status> closed;
    auto future = closed.get_future();
    std::atomic<int> callbacks{0};
    server.SetHttpRequestHandler([&](auto request) {
        ::net::SseStreamOptions limits;
        limits.outbound = {1, 4096};
        auto opened = request->BeginEventStream(limits, [&](auto status) {
            if (++callbacks == 1) closed.set_value(status);
        });
        ASSERT_TRUE(opened.ok());
        ASSERT_TRUE(opened.value()->SendEvent({"TextDelta", "first", "1"}).ok());
        EXPECT_EQ(opened.value()->SendEvent({"TextDelta", "second", "2"}).code(), core::ErrorCode::ResourceExhausted);
        const auto stats = opened.value()->EventQueueStats();
        EXPECT_EQ(stats.peak_queued_items, 1);
        EXPECT_LE(stats.peak_queued_bytes, 4096);
    });
    ASSERT_TRUE(server.Start().ok());
    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(socket, asio::buffer(request));
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get().code(), core::ErrorCode::ResourceExhausted);
    server.Stop();
    EXPECT_EQ(callbacks.load(), 1);
}

TEST(LlmStreamTransportTest, ServerHeartbeatPongAndIdleTimeoutDoNotExtendIdleOnPing) {
    ::net::HttpServerOptions options;
    options.address = "127.0.0.1"; options.port = 0;
    ::net::HttpServer server(options);
    std::promise<std::shared_ptr<::net::IServerEventStream>> opened;
    auto opened_future = opened.get_future();
    std::promise<core::Status> closed;
    auto closed_future = closed.get_future();
    server.SetHttpRequestHandler([&](auto request) {
        ::net::SseStreamOptions limits;
        limits.require_pong = true;
        limits.heartbeat_interval = 10ms;
        limits.idle_timeout = 100ms;
        auto stream = request->BeginEventStream(limits, [&](auto status) { closed.set_value(status); });
        ASSERT_TRUE(stream.ok());
        opened.set_value(stream.value());
    });
    ASSERT_TRUE(server.Start().ok());
    Clients clients("http://127.0.0.1:" + std::to_string(server.port()));
    outbound::HttpClientRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port());
    std::promise<core::Status> complete;
    auto completed = complete.get_future();
    std::atomic<int> pings{0};
    std::shared_ptr<::net::IServerEventStream> stream;
    outbound::SseDecoder decoder([&](const auto& event) {
        if (event.event == "ping") {
            const auto count = ++pings;
            if (count <= 3) EXPECT_TRUE(stream->AcknowledgeHeartbeat().ok());
        }
        return core::Status::Ok();
    });
    auto submitted = clients.http->ExecuteStreamingAsync(request, {}, {
        .on_headers = [](const auto&) { return core::Status::Ok(); },
        .on_body = [&](auto bytes) {
            if (!stream) stream = opened_future.get();
            return decoder.Feed(bytes);
        },
        .on_complete = [&](auto status) { complete.set_value(status); },
    });
    ASSERT_TRUE(submitted.ok());
    ASSERT_EQ(closed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(closed_future.get().code(), core::ErrorCode::Timeout);
    EXPECT_GT(pings.load(), 3);
    ASSERT_EQ(completed.wait_for(2s), std::future_status::ready);
    server.Stop();
}

TEST(LlmStreamTransportTest, ServerShutdownNotifiesSseConsumerOnce) {
    ::net::HttpServerOptions options;
    options.address = "127.0.0.1"; options.port = 0;
    ::net::HttpServer server(options);
    std::promise<void> opened;
    auto opened_future = opened.get_future();
    std::promise<core::Status> closed;
    auto closed_future = closed.get_future();
    std::atomic<int> callbacks{0};
    server.SetHttpRequestHandler([&](auto request) {
        auto stream = request->BeginEventStream({}, [&](auto status) {
            if (++callbacks == 1) closed.set_value(status);
        });
        ASSERT_TRUE(stream.ok());
        opened.set_value();
    });
    ASSERT_TRUE(server.Start().ok());
    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(socket, asio::buffer(request));
    ASSERT_EQ(opened_future.wait_for(2s), std::future_status::ready);
    server.Stop();
    ASSERT_EQ(closed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(closed_future.get().code(), core::ErrorCode::Cancelled);
    EXPECT_EQ(callbacks.load(), 1);
}
}
