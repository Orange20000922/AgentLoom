#include "backpressure_queue.h"
#include "connection_pool.h"
#include "http_server.h"
#include "http_types.h"
#include "request_interfaces.h"
#include "static_file_handler.h"
#include "websocket_session.h"
#include "websocket_types.h"

#include "memory_pool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(std::string_view name_prefix)
        : path_(std::filesystem::temp_directory_path() /
                (std::string(name_prefix) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(std::rand()))) {
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

TEST(HttpTypesTest, WrapsBeastRequestWithoutRedefiningHttpSemantics) {
    net::BeastHttpRequest request{net::http::verb::post, "/infer", 11};
    request.set(net::http::field::authorization, "Bearer token");
    request.set(net::http::field::content_type, "application/json");
    request.body() = R"({"text":"hello"})";
    request.prepare_payload();

    auto wrapped = net::HttpRequest::FromBeast(std::move(request), net::ConnectionContext{42, "127.0.0.1"});
    EXPECT_EQ(wrapped.message.method(), net::http::verb::post);
    EXPECT_EQ(wrapped.message.target(), "/infer");
    EXPECT_EQ(wrapped.message[net::http::field::authorization], "Bearer token");
    EXPECT_EQ(wrapped.message.body(), R"({"text":"hello"})");
    EXPECT_EQ(wrapped.connection.connection_id, 42u);
}

TEST(HttpTypesTest, BuildsBeastResponseWithoutExtraBodyCopy) {
    auto response = net::HttpResponse::Json(net::http::status::accepted, R"({"ok":true})");
    EXPECT_EQ(response.message.result(), net::http::status::accepted);
    EXPECT_EQ(response.message[net::http::field::content_type], "application/json; charset=utf-8");
    EXPECT_EQ(response.message.body(), R"({"ok":true})");
    EXPECT_EQ(response.message[net::http::field::content_length], "11");
}

TEST(BackpressureQueueTest, RejectsByItemAndByteLimits) {
    net::BackpressureQueue<std::string> queue({2, 8}, [](const std::string& value) {
        return value.size();
    });

    EXPECT_TRUE(queue.TryPush("1234").ok());
    EXPECT_TRUE(queue.TryPush("12").ok());

    auto item_rejected = queue.TryPush("1");
    ASSERT_FALSE(item_rejected.ok());
    EXPECT_EQ(item_rejected.code(), core::ErrorCode::ResourceExhausted);

    auto first = queue.TryPop();
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_EQ(std::move(first).value(), "1234");

    auto byte_rejected = queue.TryPush("1234567");
    ASSERT_FALSE(byte_rejected.ok());
    EXPECT_EQ(byte_rejected.code(), core::ErrorCode::ResourceExhausted);

    auto stats = queue.Stats();
    EXPECT_EQ(stats.queued_items, 1u);
    EXPECT_EQ(stats.pushed_items, 2u);
    EXPECT_EQ(stats.popped_items, 1u);
    EXPECT_EQ(stats.rejected_items, 2u);
}

TEST(ConnectionPoolTest, TracksLeaseLifetimeAndPerProtocolLimits) {
    net::ConnectionPool pool({.max_connections = 2, .max_http_connections = 1, .max_websocket_connections = 1});

    auto first = pool.Acquire(net::ProtocolConnectionKind::Http, net::ConnectionContext{1, "127.0.0.1"});
    ASSERT_TRUE(first.ok()) << first.status().message();

    auto duplicate = pool.Acquire(net::ProtocolConnectionKind::Http, net::ConnectionContext{1, "127.0.0.1"});
    ASSERT_FALSE(duplicate.ok());
    EXPECT_EQ(duplicate.status().code(), core::ErrorCode::AlreadyExists);

    auto http_limit = pool.Acquire(net::ProtocolConnectionKind::Http, net::ConnectionContext{2, "127.0.0.1"});
    ASSERT_FALSE(http_limit.ok());
    EXPECT_EQ(http_limit.status().code(), core::ErrorCode::ResourceExhausted);

    auto ws = pool.Acquire(net::ProtocolConnectionKind::WebSocket, net::ConnectionContext{2, "127.0.0.1"});
    ASSERT_TRUE(ws.ok()) << ws.status().message();

    auto stats = pool.Stats();
    EXPECT_EQ(stats.active_connections, 2u);
    EXPECT_EQ(stats.active_http_connections, 1u);
    EXPECT_EQ(stats.active_websocket_connections, 1u);
    EXPECT_EQ(stats.rejected_connections, 2u);

    std::move(first).value().Close(net::ConnectionCloseInfo::Remote());
    stats = pool.Stats();
    EXPECT_EQ(stats.active_connections, 1u);
    EXPECT_EQ(stats.active_http_connections, 0u);
    EXPECT_EQ(stats.closed_connections, 1u);
}

TEST(ConnectionPoolTest, AllowsHttpLeaseToUpgradeWhenPoolIsAtTotalLimit) {
    net::ConnectionPool pool({.max_connections = 1, .max_websocket_connections = 1});
    auto lease_result = pool.Acquire(net::ProtocolConnectionKind::Http, net::ConnectionContext{10, "127.0.0.1"});
    ASSERT_TRUE(lease_result.ok()) << lease_result.status().message();

    auto lease = std::move(lease_result).value();
    auto status = lease.SetKind(net::ProtocolConnectionKind::WebSocket);
    ASSERT_TRUE(status.ok()) << status.message();

    auto stats = pool.Stats();
    EXPECT_EQ(stats.active_connections, 1u);
    EXPECT_EQ(stats.active_http_connections, 0u);
    EXPECT_EQ(stats.active_websocket_connections, 1u);
}

TEST(SharedBufferTest, RejectsWritesBeyondCapacity) {
    core::BucketMemoryPool pool;
    auto buffer_result = net::SharedBuffer::AllocateCapacity(pool, 8);
    ASSERT_TRUE(buffer_result.ok()) << buffer_result.status().message();

    auto buffer = std::move(buffer_result).value();
    auto write_status = buffer.Write(0, "abcd", 4);
    ASSERT_TRUE(write_status.ok()) << write_status.message();
    EXPECT_EQ(buffer.size(), 4u);
    EXPECT_EQ(buffer.view(), "abcd");

    auto append_status = buffer.Write(4, "ef", 2);
    ASSERT_TRUE(append_status.ok()) << append_status.message();
    EXPECT_EQ(buffer.size(), 6u);
    EXPECT_EQ(buffer.view(), "abcdef");

    auto overflow_status = buffer.Write(7, "xy", 2);
    ASSERT_FALSE(overflow_status.ok());
    EXPECT_EQ(overflow_status.code(), core::ErrorCode::ResourceExhausted);
    EXPECT_EQ(buffer.size(), 6u);

    auto null_status = buffer.Write(0, nullptr, 1);
    ASSERT_FALSE(null_status.ok());
    EXPECT_EQ(null_status.code(), core::ErrorCode::InvalidArgument);
}

TEST(SharedBufferTest, SafeCopyAppliesCallerLimit) {
    core::BucketMemoryPool pool;

    auto copied = net::SharedBuffer::SafeCopy(pool, "1234", 4);
    ASSERT_TRUE(copied.ok()) << copied.status().message();
    EXPECT_EQ(copied.value().view(), "1234");

    auto rejected = net::SharedBuffer::SafeCopy(pool, "12345", 4);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), core::ErrorCode::ResourceExhausted);
}

