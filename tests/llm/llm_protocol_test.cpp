#include "llm_client.h"
#include "llm_protocol.h"
#include "openai_llm_client.h" // 同时验证旧头文件仍可取得公共类型。

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <mutex>
#include <stdexcept>

namespace {

using namespace agent::llm;
namespace net = agent::net;

constexpr auto kResponse = R"({"id":"cmpl-fixed","model":"fixture-model","choices":[{"message":{"role":"assistant","content":"答案 ✓","reasoning_content":"推理","tool_calls":[{"id":"call-1","type":"function","function":{"name":"lookup","arguments":{"q":"事实"}}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})";

ChatCompletionRequest FixtureRequest() {
    ChatCompletionRequest request;
    request.temperature = 0.5f;
    request.top_p = 0.75f;
    request.max_tokens = 128;
    request.messages.push_back({ChatRole::System, "规则\n"});
    request.messages.push_back({ChatRole::User, "", {
        ChatContentPart::Text("看图"), ChatContentPart::ImageData("image/png", "aGVsbG8=")}});
    ChatMessage assistant;
    assistant.role = ChatRole::Assistant;
    assistant.tool_calls.push_back({"call-1", "lookup", R"({"q":"事实"})"});
    assistant.reasoning_content = "理由";
    request.messages.push_back(std::move(assistant));
    ChatMessage tool;
    tool.role = ChatRole::Tool;
    tool.content = R"({"found":true})";
    tool.tool_call_id = "call-1";
    request.messages.push_back(std::move(tool));
    request.tools.push_back({"lookup", "查事实", R"({"type":"object","properties":{"q":{"type":"string"}},"required":["q"]})"});
    request.tool_choice = "auto";
    request.parallel_tool_calls = false;
    return request;
}

// 固定旧 serializer 的完整字节输出；不通过新实现反向构造期望值。
constexpr auto kRequestBody = R"({"max_tokens":128,"messages":[{"content":"规则\n","role":"system"},{"content":[{"text":"看图","type":"text"},{"image_url":{"url":"data:image/png;base64,aGVsbG8="},"type":"image_url"}],"role":"user"},{"content":null,"reasoning_content":"理由","role":"assistant","tool_calls":[{"function":{"arguments":"{\"q\":\"事实\"}","name":"lookup"},"id":"call-1","type":"function"}]},{"content":"{\"found\":true}","role":"tool","tool_call_id":"call-1"}],"model":"fixture-model","n":1,"parallel_tool_calls":false,"stream":false,"temperature":0.5,"tool_choice":"auto","tools":[{"function":{"description":"查事实","name":"lookup","parameters":{"properties":{"q":{"type":"string"}},"required":["q"],"type":"object"}},"type":"function"}],"top_p":0.75})";

TEST(ChatCompletionsProtocolTest, PreservesExactRequestBytesAndCompleteResponse) {
    const ChatCompletionsProtocol protocol;
    const auto request = FixtureRequest();
    const LlmProtocolContext context{"fixture-model", {}};
    auto encoded = protocol.EncodeRequest(request, context);
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();
    EXPECT_EQ(encoded.value(), kRequestBody);
    auto logger = core::LoggerAdapter::ForModule("test");
    auto decoded = protocol.DecodeResponse(kResponse, request, context, logger);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    const auto& response = decoded.value();
    EXPECT_EQ(response.id, "cmpl-fixed");
    EXPECT_EQ(response.model, "fixture-model");
    EXPECT_EQ(response.content, "答案 ✓");
    EXPECT_EQ(response.reasoning_content, "推理");
    EXPECT_EQ(response.finish_reason, "tool_calls");
    EXPECT_EQ(response.prompt_tokens, 10);
    EXPECT_EQ(response.completion_tokens, 5);
    EXPECT_EQ(response.total_tokens, 15);
    ASSERT_EQ(response.tool_calls.size(), 1u);
    EXPECT_EQ(response.tool_calls[0].id, "call-1");
    EXPECT_EQ(response.tool_calls[0].name, "lookup");
    EXPECT_EQ(response.tool_calls[0].arguments_json, R"({"q":"事实"})");
}

