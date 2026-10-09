#include "async_beast_http_client.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using agent::net::AsyncBeastHttpClient;
using agent::net::AsyncBeastHttpClientOptions;
using agent::net::HttpClientRequest;
using agent::net::HttpClientResponse;
using agent::net::IAsyncHttpOperation;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

namespace {

class AsyncMockHttpServer {
public:
    static std::unique_ptr<AsyncMockHttpServer> Start(std::chrono::milliseconds delay,
                                                      bool keep_alive = false) {
        auto server = std::unique_ptr<AsyncMockHttpServer>(
            new AsyncMockHttpServer(delay, keep_alive));
        server->Accept();
        server->thread_ = std::thread([self = server.get()] { self->io_context_.run(); });
        return server;
    }

    ~AsyncMockHttpServer() {
        beast::error_code ignored;
        acceptor_.close(ignored);
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return port_; }
    std::size_t accepted_connections() const noexcept {
        return accepted_connections_.load(std::memory_order_acquire);
    }

private:
    struct Session : public std::enable_shared_from_this<Session> {
        Session(tcp::socket socket, std::chrono::milliseconds delay, bool keep_alive)
            : socket(std::move(socket)),
              timer(socket.get_executor()),
              delay(delay),
              keep_alive(keep_alive) {}

        void Start() {
            auto self = shared_from_this();
            http::async_read(socket, buffer, request,
                [self](const beast::error_code& error, std::size_t) {
                    if (error) {
                        return;
                    }
                    self->timer.expires_after(self->delay);
                    self->timer.async_wait([self](const beast::error_code& timer_error) {
                        if (timer_error) {
                            return;
                        }
                        self->Write();
                    });
                });
        }

        void Write() {
            response.result(request.target() == "/unavailable"
                ? http::status::service_unavailable
                : http::status::ok);
            response.version(request.version());
            response.set(http::field::content_type, "application/json");
            response.keep_alive(keep_alive && request.keep_alive());
            response.body() = request.body().empty() ? R"({"ok":true})" : request.body();
            response.prepare_payload();

            auto self = shared_from_this();
            http::async_write(socket, response,
                [self](const beast::error_code& error, std::size_t) {
                    if (!error && self->response.keep_alive()) {
                        self->request = {};
                        self->response = {};
                        self->Start();
                        return;
                    }
                    beast::error_code ignored;
                    self->socket.shutdown(tcp::socket::shutdown_both, ignored);
                    self->socket.close(ignored);
                });
        }

        tcp::socket socket;
        asio::steady_timer timer;
        std::chrono::milliseconds delay;
        bool keep_alive = false;
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::response<http::string_body> response;
    };

    AsyncMockHttpServer(std::chrono::milliseconds delay, bool keep_alive)
        : delay_(delay),
          keep_alive_(keep_alive),
          acceptor_(io_context_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()) {}

    void Accept() {
        acceptor_.async_accept([this](const beast::error_code& error, tcp::socket socket) {
            if (error) {
                return;
            }
            accepted_connections_.fetch_add(1, std::memory_order_release);
            std::make_shared<Session>(std::move(socket), delay_, keep_alive_)->Start();
            Accept();
        });
    }

    std::chrono::milliseconds delay_;
    bool keep_alive_ = false;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::uint16_t port_ = 0;
    std::atomic<std::size_t> accepted_connections_{0};
    std::thread thread_;
};

class AsyncTruncatedHttpServer {
public:
    AsyncTruncatedHttpServer()
        : acceptor_(io_context_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()),
          thread_([this] { Serve(); }) {}

