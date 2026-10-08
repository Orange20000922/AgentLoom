// End-to-end integration test for the LLM stack.
//
// Drives the full chain from config file parsing through prompt store loading,
// API key resolution, OpenAI client construction, and live HTTP round-trip
// against a mock OpenAI-compatible server.  Validates that all the pieces wire
// together correctly without any production secrets or network access.

#include "../../src/config/option_parser.h"
#include "../../src/config/server_options.h"
#include "../../src/llm/openai_llm_client.h"
#include "../../src/net/http_client/beast_http_client.h"
#include "../../src/net/http_client/async_beast_http_client.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using agent::llm::ChatCompletionRequest;
using agent::llm::ChatRole;
using agent::llm::LlmPromptStore;
using agent::llm::OpenAiLlmClient;
using agent::llm::OpenAiLlmClientOptions;
using agent::net::BeastHttpClient;
using agent::net::BeastHttpClientOptions;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

namespace {

struct MockBehavior {
    int status = 200;
    std::string body;
    std::atomic<int> call_count{0};
    std::string last_auth_header;
    std::string last_body;
    std::string last_target;
};

class MockOpenAiServer {
public:
    static std::unique_ptr<MockOpenAiServer> Start(std::shared_ptr<MockBehavior> behavior) {
        auto self = std::unique_ptr<MockOpenAiServer>(new MockOpenAiServer(std::move(behavior)));
        self->Run();
        return self;
    }

    ~MockOpenAiServer() {
        acceptor_.close();
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::uint16_t port() const { return port_; }

private:
    explicit MockOpenAiServer(std::shared_ptr<MockBehavior> behavior)
        : behavior_(std::move(behavior)),
          acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
        port_ = acceptor_.local_endpoint().port();
    }

    void Run() {
        DoAccept();
        thread_ = std::thread([this] { ioc_.run(); });
    }

    void DoAccept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket sock) {
            if (ec) return;
            HandleSession(std::move(sock));
            DoAccept();
        });
    }

    void HandleSession(tcp::socket sock) {
        std::thread([this, sock = std::move(sock)]() mutable {
            beast::error_code ec;
            beast::flat_buffer buf;
            http::request<http::string_body> req;
            http::read(sock, buf, req, ec);
            if (ec) return;

            behavior_->call_count.fetch_add(1);
            behavior_->last_target = std::string(req.target());
            behavior_->last_body = req.body();
            if (req.count(http::field::authorization)) {
                behavior_->last_auth_header = std::string(req[http::field::authorization]);
            }

            http::response<http::string_body> res(
                static_cast<http::status>(behavior_->status), req.version());
            res.set(http::field::content_type, "application/json");
            res.keep_alive(false);
            res.body() = behavior_->body;
            res.prepare_payload();
            http::write(sock, res, ec);
            sock.shutdown(tcp::socket::shutdown_both, ec);
        }).detach();
    }

    std::shared_ptr<MockBehavior> behavior_;
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::uint16_t port_ = 0;
};

class ScopedTempDir {
public:
    ScopedTempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("llm_e2e_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& p, std::string_view content) {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void UnsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class ArgvBuilder {
public:
    ArgvBuilder(std::initializer_list<std::string> args) : storage_(args) {
        for (auto& s : storage_) argv_.push_back(s.data());
    }
    int argc() const { return static_cast<int>(argv_.size()); }
    char** argv() { return argv_.data(); }
private:
    std::vector<std::string> storage_;
    std::vector<char*> argv_;
};

const std::string kValidResponse = R"({
    "id": "cmpl-e2e-123",
    "object": "chat.completion",
    "model": "deepseek-chat",
    "choices": [{
        "message": {"role": "assistant", "content": "记忆抽取完成：用户喜欢数学。"},
        "finish_reason": "stop",
        "index": 0
    }],
    "usage": {"prompt_tokens": 42, "completion_tokens": 12, "total_tokens": 54}
})";

}  // namespace