TEST(ChatCompletionsProtocolTest, ReportsWireCapabilitiesAndCreatesStreamingDecoder) {
    ChatCompletionsProtocol protocol;
    EXPECT_EQ(protocol.Endpoint(), "chat/completions");
    const auto caps = protocol.Capabilities();
    EXPECT_TRUE(caps.function_tools);
    EXPECT_TRUE(caps.image_inputs);
    EXPECT_TRUE(caps.reasoning_content);
    EXPECT_TRUE(caps.streaming);
    ChatCompletionRequest request;
    request.stream = true;
    EXPECT_TRUE(protocol.ValidateRequest(request).ok());
    EXPECT_TRUE(protocol.CreateStreamDecoder(request, {}).ok());
    request.n = 2;
    EXPECT_EQ(protocol.ValidateRequest(request).code(), core::ErrorCode::InvalidArgument);
}

TEST(ChatCompletionsProtocolTest, PreservesToolAssociationValidationAndLegacyEntryPoint) {
    auto request = FixtureRequest();
    request.messages.back().tool_call_id = "unknown-call";
    ChatCompletionsProtocol protocol;
    auto rejected = protocol.ValidateRequest(request);
    EXPECT_EQ(rejected.code(), core::ErrorCode::InvalidArgument);
    EXPECT_EQ(rejected.message(), "tool result does not match a pending assistant call");
    EXPECT_EQ(ValidateChatCompletionRequest(request).message(), rejected.message());
    request = FixtureRequest();
    request.messages.pop_back();
    EXPECT_EQ(protocol.ValidateRequest(request).message(), "assistant tool calls are missing results");
    request = FixtureRequest();
    request.tools[0].parameters_json = "not json";
    EXPECT_EQ(protocol.ValidateRequest(request).code(), core::ErrorCode::InvalidArgument);
    EXPECT_NE(protocol.EncodeRequest(request, {}).status().code(), core::ErrorCode::Ok);
    request = FixtureRequest();
    request.tool_choice = "custom-tool";
    EXPECT_EQ(protocol.ValidateRequest(request).message(), "unsupported tool_choice");
}