TEST(StaticFileHandlerTest, ResolvesIndexAndRejectsTraversal) {
    net::StaticFileHandler handler({std::filesystem::current_path()});

    auto index = handler.ResolveTarget("/");
    ASSERT_TRUE(index.ok()) << index.status().message();
    EXPECT_EQ(index.value().filename(), "index.html");

    auto traversal = handler.ResolveTarget("/../secret.txt");
    ASSERT_FALSE(traversal.ok());
    EXPECT_EQ(traversal.status().code(), core::ErrorCode::PermissionDenied);
}

TEST(StaticFileHandlerTest, ProvidesCommonMimeTypes) {
    EXPECT_EQ(net::MimeType("index.html"), "text/html; charset=utf-8");
    EXPECT_EQ(net::MimeType("app.js"), "text/javascript; charset=utf-8");
    EXPECT_EQ(net::MimeType("module.mjs"), "text/javascript; charset=utf-8");
    EXPECT_EQ(net::MimeType("style.css"), "text/css; charset=utf-8");
    EXPECT_EQ(net::MimeType("image.svg"), "image/svg+xml");
    EXPECT_EQ(net::MimeType("model.wasm"), "application/wasm");
}

TEST(HttpServerRuntimeTest, HandlesHttpRequestWithRegisteredHandler) {
    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpHandler([](net::HttpRequest request, net::HttpGeneratorCallback respond) {
        EXPECT_EQ(request.message.method(), net::http::verb::get);
        EXPECT_EQ(request.message.target(), "/health");
        respond(net::HttpResponse::Json(net::http::status::ok, R"({"status":"ok"})").message);
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
    stream.connect(endpoints);

    net::BeastHttpRequest request{net::http::verb::get, "/health", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::ok);
    EXPECT_EQ(response.body(), R"({"status":"ok"})");

    server.Stop();
}

TEST(HttpServerRuntimeTest, DispatchesTypedHttpRequestInterface) {
    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    std::promise<void> request_seen;
    auto request_seen_future = request_seen.get_future();

    server.SetHttpRequestHandler([&](std::shared_ptr<net::IHttpRequest> request) {
        EXPECT_EQ(request->message().method(), net::http::verb::post);
        EXPECT_EQ(request->message().target(), "/api/vector/query");
        EXPECT_NE(request->connection().connection_id, 0u);
        EXPECT_NE(&request->memory_pool(), nullptr);
        EXPECT_EQ(request->task_pool(), nullptr);
        request->Respond(net::HttpResponse::Json(net::http::status::ok, R"({"typed":true})").message);
        request_seen.set_value();
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::post, "/api/vector/query", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.body() = R"({"embedding":[1,0,0]})";
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::ok);
    EXPECT_EQ(response.body(), R"({"typed":true})");
    EXPECT_EQ(request_seen_future.wait_for(2s), std::future_status::ready);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream.socket().close(ec);
    for (int i = 0; i < 50 && server.ConnectionStats().active_connections != 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }

    server.Stop();
    EXPECT_EQ(server.ConnectionStats().active_connections, 0u);
}

TEST(HttpServerRuntimeTest, RejectsOversizedBodyDuringParsing) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.request_body_limit = 8;
    net::HttpServer server(options);

    std::atomic<bool> handler_called = false;
    server.SetHttpHandler([&](net::HttpRequest, net::HttpGeneratorCallback) {
        handler_called.store(true, std::memory_order_release);
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::post, "/oversized", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.body() = "123456789";
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::payload_too_large);
    EXPECT_EQ(response.body(), "payload too large");
    EXPECT_FALSE(response.keep_alive());
    EXPECT_FALSE(handler_called.load(std::memory_order_acquire));

    server.Stop();
}

TEST(HttpServerRuntimeTest, AllowsTypedHandlerToRespondAfterReadTimeoutWindow) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.request_timeout = 1s;
    net::HttpServer server(options);

    server.SetHttpRequestHandler([](std::shared_ptr<net::IHttpRequest> request) {
        std::thread([request = std::move(request)]() mutable {
            std::this_thread::sleep_for(1500ms);
            request->Respond(net::HttpResponse::Json(net::http::status::ok, R"({"delayed":true})").message);
        }).detach();
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.expires_after(5s);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::post, "/api/delayed", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.body() = R"({"wait":true})";
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::ok);
    EXPECT_EQ(response.body(), R"({"delayed":true})");

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream.socket().close(ec);
    server.Stop();
}

TEST(HttpServerRuntimeTest, ServesStaticFileWithBeastFileBody) {
    ScopedTempDirectory temp_dir("agent_net_static_test");
    const auto& root = temp_dir.path();
    {
        std::ofstream file(root / "index.html", std::ios::binary);
        file << "<html>ok</html>";
    }

    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetStaticFiles({root});
    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::get, "/", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::ok);
    EXPECT_EQ(response[net::http::field::content_type], "text/html; charset=utf-8");
    EXPECT_EQ(response.body(), "<html>ok</html>");

    server.Stop();
}

TEST(HttpServerRuntimeTest, ServesStaticSpaAndDynamicallyLoadedJavaScriptAssets) {
    ScopedTempDirectory temp_dir("agent_net_spa_test");
    const auto& root = temp_dir.path();
    std::filesystem::create_directories(root / "assets");
    {
        std::ofstream file(root / "index.html", std::ios::binary);
        file << "<!doctype html><html><head>"
                "<link rel=\"stylesheet\" href=\"/assets/app.css\">"
                "</head><body><div id=\"app\"></div>"
                "<script type=\"module\" src=\"/assets/app.js\"></script>"
                "</body></html>";
    }
    {
        std::ofstream file(root / "assets" / "app.js", std::ios::binary);
        file << "document.querySelector('#app').textContent = 'boot';\n"
                "export async function loadDynamic(){ return import('./chunk-view.js?v=42'); }\n";
    }
    {
        std::ofstream file(root / "assets" / "chunk-view.js", std::ios::binary);
        file << "export const viewName = 'dynamic-view';\n";
    }
    {
        std::ofstream file(root / "assets" / "app.css", std::ios::binary);
        file << "#app{color:#123456;}\n";
    }

    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetStaticFiles({root});
    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    auto fetch = [&](std::string target) {
        asio::io_context io;
        tcp::resolver resolver(io);
        beast::tcp_stream stream(io);
        stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

        net::BeastHttpRequest request{net::http::verb::get, target, 11};
        request.set(net::http::field::host, "127.0.0.1");
        request.prepare_payload();
        net::http::write(stream, request);

        beast::flat_buffer buffer;
        net::BeastHttpResponse response;
        net::http::read(stream, buffer, response);

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        stream.socket().close(ec);
        return response;
    };

    auto index = fetch("/");
    EXPECT_EQ(index.result(), net::http::status::ok);
    EXPECT_EQ(index[net::http::field::content_type], "text/html; charset=utf-8");
    EXPECT_NE(index.body().find("type=\"module\" src=\"/assets/app.js\""), std::string::npos);

    auto app_js = fetch("/assets/app.js");
    EXPECT_EQ(app_js.result(), net::http::status::ok);
    EXPECT_EQ(app_js[net::http::field::content_type], "text/javascript; charset=utf-8");
    EXPECT_NE(app_js.body().find("import('./chunk-view.js?v=42')"), std::string::npos);

    auto dynamic_chunk = fetch("/assets/chunk-view.js?v=42");
    EXPECT_EQ(dynamic_chunk.result(), net::http::status::ok);
    EXPECT_EQ(dynamic_chunk[net::http::field::content_type], "text/javascript; charset=utf-8");
    EXPECT_NE(dynamic_chunk.body().find("dynamic-view"), std::string::npos);

    auto css = fetch("/assets/app.css");
    EXPECT_EQ(css.result(), net::http::status::ok);
    EXPECT_EQ(css[net::http::field::content_type], "text/css; charset=utf-8");
    EXPECT_NE(css.body().find("#app{color:#123456;}"), std::string::npos);

    auto route_fallback = fetch("/classroom/session/42");
    EXPECT_EQ(route_fallback.result(), net::http::status::ok);
    EXPECT_EQ(route_fallback[net::http::field::content_type], "text/html; charset=utf-8");
    EXPECT_NE(route_fallback.body().find("<div id=\"app\"></div>"), std::string::npos);

    server.Stop();
}

TEST(HttpServerRuntimeTest, AppliesAccessControllerBeforeHandler) {
    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetAccessController([](const net::BeastHttpRequest&, const net::ConnectionContext&, net::AccessCompletion complete) {
        complete(net::AccessDecision::Deny(403, "blocked"));
    });
    server.SetHttpHandler([](net::HttpRequest, net::HttpGeneratorCallback respond) {
        respond(net::HttpResponse::Text(net::http::status::ok, "unexpected").message);
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::get, "/blocked", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::forbidden);
    EXPECT_EQ(response.body(), "blocked");

    server.Stop();
}

TEST(HttpServerRuntimeTest, FiltersSuspiciousHttpRequestBeforeHandler) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.request_filter.enabled = true;

    net::HttpServer server(options);
    std::atomic_size_t handler_calls{0};
    server.SetHttpHandler([&](net::HttpRequest, net::HttpGeneratorCallback respond) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        respond(net::HttpResponse::Text(net::http::status::ok, "unexpected").message);
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::post, "/login", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.set(net::http::field::content_type, "application/x-www-form-urlencoded");
    request.body() = "name=admin' OR 1=1 --";
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::forbidden);
    EXPECT_NE(response.body().find("security filter"), std::string::npos);
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 0u);

    server.Stop();
}

TEST(HttpServerRuntimeTest, AllowsBrowserAcceptWildcardHeaderThroughFilter) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.request_filter.enabled = true;

    net::HttpServer server(options);
    std::atomic_size_t handler_calls{0};
    server.SetHttpHandler([&](net::HttpRequest, net::HttpGeneratorCallback respond) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        respond(net::HttpResponse::Text(net::http::status::ok, "index").message);
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::get, "/", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.set(net::http::field::accept, "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::ok);
    EXPECT_EQ(response.body(), "index");
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 1u);

    server.Stop();
}

TEST(HttpServerRuntimeTest, FiltersOversizedHeaderBeforeHandler) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.request_filter.enabled = true;
    options.request_filter.max_header_value_bytes = 8;

    net::HttpServer server(options);
    std::atomic_size_t handler_calls{0};
    server.SetHttpHandler([&](net::HttpRequest, net::HttpGeneratorCallback respond) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        respond(net::HttpResponse::Text(net::http::status::ok, "unexpected").message);
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));

    net::BeastHttpRequest request{net::http::verb::get, "/health", 11};
    request.set(net::http::field::host, "127.0.0.1");
    request.set("x-long", "123456789");
    request.prepare_payload();
    net::http::write(stream, request);

    beast::flat_buffer buffer;
    net::BeastHttpResponse response;
    net::http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), net::http::status::request_header_fields_too_large);
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 0u);

    server.Stop();
}