// ── E2E: full chain from config file to LLM response ─────────────────────────

namespace {

// 仅在 E2E 中改变 endpoint；复用生产编解码，验证真实 HTTP 两条路径服从协议接口。
class RoutedTestProtocol final : public agent::llm::ILlmProtocol {
public:
    std::string_view Endpoint() const noexcept override { return "test-protocol/complete"; }
    agent::llm::LlmProtocolCapabilities Capabilities() const noexcept override {
        return delegate_.Capabilities();
    }
    core::Status ValidateRequest(const ChatCompletionRequest& request) const override {
        return delegate_.ValidateRequest(request);
    }
    core::Result<std::string> EncodeRequest(const ChatCompletionRequest& request,
        const agent::llm::LlmProtocolContext& context) const override {
        return delegate_.EncodeRequest(request, context);
    }
    core::Result<agent::llm::ChatCompletionResponse> DecodeResponse(std::string_view body,
        const ChatCompletionRequest& request, const agent::llm::LlmProtocolContext& context,
        core::LoggerAdapter& logger) const override {
        return delegate_.DecodeResponse(body, request, context, logger);
    }
private:
    agent::llm::ChatCompletionsProtocol delegate_;
};

class LlmProtocolIntegrationE2E : public testing::TestWithParam<bool> {};

TEST_P(LlmProtocolIntegrationE2E, InjectedProtocolRunsThroughRealHttpAndMatchesDefaultWireFormat) {
    auto behavior = std::make_shared<MockBehavior>();
    behavior->body = kValidResponse;
    auto server = MockOpenAiServer::Start(behavior);
    OpenAiLlmClientOptions options;
    options.base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/v1";
    options.api_key = "protocol-e2e-key";
    options.default_model = "deepseek-chat";
    options.timeout_ms = 5000;
    options.retry_policy.max_retries = 0;
    options.protocol = std::make_shared<RoutedTestProtocol>();
    ChatCompletionRequest request;
    request.temperature = 0.5f;
    request.messages.push_back({ChatRole::User, "你好"});
    if (GetParam()) {
        auto http_client = agent::net::AsyncBeastHttpClient::Create({});
        ASSERT_TRUE(http_client.ok());
        auto client = agent::llm::OpenAiAsyncLlmClient::Create(options, *http_client.value());
        ASSERT_TRUE(client.ok());
        std::promise<core::Result<agent::llm::ChatCompletionResponse>> completed;
        auto future = completed.get_future();
        ASSERT_TRUE(client.value()->CompleteAsync(request, [&](auto result) {
            completed.set_value(std::move(result));
        }).ok());
        ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        auto result = future.get();
        ASSERT_TRUE(result.ok()) << result.status().message();
        EXPECT_EQ(result.value().content, "记忆抽取完成：用户喜欢数学。");
        EXPECT_EQ(result.value().total_tokens, 54);
    } else {
        auto http_client = BeastHttpClient::Create({});
        ASSERT_TRUE(http_client.ok());
        auto client = OpenAiLlmClient::Create(options, *http_client.value());
        ASSERT_TRUE(client.ok());
        auto result = client.value()->Complete(request);
        ASSERT_TRUE(result.ok()) << result.status().message();
        EXPECT_EQ(result.value().content, "记忆抽取完成：用户喜欢数学。");
        EXPECT_EQ(result.value().total_tokens, 54);
    }
    EXPECT_EQ(behavior->last_target, "/v1/test-protocol/complete");
    EXPECT_EQ(behavior->last_auth_header, "Bearer protocol-e2e-key");
    auto default_body = agent::llm::ChatCompletionsProtocol{}.EncodeRequest(
        request, {options.default_model, options.response_validation});
    ASSERT_TRUE(default_body.ok());
    EXPECT_EQ(behavior->last_body, default_body.value());
    EXPECT_EQ(behavior->call_count.load(), 1);
}

INSTANTIATE_TEST_SUITE_P(SyncAndAsync, LlmProtocolIntegrationE2E, testing::Bool());

}