TEST(ChatCompletionsProtocolTest, PreservesTextAndUtf8ValidationPolicies) {
    ChatCompletionsProtocol protocol;
    auto logger = core::LoggerAdapter::ForModule("test");
    const auto decode = [&](std::string_view body) {
        return protocol.DecodeResponse(body, {}, {}, logger);
    };
    EXPECT_EQ(decode("not json").status().code(), core::ErrorCode::DataLoss);
    EXPECT_EQ(decode("").status().message(), "LLM response body is empty");
    EXPECT_EQ(decode(R"({"choices":[]})").status().message(), "LLM response missing choices array");
    EXPECT_EQ(decode(R"({"choices":[{"message":{"content":42}}]})").status().code(), core::ErrorCode::DataLoss);
    EXPECT_EQ(decode(R"({"choices":[{"message":{"content":"\u0001"}}]})").status().message(), "LLM content is not valid UTF-8 text");
    EXPECT_EQ(decode(R"({"choices":[{"message":{"content":"ok","reasoning_content":"\u0001"}}]})").status().code(), core::ErrorCode::DataLoss);
    EXPECT_EQ(decode(R"({"choices":[{"message":{"content":" "}}]})").status().code(), core::ErrorCode::Unavailable);
    EXPECT_EQ(decode(R"({"choices":[{"message":{"content":"ok"},"finish_reason":"length"}]})").status().code(), core::ErrorCode::ResourceExhausted);
    LlmProtocolContext relaxed;
    relaxed.response_validation.reject_empty_content = false;
    relaxed.response_validation.reject_length_finish = false;
    EXPECT_TRUE(protocol.DecodeResponse(R"({"choices":[{"message":{"content":" "},"finish_reason":"length"}]})", {}, relaxed, logger).ok());
    ChatCompletionRequest request;
    request.messages.push_back({ChatRole::User, std::string(1, static_cast<char>(0xFF))});
    auto encoded = protocol.EncodeRequest(request, {"model", {}});
    ASSERT_TRUE(encoded.ok());
    EXPECT_NE(encoded.value().find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(ChatCompletionsProtocolTest, RejectsMalformedUsageAndToolCallsWithoutHttp) {
    ChatCompletionsProtocol protocol;
    auto logger = core::LoggerAdapter::ForModule("test");
    for (auto usage : {R"({"prompt_tokens":10,"completion_tokens":5,"total_tokens":12})",
                       R"({"completion_tokens":-1})", R"({"completion_tokens":2147483648})",
                       R"({"completion_tokens":"5"})", "[]"}) {
        const auto body = std::string(R"({"choices":[{"message":{"content":"ok"}}],"usage":)") + usage + "}";
        EXPECT_EQ(protocol.DecodeResponse(body, {}, {}, logger).status().code(), core::ErrorCode::DataLoss);
    }
    EXPECT_EQ(protocol.DecodeResponse(R"({"choices":[{"message":{"content":null,"tool_calls":[{}]}}]})", {}, {}, logger).status().message(), "malformed LLM tool call");
    auto missing_usage = protocol.DecodeResponse(R"({"choices":[{"message":{"content":"ok"}}]})", {}, {}, logger);
    ASSERT_TRUE(missing_usage.ok());
    EXPECT_EQ(missing_usage.value().total_tokens, 0);
}

class Counter final : public ICompletionTokenCounter {
public:
    core::Result<std::size_t> CountTokens(std::string_view model,
                                         const ChatCompletionResponse&) const override {
        EXPECT_EQ(model, "fixture-model");
        return std::size_t{100};
    }
};

TEST(ChatCompletionsProtocolTest, KeepsStrictAndAuditTokenCountSemantics) {
    ChatCompletionsProtocol protocol;
    LlmProtocolContext context{"fixture-model", {}};
    context.response_validation.token_count_mode = CompletionTokenValidationMode::Strict;
    context.response_validation.token_counter = std::make_shared<Counter>();
    auto logger = core::LoggerAdapter::ForModule("test");
    EXPECT_EQ(protocol.DecodeResponse(kResponse, {}, context, logger).status().message(),
              "LLM completion token count differs from provider usage");
    context.response_validation.token_count_mode = CompletionTokenValidationMode::Audit;
    EXPECT_TRUE(protocol.DecodeResponse(kResponse, {}, context, logger).ok());
}

// 测试协议只在本测试文件实现，确保客户端不假设 JSON 或 Chat Completions endpoint。
class RecordingProtocol final : public ILlmProtocol {
public:
    std::string_view Endpoint() const noexcept override { return "custom/infer"; }
    LlmProtocolCapabilities Capabilities() const noexcept override { return {}; }
    core::Status ValidateRequest(const ChatCompletionRequest& request) const override {
        if (throw_stage == 1) throw std::runtime_error("test validation exception");
        if (request.stream) return core::Status::Error(core::ErrorCode::Unimplemented, "test streaming unavailable");
        return core::Status::Ok();
    }
    core::Result<std::string> EncodeRequest(const ChatCompletionRequest&,
                                           const LlmProtocolContext& context) const override {
        if (throw_stage == 2) throw std::runtime_error("test encode exception");
        if (fail_encode) return core::Status::Error(core::ErrorCode::InvalidArgument, "test encode rejected");
        return "owned-body:" + context.default_model;
    }
    core::Result<ChatCompletionResponse> DecodeResponse(std::string_view body,
        const ChatCompletionRequest&, const LlmProtocolContext&,
        core::LoggerAdapter&) const override {
        ++decodes;
        if (throw_stage == 3) throw std::runtime_error("test decode exception");
        ChatCompletionResponse response;
        response.content = "decoded:" + std::string(body);
        return response;
    }
    int throw_stage = 0;
    bool fail_encode = false;
    mutable std::atomic<int> decodes{0};
};

class CompletedHttpOperation final : public net::IAsyncHttpOperation {
public:
    void Cancel() noexcept override {}
};

class RecordingTransport final : public net::IHttpClient, public net::IAsyncHttpClient {
public:
    core::Result<net::HttpClientResponse> Execute(const net::HttpClientRequest& request) override {
        std::lock_guard lock(mutex);
        requests.push_back(request);
        if (transport_error) return core::Status::Error(core::ErrorCode::Timeout, "transport failed");
        return net::HttpClientResponse{status, {}, body};
    }
    core::Result<std::shared_ptr<net::IAsyncHttpOperation>> ExecuteAsync(
        net::HttpClientRequest request, Callback callback) override {
        callback(Execute(request));
        return std::shared_ptr<net::IAsyncHttpOperation>(std::make_shared<CompletedHttpOperation>());
    }
    std::mutex mutex;
    std::vector<net::HttpClientRequest> requests;
    std::string body = kResponse;
    int status = 200;
    bool transport_error = false;
};

// 参数化测试在完全相同输入上检查同步/异步 envelope、响应和拒绝语义。
class ProtocolTransportTest : public testing::TestWithParam<bool> {
protected:
    core::Result<ChatCompletionResponse> Complete(OpenAiLlmClientOptions options,
        RecordingTransport& transport, ChatCompletionRequest request = {}) {
        if (!GetParam()) {
            auto client = OpenAiLlmClient::Create(std::move(options), transport);
            if (!client.ok()) return client.status();
            return client.value()->Complete(request);
        }
        auto client = OpenAiAsyncLlmClient::Create(std::move(options), transport);
        if (!client.ok()) return client.status();
        std::promise<core::Result<ChatCompletionResponse>> completed;
        auto future = completed.get_future();
        auto submitted = client.value()->CompleteAsync(std::move(request), [&](auto result) {
            completed.set_value(std::move(result));
        });
        if (!submitted.ok()) return submitted.status();
        return future.get();
    }
    OpenAiLlmClientOptions Options(std::shared_ptr<const ILlmProtocol> protocol = {}) {
        OpenAiLlmClientOptions options;
        options.base_url = "http://example.invalid/v1";
        options.api_key = "test-key";
        options.default_model = "fixture-model";
        options.retry_policy.max_retries = 0;
        options.protocol = std::move(protocol);
        return options;
    }
};

TEST_P(ProtocolTransportTest, DefaultProtocolMatchesLegacyBytesOverBothTransports) {
    RecordingTransport transport;
    auto result = Complete(Options(), transport, FixtureRequest());
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().content, "答案 ✓");
    ASSERT_EQ(transport.requests.size(), 1u);
    const auto& request = transport.requests[0];
    EXPECT_EQ(request.url, "http://example.invalid/v1/chat/completions");
    EXPECT_EQ(request.body, kRequestBody);
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.timeout_ms, 30000);
    ASSERT_EQ(request.headers.size(), 2u);
    EXPECT_EQ(request.headers[0].name, "Content-Type");
    EXPECT_EQ(request.headers[0].value, "application/json");
    EXPECT_EQ(request.headers[1].name, "Authorization");
    EXPECT_EQ(request.headers[1].value, "Bearer test-key");
}