TEST(HttpServerRuntimeTest, UpgradesAndEchoesWebSocketMessage) {
    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    core::BucketMemoryPool response_pool;
    std::promise<void> message_seen;
    auto message_seen_future = message_seen.get_future();

    server.SetWebSocketHandler("/ws", [&](net::WebSocketSessionHandle& session, net::WebSocketMessage message) {
        EXPECT_EQ(message.kind, net::WebSocketMessageKind::Text);
        EXPECT_TRUE(message.final_fragment);
        ASSERT_EQ(message.fragments.size(), 1u);
        EXPECT_EQ(message.fragments[0].view(), "hello");
        auto payload = net::SharedBuffer::Copy(response_pool, "echo:hello");
        ASSERT_TRUE(payload.ok()) << payload.status().message();
        auto status = session.Send(net::WebSocketFrame{net::WebSocketMessageKind::Text, true, false, std::move(payload).value()});
        EXPECT_TRUE(status.ok()) << status.message();
        message_seen.set_value();
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
    asio::connect(ws.next_layer(), endpoints);
    ws.handshake("127.0.0.1", "/ws");
    ws.text(true);
    ws.write(asio::buffer(std::string("hello")));

    beast::flat_buffer buffer;
    ws.read(buffer);
    EXPECT_TRUE(ws.got_text());
    EXPECT_EQ(beast::buffers_to_string(buffer.data()), "echo:hello");

    EXPECT_EQ(message_seen_future.wait_for(2s), std::future_status::ready);

    beast::error_code ec;
    ws.close(beast::websocket::close_code::normal, ec);
    server.Stop();
}

TEST(HttpServerRuntimeTest, DispatchesTypedWebSocketStreamRequestAndTracksConnection) {
    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    core::BucketMemoryPool response_pool;
    std::promise<void> message_seen;
    auto message_seen_future = message_seen.get_future();

    server.SetWebSocketStreamHandler("/stream", [&](std::shared_ptr<net::IWebSocketStreamRequest> request) {
        EXPECT_NE(request->connection().connection_id, 0u);
        EXPECT_EQ(request->handshake_request().target(), "/stream");
        EXPECT_EQ(request->handshake_request()["Cookie"], "agent_auth=ws-token");
        EXPECT_EQ(request->handshake_request()["Authorization"], "Bearer ws-token");
        EXPECT_EQ(request->message().kind, net::WebSocketMessageKind::Text);
        ASSERT_EQ(request->message().fragments.size(), 1u);
        EXPECT_EQ(request->message().fragments.front().view(), "frame");
        EXPECT_NE(&request->memory_pool(), nullptr);

        auto snapshots = server.ConnectionSnapshots();
        ASSERT_EQ(snapshots.size(), 1u);
        EXPECT_EQ(snapshots.front().kind, net::ProtocolConnectionKind::WebSocket);

        auto payload = net::SharedBuffer::Copy(response_pool, "ack");
        ASSERT_TRUE(payload.ok()) << payload.status().message();
        auto status = request->Send(net::WebSocketFrame{net::WebSocketMessageKind::Text, true, false, std::move(payload).value()});
        EXPECT_TRUE(status.ok()) << status.message();
        message_seen.set_value();
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    ws.set_option(beast::websocket::stream_base::decorator([](auto& req) {
        req.set(net::http::field::cookie, "agent_auth=ws-token");
        req.set(net::http::field::authorization, "Bearer ws-token");
    }));
    asio::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.port())));
    ws.handshake("127.0.0.1", "/stream");
    ws.text(true);
    ws.write(asio::buffer(std::string("frame")));

    beast::flat_buffer buffer;
    ws.read(buffer);
    EXPECT_TRUE(ws.got_text());
    EXPECT_EQ(beast::buffers_to_string(buffer.data()), "ack");
    EXPECT_EQ(message_seen_future.wait_for(2s), std::future_status::ready);

    auto stats = server.ConnectionStats();
    EXPECT_EQ(stats.active_connections, 1u);
    EXPECT_EQ(stats.active_websocket_connections, 1u);

    beast::error_code ec;
    ws.close(beast::websocket::close_code::normal, ec);
    server.Stop();
}

TEST(HttpServerRuntimeTest, TunesWebSocketReadCapacityByRouteAndObservedBytes) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.websocket.max_frame_bytes = 32;
    options.websocket.max_message_bytes = 8 * 1024;
    options.websocket_read_buffer_limit = 8 * 1024;
    options.websocket_read_tuning = net::WebSocketReadTuning{
        4,
        16,
        1};
    options.websocket_stream_read_tuning = net::WebSocketReadTuning{
        8,
        32,
        1};

    net::HttpServer server(options);
    std::mutex capacities_mutex;
    std::vector<std::size_t> message_capacities;
    std::vector<std::size_t> stream_capacities;
    std::promise<void> message_done;
    std::promise<void> stream_done;
    auto message_done_future = message_done.get_future();
    auto stream_done_future = stream_done.get_future();

    server.SetWebSocketHandler("/message", [&](net::WebSocketSessionHandle&, net::WebSocketMessage message) {
        EXPECT_TRUE(message.final_fragment);
        ASSERT_GT(message.fragments.size(), 1u);
        {
            std::lock_guard lock(capacities_mutex);
            for (const auto& fragment : message.fragments) {
                message_capacities.push_back(fragment.capacity());
            }
        }
        message_done.set_value();
    });
    server.SetWebSocketStreamHandler("/stream", [&](std::shared_ptr<net::IWebSocketStreamRequest> request) {
        EXPECT_TRUE(request->message().final_fragment);
        ASSERT_GT(request->message().fragments.size(), 1u);
        {
            std::lock_guard lock(capacities_mutex);
            for (const auto& fragment : request->message().fragments) {
                stream_capacities.push_back(fragment.capacity());
            }
        }
        stream_done.set_value();
    });

    ASSERT_TRUE(server.Start().ok());

    auto send_message = [&](std::string_view target, std::future<void>& done) {
        asio::io_context io;
        tcp::resolver resolver(io);
        beast::websocket::stream<tcp::socket> ws(io);
        asio::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.port())));
        ws.handshake("127.0.0.1", std::string(target));
        const std::string payload(4 * 1024, 'x');
        ws.binary(true);
        ws.write(asio::buffer(payload));
        EXPECT_EQ(done.wait_for(2s), std::future_status::ready);
        beast::error_code ec;
        ws.close(beast::websocket::close_code::normal, ec);
        EXPECT_FALSE(ec) << ec.message();
    };

    send_message("/message", message_done_future);
    send_message("/stream", stream_done_future);

    {
        std::lock_guard lock(capacities_mutex);
        ASSERT_FALSE(message_capacities.empty());
        EXPECT_EQ(message_capacities.front(), 4u);
        EXPECT_NE(std::find(message_capacities.begin(), message_capacities.end(), 16u),
                  message_capacities.end());
        EXPECT_TRUE(std::all_of(message_capacities.begin(), message_capacities.end(), [](std::size_t capacity) {
            return capacity <= 16;
        }));

        ASSERT_FALSE(stream_capacities.empty());
        EXPECT_EQ(stream_capacities.front(), 8u);
        EXPECT_NE(std::find(stream_capacities.begin(), stream_capacities.end(), 32u),
                  stream_capacities.end());
        EXPECT_TRUE(std::all_of(stream_capacities.begin(), stream_capacities.end(), [](std::size_t capacity) {
            return capacity <= 32;
        }));
    }

    server.Stop();
}