    ~AsyncTruncatedHttpServer() {
        beast::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return port_; }

private:
    void Serve() {
        beast::error_code error;
        tcp::socket socket(io_context_);
        acceptor_.accept(socket, error);
        if (error) {
            return;
        }
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request, error);
        if (error) {
            return;
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: 64\r\nConnection: close\r\n\r\n{}";
        asio::write(socket, asio::buffer(response), error);
        socket.shutdown(tcp::socket::shutdown_both, error);
        socket.close(error);
    }

    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::uint16_t port_ = 0;
    std::thread thread_;
};

std::string Url(std::uint16_t port, std::string_view target = "/") {
    return "http://127.0.0.1:" + std::to_string(port) + std::string(target);
}

std::unique_ptr<AsyncBeastHttpClient> CreateClient(std::size_t thread_count = 1) {
    AsyncBeastHttpClientOptions options;
    options.io_thread_count = thread_count;
    auto result = AsyncBeastHttpClient::Create(std::move(options));
    EXPECT_TRUE(result.ok()) << result.status().message();
    return result.ok() ? std::move(result).value() : nullptr;
}

core::Result<HttpClientResponse> ExecuteAndWait(AsyncBeastHttpClient& client,
                                                std::string url) {
    auto completion = std::make_shared<std::promise<core::Result<HttpClientResponse>>>();
    auto future = completion->get_future();
    HttpClientRequest request;
    request.method = "GET";
    request.url = std::move(url);
    auto submitted = client.ExecuteAsync(
        std::move(request),
        [completion](auto result) { completion->set_value(std::move(result)); });
    if (!submitted.ok()) {
        return submitted.status();
    }
    return future.get();
}

}

TEST(AsyncBeastHttpClientTest, RejectsInvalidOptionsAndRequest) {
    AsyncBeastHttpClientOptions options;
    options.io_thread_count = 0;
    auto invalid_client = AsyncBeastHttpClient::Create(std::move(options));
    ASSERT_FALSE(invalid_client.ok());
    EXPECT_EQ(invalid_client.status().code(), core::ErrorCode::InvalidArgument);

    options.io_thread_count = 1;
    options.idle_connection_timeout = std::chrono::milliseconds::zero();
    auto invalid_pool = AsyncBeastHttpClient::Create(std::move(options));
    ASSERT_FALSE(invalid_pool.ok());
    EXPECT_EQ(invalid_pool.status().code(), core::ErrorCode::InvalidArgument);

    auto client = CreateClient();
    ASSERT_NE(client, nullptr);
    HttpClientRequest request;
    request.url = "not-a-url";
    auto invalid_request = client->ExecuteAsync(std::move(request), [](auto) {});
    EXPECT_FALSE(invalid_request.ok());
    EXPECT_EQ(invalid_request.status().code(), core::ErrorCode::InvalidArgument);
}

TEST(AsyncBeastHttpClientTest, RejectsTruncatedContentLengthBody) {
    AsyncTruncatedHttpServer server;
    auto client = CreateClient();
    ASSERT_NE(client, nullptr);

    auto result = ExecuteAndWait(*client, Url(server.port(), "/truncated"));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), core::ErrorCode::DataLoss);
}

TEST(AsyncBeastHttpClientTest, ReusesKeepAliveConnectionForSequentialRequests) {
    auto server = AsyncMockHttpServer::Start(std::chrono::milliseconds(0), true);
    auto client = CreateClient();
    ASSERT_NE(client, nullptr);

    auto first = ExecuteAndWait(*client, Url(server->port(), "/first"));
    auto second = ExecuteAndWait(*client, Url(server->port(), "/second"));

    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(server->accepted_connections(), 1u);
}

TEST(AsyncBeastHttpClientTest, CanDisableKeepAlivePool) {
    auto server = AsyncMockHttpServer::Start(std::chrono::milliseconds(0), true);
    AsyncBeastHttpClientOptions options;
    options.enable_keep_alive = false;
    auto created = AsyncBeastHttpClient::Create(std::move(options));
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto client = std::move(created).value();

    ASSERT_TRUE(ExecuteAndWait(*client, Url(server->port(), "/first")).ok());
    ASSERT_TRUE(ExecuteAndWait(*client, Url(server->port(), "/second")).ok());
    EXPECT_EQ(server->accepted_connections(), 2u);
}