TEST_P(ProtocolTransportTest, UsesInjectedEndpointEncoderAndDecoder) {
    RecordingTransport transport;
    transport.body = "raw-payload";
    auto protocol = std::make_shared<RecordingProtocol>();
    auto options = Options(protocol);
    options.base_url += '/';
    auto result = Complete(options, transport);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().content, "decoded:raw-payload");
    EXPECT_EQ(transport.requests[0].url, "http://example.invalid/v1/custom/infer");
    EXPECT_EQ(transport.requests[0].body, "owned-body:fixture-model");
    EXPECT_EQ(protocol->decodes.load(), 1);
}

TEST_P(ProtocolTransportTest, HttpStatusAndTransportErrorsNeverEnterProtocolDecoder) {
    auto protocol = std::make_shared<RecordingProtocol>();
    for (const auto& [http_status, error] : std::vector<std::pair<int, core::ErrorCode>>{
             {401, core::ErrorCode::PermissionDenied}, {403, core::ErrorCode::PermissionDenied},
             {429, core::ErrorCode::ResourceExhausted}, {503, core::ErrorCode::Unavailable},
             {400, core::ErrorCode::InternalError}, {201, core::ErrorCode::InternalError}}) {
        RecordingTransport transport;
        transport.status = http_status;
        transport.body = "not-json";
        auto result = Complete(Options(protocol), transport);
        EXPECT_EQ(result.status().code(), error);
        EXPECT_EQ(result.status().message(), "LLM API returned status " + std::to_string(http_status) + ": not-json");
        EXPECT_EQ(protocol->decodes.load(), 0);
    }
    RecordingTransport failed;
    failed.transport_error = true;
    EXPECT_EQ(Complete(Options(protocol), failed).status().code(), core::ErrorCode::Timeout);
    EXPECT_EQ(protocol->decodes.load(), 0);
}