TEST(HttpServerRuntimeTest, ClosesIdleWebSocketAfterConfiguredTimeout) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.websocket_idle_timeout = 1s;

    net::HttpServer server(options);
    std::promise<net::ConnectionCloseReason> close_seen;
    auto close_seen_future = close_seen.get_future();
    std::atomic_bool close_recorded{false};
    server.SetWebSocketHandler("/ws", [](net::WebSocketSessionHandle&, net::WebSocketMessage) {});
    server.SetWebSocketCloseHandler([&](const net::ConnectionCloseInfo& close_info) {
        if (!close_recorded.exchange(true)) {
            close_seen.set_value(close_info.reason);
        }
    });
    ASSERT_TRUE(server.Start().ok());

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    asio::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.port())));
    ws.handshake("127.0.0.1", "/ws");

    ASSERT_EQ(close_seen_future.wait_for(4s), std::future_status::ready);
    EXPECT_EQ(close_seen_future.get(), net::ConnectionCloseReason::IdleTimeout);

    beast::error_code ec;
    ws.next_layer().close(ec);
    server.Stop();
}

TEST(HttpServerRuntimeTest, ReleasesWebSocketSessionAfterPeerClose) {
    net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    std::promise<std::weak_ptr<net::WebSocketSession>> session_accepted;
    auto session_accepted_future = session_accepted.get_future();
    server.SetWebSocketHandler("/ws", [](net::WebSocketSessionHandle&, net::WebSocketMessage) {});
    server.SetWebSocketAcceptHandler([&](net::WebSocketSessionHandle& session) {
        auto& websocket_session = dynamic_cast<net::WebSocketSession&>(session);
        session_accepted.set_value(websocket_session.weak_from_this());
    });
    ASSERT_TRUE(server.Start().ok());

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    asio::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.port())));
    ws.handshake("127.0.0.1", "/ws");

    ASSERT_EQ(session_accepted_future.wait_for(2s), std::future_status::ready);
    auto session = session_accepted_future.get();
    ASSERT_FALSE(session.expired());

    beast::error_code ec;
    ws.close(beast::websocket::close_code::normal, ec);
    ASSERT_FALSE(ec) << ec.message();

    for (int i = 0; i < 200 && !session.expired(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(session.expired());

    server.Stop();
}

TEST(HttpServerRuntimeTest, AssemblesLargeWebSocketMessageBeforeHandler) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.websocket.max_frame_bytes = 4;
    options.websocket.max_message_bytes = 64;
    options.websocket_read_buffer_limit = 64;

    net::HttpServer server(options);
    core::BucketMemoryPool response_pool;
    std::mutex fragments_mutex;
    std::vector<std::string> fragments;
    std::promise<void> final_seen;
    auto final_seen_future = final_seen.get_future();
    std::atomic_size_t handler_calls{0};

    server.SetWebSocketHandler("/ws", [&](net::WebSocketSessionHandle& session, net::WebSocketMessage message) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        EXPECT_EQ(message.kind, net::WebSocketMessageKind::Text);
        EXPECT_TRUE(message.final_fragment);
        EXPECT_EQ(message.total_bytes, 12u);
        ASSERT_GT(message.fragments.size(), 1u);
        {
            std::lock_guard lock(fragments_mutex);
            for (const auto& fragment : message.fragments) {
                fragments.emplace_back(fragment.view());
            }
        }

        auto response = net::SharedBuffer::Copy(response_pool, "done");
        ASSERT_TRUE(response.ok()) << response.status().message();
        auto status = session.Send(net::WebSocketFrame{
            net::WebSocketMessageKind::Text,
            true,
            false,
            std::move(response).value()});
        EXPECT_TRUE(status.ok()) << status.message();
        final_seen.set_value();
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
    asio::connect(ws.next_layer(), endpoints);
    ws.handshake("127.0.0.1", "/ws");
    ws.text(true);

    const std::string payload = "abcdefghijkl";
    ws.write(asio::buffer(payload));

    beast::flat_buffer buffer;
    ws.read(buffer);
    EXPECT_TRUE(ws.got_text());
    EXPECT_EQ(beast::buffers_to_string(buffer.data()), "done");

    EXPECT_EQ(final_seen_future.wait_for(2s), std::future_status::ready);

    {
        std::lock_guard lock(fragments_mutex);
        ASSERT_GT(fragments.size(), 1u);
        std::string joined;
        for (const auto& fragment : fragments) {
            joined += fragment;
        }
        EXPECT_EQ(joined, payload);
    }
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 1u);

    beast::error_code ec;
    ws.close(beast::websocket::close_code::normal, ec);
    server.Stop();
}

