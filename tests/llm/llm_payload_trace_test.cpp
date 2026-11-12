#include "openai_llm_client.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <future>
#include <sstream>

namespace {
namespace llm = agent::llm;
namespace net = agent::net;
using Json = nlohmann::json;

// Mock 仅在测试内提供，同一响应通过同步、异步和逐字节 SSE 三条客户端路径。
class Done final : public net::IAsyncHttpOperation { public: void Cancel() noexcept override {} };
class Transport final : public net::IHttpClient, public net::IAsyncHttpClient,
                        public net::IAsyncStreamingHttpClient {
public:
    core::Result<net::HttpClientResponse> Execute(const net::HttpClientRequest&) override {
        return net::HttpClientResponse{200, {}, body};
    }
    core::Result<std::shared_ptr<net::IAsyncHttpOperation>> ExecuteAsync(
        net::HttpClientRequest request, Callback callback) override {
        callback(Execute(request));
        return std::shared_ptr<net::IAsyncHttpOperation>(std::make_shared<Done>());
    }
    core::Result<std::shared_ptr<net::IAsyncHttpOperation>> ExecuteStreamingAsync(
        net::HttpClientRequest, net::HttpStreamOptions, net::HttpStreamCallbacks callbacks) override {
        auto status = callbacks.on_headers({200, {{"Content-Type", "text/event-stream"}}, {}});
        for (std::size_t i = 0; status.ok() && i < body.size(); ++i)
            status = callbacks.on_body(std::string_view(body).substr(i, 1));
        callbacks.on_complete(status);
        return std::shared_ptr<net::IAsyncHttpOperation>(std::make_shared<Done>());
    }
    std::string body;
};

class PayloadTraceTest : public testing::TestWithParam<int> {
protected:
    void SetUp() override {
        logger = std::make_shared<spdlog::logger>("llm-payload", std::make_shared<spdlog::sinks::ostream_sink_mt>(output));
        logger->set_pattern("%v");
        logger->set_level(spdlog::level::debug);
        spdlog::register_logger(logger);
        options.base_url = "http://fixture";
        options.api_key = "secret-bearer-never-log";
        options.retry_policy.max_retries = 0;
        options.debug_payloads = true;
    }
    void TearDown() override { spdlog::drop("llm-payload"); }
    core::Result<llm::ChatCompletionResponse> Run(std::string content, std::string finish = "stop") {
        Json message{{"content", content}, {"reasoning_content", "工具推理🙂"}};
        if (GetParam() == 2) {
            transport.body = "data: " + Json{{"choices", Json::array({{{"index", 0}, {"delta", message},
                {"finish_reason", finish}}})}}.dump() + "\n\ndata: [DONE]\n\n";
        } else transport.body = Json{{"choices", Json::array({{{"message", message}, {"finish_reason", finish}}})}}.dump();
        llm::ChatCompletionRequest request;
        request.messages.push_back({llm::ChatRole::User, "诊断请求"});
        if (GetParam() == 0) return llm::OpenAiLlmClient::Create(options, transport).value()->Complete(request);
        auto client = llm::OpenAiAsyncLlmClient::Create(options, transport).value();
        std::promise<core::Result<llm::ChatCompletionResponse>> promise;
        auto callback = [&](auto result) { promise.set_value(std::move(result)); };
        auto operation = GetParam() == 1 ? client->CompleteAsync(request, callback) :
            client->CompleteStreamingAsync(request, [](const auto&) { return core::Status::Ok(); }, callback);
        if (!operation.ok()) return operation.status();
        return promise.get_future().get();
    }
    std::ostringstream output;
    std::shared_ptr<spdlog::logger> logger;
    llm::OpenAiLlmClientOptions options;
    Transport transport;
};

TEST_P(PayloadTraceTest, PreservesReasoningAndDsmlWithoutInventingCalls) {
    auto result = Run("<｜tool_calls｜>诊断");
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_TRUE(result.value().tool_calls.empty());
    EXPECT_EQ(result.value().reasoning_content, "工具推理🙂");
    const auto log = output.str();
    EXPECT_NE(log.find("encoded_request"), std::string::npos);
    EXPECT_NE(log.find(GetParam() == 2 ? "raw_sse" : "raw_response"), std::string::npos);
    EXPECT_NE(log.find("decoded_response"), std::string::npos);
    EXPECT_NE(log.find("工具推理🙂"), std::string::npos);
    EXPECT_NE(log.find("<｜tool_calls｜>"), std::string::npos);
    EXPECT_EQ(log.find(options.api_key), std::string::npos);
}
TEST_P(PayloadTraceTest, RequiresBothOptInAndDebugLevel) {
    options.debug_payloads = false;
    ASSERT_TRUE(Run("private").ok());
    EXPECT_TRUE(output.str().empty());
    options.debug_payloads = true;
    logger->set_level(spdlog::level::info);
    ASSERT_TRUE(Run("private").ok());
    EXPECT_TRUE(output.str().empty());
}
TEST_P(PayloadTraceTest, KeepsRawEvidenceOnLengthRejection) {
    EXPECT_FALSE(Run("截断<｜tool", "length").ok());
    EXPECT_NE(output.str().find("length"), std::string::npos);
    EXPECT_NE(output.str().find("decode_error"), std::string::npos);
}
TEST_P(PayloadTraceTest, BoundsLogsAndKeepsUtf8Tail) {
    options.debug_payload_max_bytes = 256;
    ASSERT_TRUE(Run(std::string(8000, 'x') + "尾部<｜tool_calls｜>").ok());
    EXPECT_LT(output.str().size(), 2500u);
    EXPECT_NE(output.str().find("尾部"), std::string::npos);
    std::istringstream lines(output.str());
    std::string line;
    while (std::getline(lines, line)) EXPECT_NO_THROW(static_cast<void>(Json::parse(line.substr(12))));
}
INSTANTIATE_TEST_SUITE_P(ClientPaths, PayloadTraceTest, testing::Values(0, 1, 2));
}