TEST_P(ProtocolTransportTest, RejectsUnsupportedRequestsAndEncodeFailuresBeforeHttp) {
    RecordingTransport transport;
    auto protocol = std::make_shared<RecordingProtocol>();
    ChatCompletionRequest request;
    request.stream = true;
    EXPECT_EQ(Complete(Options(protocol), transport, request).status().code(), core::ErrorCode::Unimplemented);
    protocol->fail_encode = true;
    EXPECT_EQ(Complete(Options(protocol), transport).status().message(), "test encode rejected");
    EXPECT_TRUE(transport.requests.empty());
    EXPECT_EQ(Complete(Options(), transport, request).status().code(), core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(transport.requests.empty());
}

TEST_P(ProtocolTransportTest, ConvertsThirdPartyProtocolExceptionsToStatus) {
    for (int stage : {1, 2, 3}) {
        RecordingTransport transport;
        auto protocol = std::make_shared<RecordingProtocol>();
        protocol->throw_stage = stage;
        auto result = Complete(Options(protocol), transport);
        EXPECT_EQ(result.status().code(), core::ErrorCode::InternalError);
        EXPECT_EQ(transport.requests.size(), stage == 3 ? 1u : 0u);
    }
}

INSTANTIATE_TEST_SUITE_P(SyncAndAsync, ProtocolTransportTest, testing::Bool());

TEST(ChatCompletionsProtocolTest, ImmutableProtocolCanBeSharedAcrossConcurrentRequests) {
    const auto protocol = std::make_shared<const ChatCompletionsProtocol>();
    std::vector<std::future<bool>> futures;
    // 测试线程只用于制造共享协议并发，生产仍复用已有线程池与异步 HTTP。
    for (int i = 0; i != 12; ++i) {
        futures.push_back(std::async(std::launch::async, [protocol, i] {
            ChatCompletionRequest request;
            request.model = "model-" + std::to_string(i);
            auto encoded = protocol->EncodeRequest(request, {"default", {}});
            auto logger = core::LoggerAdapter::ForModule("test");
            auto decoded = protocol->DecodeResponse(kResponse, request, {}, logger);
            return encoded.ok() && encoded.value().find(request.model) != std::string::npos &&
                   decoded.ok() && decoded.value().content == "答案 ✓";
        }));
    }
    for (auto& future : futures) EXPECT_TRUE(future.get());
}

}