TEST(HttpServerRuntimeTest, KeepsBackToBackWebSocketMessagesSeparated) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.websocket.max_frame_bytes = 4;
    options.websocket.max_message_bytes = 64;
    options.websocket_read_buffer_limit = 64;

    net::HttpServer server(options);
    std::mutex messages_mutex;
    std::vector<std::string> messages;

    server.SetWebSocketHandler("/ws", [&](net::WebSocketSessionHandle& session, net::WebSocketMessage message) {
        EXPECT_TRUE(message.final_fragment);
        std::string joined;
        for (const auto& fragment : message.fragments) {
            joined.append(fragment.view());
        }
        {
            std::lock_guard lock(messages_mutex);
            messages.push_back(joined);
        }

        core::BucketMemoryPool response_pool;
        auto response = net::SharedBuffer::Copy(response_pool, joined);
        ASSERT_TRUE(response.ok()) << response.status().message();
        auto status = session.Send(net::WebSocketFrame{
            net::WebSocketMessageKind::Text,
            true,
            false,
            std::move(response).value()});
        ASSERT_TRUE(status.ok()) << status.message();
    });

    ASSERT_TRUE(server.Start().ok());

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    asio::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.port())));
    ws.handshake("127.0.0.1", "/ws");
    ws.text(true);

    for (const std::string payload : {"first-message", "second-message"}) {
        ws.write(asio::buffer(payload));
        beast::flat_buffer buffer;
        ws.read(buffer);
        EXPECT_EQ(beast::buffers_to_string(buffer.data()), payload);
    }

    {
        std::lock_guard lock(messages_mutex);
        ASSERT_EQ(messages.size(), 2u);
        EXPECT_EQ(messages[0], "first-message");
        EXPECT_EQ(messages[1], "second-message");
    }

    beast::error_code ec;
    ws.close(beast::websocket::close_code::normal, ec);
    server.Stop();
}