TEST(AsyncBeastHttpClientTest, ExpiresIdleKeepAliveConnection) {
    auto server = AsyncMockHttpServer::Start(std::chrono::milliseconds(0), true);
    AsyncBeastHttpClientOptions options;
    options.idle_connection_timeout = std::chrono::milliseconds(50);
    auto created = AsyncBeastHttpClient::Create(std::move(options));
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto client = std::move(created).value();

    ASSERT_TRUE(ExecuteAndWait(*client, Url(server->port(), "/first")).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_TRUE(ExecuteAndWait(*client, Url(server->port(), "/second")).ok());
    EXPECT_EQ(server->accepted_connections(), 2u);
}

TEST(AsyncBeastHttpClientTest, SubmissionDoesNotWaitForSlowResponse) {
    auto server = AsyncMockHttpServer::Start(std::chrono::milliseconds(300));
    auto client = CreateClient();
    ASSERT_NE(client, nullptr);

    std::promise<core::Result<HttpClientResponse>> completion;
    auto future = completion.get_future();
    HttpClientRequest request;
    request.method = "POST";
    request.url = Url(server->port(), "/echo");
    request.body = R"({"message":"hello"})";

    const auto started = std::chrono::steady_clock::now();
    auto operation = client->ExecuteAsync(
        std::move(request),
        [&completion](auto result) { completion.set_value(std::move(result)); });
    const auto submit_elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(operation.ok()) << operation.status().message();
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(submit_elapsed).count(), 100);
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);

    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().status, 200);
    EXPECT_EQ(result.value().body, R"({"message":"hello"})");
}

TEST(AsyncBeastHttpClientTest, OneIoThreadDrivesConcurrentRequests) {
    constexpr std::size_t request_count = 8;
    auto server = AsyncMockHttpServer::Start(std::chrono::milliseconds(150));
    auto client = CreateClient(1);
    ASSERT_NE(client, nullptr);

    std::mutex mutex;
    std::condition_variable condition;
    std::size_t completed = 0;
    std::size_t succeeded = 0;
    std::vector<std::shared_ptr<IAsyncHttpOperation>> operations;
    operations.reserve(request_count);

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < request_count; ++i) {
        HttpClientRequest request;
        request.method = "GET";
        request.url = Url(server->port());
        auto submitted = client->ExecuteAsync(std::move(request), [&](auto result) {
            std::lock_guard lock(mutex);
            ++completed;
            if (result.ok()) {
                ++succeeded;
            }
            condition.notify_one();
        });
        ASSERT_TRUE(submitted.ok()) << submitted.status().message();
        operations.push_back(std::move(submitted).value());
    }

    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3), [&] {
        return completed == request_count;
    }));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(succeeded, request_count);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 700);
}

TEST(AsyncBeastHttpClientTest, TimeoutCompletesExactlyOnce) {
    auto server = AsyncMockHttpServer::Start(std::chrono::seconds(2));
    auto client = CreateClient();
    ASSERT_NE(client, nullptr);

    std::promise<core::Status> completion;
    auto future = completion.get_future();
    std::atomic<int> callback_count = 0;
    HttpClientRequest request;
    request.url = Url(server->port());
    request.timeout_ms = 100;

    auto operation = client->ExecuteAsync(std::move(request), [&](auto result) {
        ++callback_count;
        completion.set_value(result.ok() ? core::Status::Ok() : result.status());
    });
    ASSERT_TRUE(operation.ok());
    auto status = future.get();
    EXPECT_EQ(status.code(), core::ErrorCode::Timeout);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(callback_count.load(), 1);
}