TEST(LlmIntegrationE2E, FullChainFromConfigFile) {
    UnsetEnv("AGENT_LLM_API_KEY");

    auto behavior = std::make_shared<MockBehavior>();
    behavior->body = kValidResponse;
    auto server = MockOpenAiServer::Start(behavior);

    // 1. 准备配置文件目录：config.json + prompts/memory_extraction.txt
    ScopedTempDir tmp;
    auto prompts_dir = tmp.path() / "prompts";
    std::filesystem::create_directories(prompts_dir);

    const std::string memory_prompt =
        "你是记忆抽取器。请从下方对话中抽取用户偏好。\n"
        "输出 JSON 格式：{\"facts\": [...]}";
    WriteFile(prompts_dir / "memory_extraction.txt", memory_prompt);

    auto config_file = tmp.path() / "config.json";
    std::string config_content = R"({
        "llm": {
            "base_url": "http://127.0.0.1:)" + std::to_string(server->port()) + R"(/v1",
            "model": "deepseek-chat",
            "timeout_ms": 5000,
            "max_retries": 0,
            "prompts": {
                "memory_extraction": "prompts/memory_extraction.txt"
            }
        }
    })";
    WriteFile(config_file, config_content);

    SetEnv("AGENT_LLM_API_KEY", "sk-e2e-test-key");

    // 2. 通过 ParseMultimodalOptions 走完整配置链路
    ArgvBuilder argv({"server", "--llm", "llm.gguf", "--config", config_file.string()});
    MultimodalServerOptions options;
    ASSERT_NO_THROW(options = ParseMultimodalOptions(argv.argc(), argv.argv()));

    EXPECT_FALSE(options.llm.base_url.empty());
    EXPECT_EQ(options.llm.api_key, "sk-e2e-test-key");
    EXPECT_EQ(options.llm.prompts.size(), 1u);
    EXPECT_TRUE(options.config_file_path.is_absolute());

    // 3. 加载 prompt store（路径相对于 config 文件目录）
    LlmPromptStore prompt_store;
    auto st = prompt_store.Load(options.llm.prompts,
                                 options.config_file_path.parent_path());
    ASSERT_TRUE(st.ok()) << st.message();
    ASSERT_TRUE(prompt_store.Has("memory_extraction"));

    auto loaded = prompt_store.Get("memory_extraction");
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value(), memory_prompt);

    // 4. 构造 HTTP 客户端 + LLM 客户端
    auto http_r = BeastHttpClient::Create(BeastHttpClientOptions{});
    ASSERT_TRUE(http_r.ok());
    auto http_client = std::move(http_r).value();

    OpenAiLlmClientOptions llm_opts;
    llm_opts.base_url = options.llm.base_url;
    llm_opts.api_key = options.llm.api_key;
    llm_opts.default_model = options.llm.model;
    llm_opts.timeout_ms = options.llm.timeout_ms;
    llm_opts.retry_policy.max_retries = options.llm.max_retries;

    auto client_r = OpenAiLlmClient::Create(std::move(llm_opts), *http_client);
    ASSERT_TRUE(client_r.ok());
    auto client = std::move(client_r).value();

    // 5. 业务代码组装：静态 prompt + 动态对话
    ChatCompletionRequest req;
    req.messages.push_back({ChatRole::System, prompt_store.Get("memory_extraction").value()});
    req.messages.push_back({ChatRole::User, "我最喜欢的科目是数学"});

    auto resp = client->Complete(req);
    ASSERT_TRUE(resp.ok()) << resp.status().message();

    EXPECT_EQ(resp.value().content, "记忆抽取完成：用户喜欢数学。");
    EXPECT_EQ(resp.value().model, "deepseek-chat");
    EXPECT_EQ(resp.value().prompt_tokens, 42);
    EXPECT_EQ(resp.value().completion_tokens, 12);

    // 6. 验证请求侧细节
    EXPECT_EQ(behavior->call_count.load(), 1);
    EXPECT_EQ(behavior->last_auth_header, "Bearer sk-e2e-test-key");
    EXPECT_EQ(behavior->last_target, "/v1/chat/completions");
    EXPECT_NE(behavior->last_body.find("记忆抽取器"), std::string::npos);
    EXPECT_NE(behavior->last_body.find("最喜欢的科目"), std::string::npos);

    UnsetEnv("AGENT_LLM_API_KEY");
}