TEST(HttpServerRuntimeTest, IsolatesCompleteMessagesAcrossConcurrentConnections) {
    constexpr std::size_t kClientCount = 8;

    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 2;
    options.websocket.max_frame_bytes = 8;
    options.websocket.max_message_bytes = 1024;
    options.websocket_read_buffer_limit = 1024;

    net::HttpServer server(options);
    std::mutex messages_mutex;
    std::vector<std::string> messages;

    server.SetWebSocketHandler("/ws", [&](net::WebSocketSessionHandle& session, net::WebSocketMessage message) {
        EXPECT_TRUE(message.final_fragment);
        std::string joined;
        for (const auto& fragment : message.fragments) {
            joined.append(fragment.view());
        }
        {
            std::lock_guard lock(messages_mutex);
            messages.push_back(joined);
        }

        core::BucketMemoryPool response_pool;
        auto response = net::SharedBuffer::Copy(response_pool, "ack");
        ASSERT_TRUE(response.ok()) << response.status().message();
        auto status = session.Send(net::WebSocketFrame{
            net::WebSocketMessageKind::Text,
            true,
            false,
            std::move(response).value()});
        ASSERT_TRUE(status.ok()) << status.message();
    });

    ASSERT_TRUE(server.Start().ok());

    std::vector<std::jthread> clients;
    clients.reserve(kClientCount);
    for (std::size_t index = 0; index < kClientCount; ++index) {
        clients.emplace_back([&, index] {
            asio::io_context io;
            tcp::resolver resolver(io);
            beast::websocket::stream<tcp::socket> ws(io);
            asio::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.port())));
            ws.handshake("127.0.0.1", "/ws");
            ws.text(true);

            const auto payload = "connection-" + std::to_string(index) + ":" + std::string(256, static_cast<char>('a' + index));
            ws.write(asio::buffer(payload));
            beast::flat_buffer buffer;
            ws.read(buffer);
            EXPECT_EQ(beast::buffers_to_string(buffer.data()), "ack");

            beast::error_code ec;
            ws.close(beast::websocket::close_code::normal, ec);
        });
    }
    clients.clear();

    std::vector<std::string> expected;
    expected.reserve(kClientCount);
    for (std::size_t index = 0; index < kClientCount; ++index) {
        expected.push_back(
            "connection-" + std::to_string(index) + ":" +
            std::string(256, static_cast<char>('a' + index)));
    }

    {
        std::lock_guard lock(messages_mutex);
        ASSERT_EQ(messages.size(), kClientCount);
        std::sort(messages.begin(), messages.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(messages, expected);
    }

    server.Stop();
}