TEST(AsyncBeastHttpClientTest, CancelIsIdempotentAndCompletesExactlyOnce) {
    auto server = AsyncMockHttpServer::Start(std::chrono::seconds(2));
    auto client = CreateClient();
    ASSERT_NE(client, nullptr);

    std::promise<core::Status> completion;
    auto future = completion.get_future();
    std::atomic<int> callback_count = 0;
    HttpClientRequest request;
    request.url = Url(server->port());

    auto submitted = client->ExecuteAsync(std::move(request), [&](auto result) {
        ++callback_count;
        completion.set_value(result.ok() ? core::Status::Ok() : result.status());
    });
    ASSERT_TRUE(submitted.ok());
    auto operation = std::move(submitted).value();
    operation->Cancel();
    operation->Cancel();

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto status = future.get();
    EXPECT_EQ(status.code(), core::ErrorCode::Cancelled);
    EXPECT_EQ(callback_count.load(), 1);
}

TEST(AsyncBeastHttpClientTest, ShutdownCancelsInflightAndWaitsForCallback) {
    auto server = AsyncMockHttpServer::Start(std::chrono::seconds(2));
    auto client = CreateClient(2);
    ASSERT_NE(client, nullptr);

    std::atomic<bool> callback_finished = false;
    HttpClientRequest request;
    request.url = Url(server->port());
    auto submitted = client->ExecuteAsync(std::move(request), [&](auto result) {
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.status().code(), core::ErrorCode::Cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        callback_finished.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(submitted.ok());

    client->Shutdown();
    EXPECT_TRUE(callback_finished.load(std::memory_order_acquire));

    HttpClientRequest rejected_request;
    rejected_request.url = Url(server->port());
    auto rejected = client->ExecuteAsync(std::move(rejected_request), [](auto) {});
    EXPECT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), core::ErrorCode::Cancelled);
}

TEST(AsyncBeastHttpClientTest, HttpErrorStatusIsSuccessfulTransportResult) {
    auto server = AsyncMockHttpServer::Start(std::chrono::milliseconds(0));
    auto client = CreateClient();
    ASSERT_NE(client, nullptr);

    std::promise<core::Result<HttpClientResponse>> completion;
    auto future = completion.get_future();
    HttpClientRequest request;
    request.url = Url(server->port(), "/unavailable");
    auto submitted = client->ExecuteAsync(
        std::move(request),
        [&completion](auto result) { completion.set_value(std::move(result)); });
    ASSERT_TRUE(submitted.ok());

    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().status, 503);
}

TEST(AsyncBeastHttpClientTest, StreamingAndOrdinaryRequestsShareKeepAliveAndIdleExpiry) {
    auto server = AsyncMockHttpServer::Start(std::chrono::milliseconds(0), true);
    AsyncBeastHttpClientOptions options;
    options.io_thread_count = 2;
    options.idle_connection_timeout = std::chrono::milliseconds(50);
    auto created = AsyncBeastHttpClient::Create(options);
    ASSERT_TRUE(created.ok());
    auto client = std::move(created).value();
    HttpClientRequest request;
    request.url = Url(server->port());
    request.body = "stream-body";
    std::string received;
    std::promise<core::Status> stream_completion;
    auto stream_future = stream_completion.get_future();
    auto submitted = client->ExecuteStreamingAsync(request, {}, {
        .on_headers = [](const auto& headers) { EXPECT_EQ(headers.status, 200); return core::Status::Ok(); },
        .on_body = [&](auto bytes) { received.append(bytes); return core::Status::Ok(); },
        .on_complete = [&](auto status) { stream_completion.set_value(status); },
    });
    ASSERT_TRUE(submitted.ok());
    ASSERT_EQ(stream_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(stream_future.get().ok());
    EXPECT_EQ(received, "stream-body");
    auto ordinary = [&] {
        std::promise<core::Result<HttpClientResponse>> completed;
        auto future = completed.get_future();
        auto operation = client->ExecuteAsync(request, [&](auto result) { completed.set_value(std::move(result)); });
        EXPECT_TRUE(operation.ok());
        if (operation.ok()) EXPECT_TRUE(future.get().ok());
    };
    ordinary();
    EXPECT_EQ(server->accepted_connections(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ordinary();
    EXPECT_EQ(server->accepted_connections(), 2);
}