TEST(LlmIntegrationE2E, ApiKeyFromFileEndToEnd) {
    UnsetEnv("AGENT_LLM_API_KEY");

    auto behavior = std::make_shared<MockBehavior>();
    behavior->body = kValidResponse;
    auto server = MockOpenAiServer::Start(behavior);

    ScopedTempDir tmp;
    auto key_file = tmp.path() / "secret.key";
    WriteFile(key_file, "sk-from-file-e2e\n");

    auto config_file = tmp.path() / "config.json";
    std::string config_content = R"({
        "llm": {
            "base_url": "http://127.0.0.1:)" + std::to_string(server->port()) + R"(/v1",
            "api_key_file": ")" + key_file.generic_string() + R"(",
            "model": "deepseek-chat",
            "timeout_ms": 5000,
            "max_retries": 0
        }
    })";
    WriteFile(config_file, config_content);

    ArgvBuilder argv({"server", "--llm", "llm.gguf", "--config", config_file.string()});
    auto options = ParseMultimodalOptions(argv.argc(), argv.argv());
    EXPECT_EQ(options.llm.api_key, "sk-from-file-e2e");

    auto http_client = BeastHttpClient::Create(BeastHttpClientOptions{}).value();
    OpenAiLlmClientOptions llm_opts;
    llm_opts.base_url = options.llm.base_url;
    llm_opts.api_key = options.llm.api_key;
    llm_opts.default_model = options.llm.model;
    llm_opts.timeout_ms = options.llm.timeout_ms;

    auto client = OpenAiLlmClient::Create(std::move(llm_opts), *http_client).value();

    ChatCompletionRequest req;
    req.messages.push_back({ChatRole::User, "hello"});
    auto resp = client->Complete(req);
    ASSERT_TRUE(resp.ok());

    EXPECT_EQ(behavior->last_auth_header, "Bearer sk-from-file-e2e");
}

TEST(LlmIntegrationE2E, PromptStoreResolvesRelativePathsFromConfigDir) {
    UnsetEnv("AGENT_LLM_API_KEY");
    SetEnv("AGENT_LLM_API_KEY", "sk-test");

    ScopedTempDir tmp;
    // 创建一个嵌套的 prompts 目录，验证相对路径解析正确
    auto nested = tmp.path() / "subdir" / "prompts";
    std::filesystem::create_directories(nested);
    WriteFile(nested / "p1.txt", "prompt-one-content");
    WriteFile(nested / "p2.txt", "prompt-two-content");

    auto config_file = tmp.path() / "subdir" / "config.json";
    WriteFile(config_file, R"({
        "llm": {
            "base_url": "http://127.0.0.1:1/v1",
            "prompts": {
                "p1": "prompts/p1.txt",
                "p2": "prompts/p2.txt"
            }
        }
    })");

    ArgvBuilder argv({"server", "--llm", "llm.gguf", "--config", config_file.string()});
    auto options = ParseMultimodalOptions(argv.argc(), argv.argv());

    LlmPromptStore store;
    auto st = store.Load(options.llm.prompts, options.config_file_path.parent_path());
    ASSERT_TRUE(st.ok()) << st.message();

    EXPECT_EQ(store.Get("p1").value(), "prompt-one-content");
    EXPECT_EQ(store.Get("p2").value(), "prompt-two-content");

    UnsetEnv("AGENT_LLM_API_KEY");
}