TEST(HttpServerRuntimeTest, ReportsOversizedWebSocketMessageWithoutClosingConnection) {
    net::HttpServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;
    options.websocket.max_frame_bytes = 4;
    options.websocket.max_message_bytes = 8;
    options.websocket_read_buffer_limit = 64;

    net::HttpServer server(options);
    core::BucketMemoryPool response_pool;
    std::atomic_size_t handler_calls{0};
    std::atomic_size_t read_errors{0};
    std::atomic_bool close_recorded{false};
    std::promise<void> read_error_seen;
    auto read_error_seen_future = read_error_seen.get_future();
    std::promise<void> next_message_seen;
    auto next_message_seen_future = next_message_seen.get_future();

    server.SetWebSocketHandler("/ws", [&](net::WebSocketSessionHandle& session, net::WebSocketMessage message) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        if (!message.ok()) {
            EXPECT_EQ(message.status.code(), core::ErrorCode::ResourceExhausted);
            if (read_errors.fetch_add(1, std::memory_order_relaxed) == 0) {
                read_error_seen.set_value();
            }
            return;
        }

        EXPECT_TRUE(message.final_fragment);
        ASSERT_EQ(message.fragments.size(), 1u);
        EXPECT_EQ(message.fragments.front().view(), "ok");
        auto payload = net::SharedBuffer::Copy(response_pool, "after-error");
        ASSERT_TRUE(payload.ok()) << payload.status().message();
        auto status = session.Send(net::WebSocketFrame{net::WebSocketMessageKind::Text, true, false, std::move(payload).value()});
        EXPECT_TRUE(status.ok()) << status.message();
        next_message_seen.set_value();
    });
    server.SetWebSocketCloseHandler([&](const net::ConnectionCloseInfo& close_info) {
        if (!close_recorded.exchange(true)) {
            EXPECT_NE(close_info.reason, net::ConnectionCloseReason::BackpressureLimit);
            EXPECT_GT(close_info.connection_id, 0u);
            EXPECT_EQ(close_info.target, "/ws");
        }
    });

    auto start_status = server.Start();
    ASSERT_TRUE(start_status.ok()) << start_status.message();

    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
    asio::connect(ws.next_layer(), endpoints);
    ws.handshake("127.0.0.1", "/ws");
    ws.text(true);

    beast::error_code ec;
    ws.write(asio::buffer(std::string("123456789")), ec);
    ASSERT_FALSE(ec) << ec.message();

    ASSERT_EQ(read_error_seen_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(read_errors.load(std::memory_order_relaxed), 1u);
    EXPECT_FALSE(close_recorded.load(std::memory_order_relaxed));

    ws.write(asio::buffer(std::string("ok")), ec);
    ASSERT_FALSE(ec) << ec.message();

    beast::flat_buffer buffer;
    ws.read(buffer, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(ws.got_text());
    EXPECT_EQ(beast::buffers_to_string(buffer.data()), "after-error");

    EXPECT_EQ(next_message_seen_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 2u);

    ws.close(beast::websocket::close_code::normal, ec);
    server.Stop();
}

TEST(WebSocketTypesTest, UsesBeastCompressionOptions) {
    net::WebSocketOptions options;
    options.enable_compression = true;
    options.compression_min_bytes = 4096;

    auto server_option = options.CompressionOptions(true);
    EXPECT_TRUE(server_option.server_enable);
    EXPECT_FALSE(server_option.client_enable);
    EXPECT_TRUE(server_option.server_no_context_takeover);
    EXPECT_TRUE(server_option.client_no_context_takeover);
    EXPECT_EQ(server_option.msg_size_threshold, 4096u);

    auto client_option = options.CompressionOptions(false);
    EXPECT_FALSE(client_option.server_enable);
    EXPECT_TRUE(client_option.client_enable);
}

TEST(WebSocketTypesTest, AssemblesFragmentedMessageWithoutCoalescingBuffers) {
    core::BucketMemoryPool pool;
    net::WebSocketMessageAssembler assembler({64, 1024});

    auto first_payload = net::SharedBuffer::Copy(pool, "hello ");
    ASSERT_TRUE(first_payload.ok()) << first_payload.status().message();
    auto second_payload = net::SharedBuffer::Copy(pool, "world");
    ASSERT_TRUE(second_payload.ok()) << second_payload.status().message();

    auto first_status = assembler.AppendFrame(
        net::WebSocketFrame{net::WebSocketMessageKind::Text, false, false, std::move(first_payload).value()});
    ASSERT_TRUE(first_status.ok()) << first_status.message();
    EXPECT_FALSE(assembler.complete());

    auto second_status = assembler.AppendFrame(
        net::WebSocketFrame{net::WebSocketMessageKind::Text, true, false, std::move(second_payload).value()});
    ASSERT_TRUE(second_status.ok()) << second_status.message();
    ASSERT_TRUE(assembler.complete());

    auto message_result = assembler.TakeMessage();
    ASSERT_TRUE(message_result.ok()) << message_result.status().message();

    auto message = std::move(message_result).value();
    EXPECT_EQ(message.kind, net::WebSocketMessageKind::Text);
    EXPECT_TRUE(message.final_fragment);
    EXPECT_TRUE(message.fragmented());
    EXPECT_EQ(message.fragment_count(), 2u);
    EXPECT_EQ(message.total_bytes, 11u);
    EXPECT_EQ(message.fragments[0].view(), "hello ");
    EXPECT_EQ(message.fragments[1].view(), "world");
}

TEST(WebSocketTypesTest, AssemblesInboundReadFragmentsWithoutCopying) {
    core::BucketMemoryPool pool;
    net::WebSocketMessageAssembler assembler({64, 1024});

    auto first_payload = net::SharedBuffer::Copy(pool, "read ");
    ASSERT_TRUE(first_payload.ok()) << first_payload.status().message();
    auto second_payload = net::SharedBuffer::Copy(pool, "complete");
    ASSERT_TRUE(second_payload.ok()) << second_payload.status().message();

    auto first_status = assembler.AppendFragment(
        net::WebSocketMessageKind::Text,
        false,
        false,
        std::move(first_payload).value());
    ASSERT_TRUE(first_status.ok()) << first_status.message();
    EXPECT_FALSE(assembler.complete());

    auto second_status = assembler.AppendFragment(
        net::WebSocketMessageKind::Text,
        true,
        false,
        std::move(second_payload).value());
    ASSERT_TRUE(second_status.ok()) << second_status.message();

    auto message_result = assembler.TakeMessage();
    ASSERT_TRUE(message_result.ok()) << message_result.status().message();
    auto message = std::move(message_result).value();
    ASSERT_EQ(message.fragments.size(), 2u);
    EXPECT_EQ(message.total_bytes, 13u);
    EXPECT_EQ(message.fragments[0].view(), "read ");
    EXPECT_EQ(message.fragments[1].view(), "complete");
}

TEST(WebSocketTypesTest, RejectsMessagesAboveConfiguredLimit) {
    core::BucketMemoryPool pool;
    net::WebSocketMessageAssembler assembler({64, 8});

    auto payload = net::SharedBuffer::Copy(pool, "too-large");
    ASSERT_TRUE(payload.ok()) << payload.status().message();

    auto status = assembler.AppendFrame(
        net::WebSocketFrame{net::WebSocketMessageKind::Binary, true, false, std::move(payload).value()});
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), core::ErrorCode::ResourceExhausted);
}

TEST(WebSocketTypesTest, OutboundQueueAppliesBackpressureToPooledFrames) {
    core::BucketMemoryPool pool;
    net::WebSocketOptions options;
    options.outbound_max_items = 2;
    options.outbound_max_bytes = 8;
    auto queue = net::MakeWebSocketOutboundQueue(options);

    auto first = net::SharedBuffer::Copy(pool, "1234");
    ASSERT_TRUE(first.ok()) << first.status().message();
    auto second = net::SharedBuffer::Copy(pool, "12");
    ASSERT_TRUE(second.ok()) << second.status().message();
    auto third = net::SharedBuffer::Copy(pool, "1");
    ASSERT_TRUE(third.ok()) << third.status().message();

    EXPECT_TRUE(queue.TryPush(net::WebSocketFrame{net::WebSocketMessageKind::Binary, true, false, std::move(first).value()}).ok());
    EXPECT_TRUE(queue.TryPush(net::WebSocketFrame{net::WebSocketMessageKind::Binary, true, false, std::move(second).value()}).ok());
    auto rejected = queue.TryPush(net::WebSocketFrame{net::WebSocketMessageKind::Binary, true, false, std::move(third).value()});
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.code(), core::ErrorCode::ResourceExhausted);

    auto stats = queue.Stats();
    EXPECT_EQ(stats.queued_items, 2u);
    EXPECT_EQ(stats.queued_bytes, 6u);
    EXPECT_EQ(stats.rejected_items, 1u);
}

} // namespace
