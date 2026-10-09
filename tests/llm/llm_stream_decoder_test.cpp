#include "llm_protocol.h"
#include "../../src/net/sse.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
using namespace agent::llm;
using Json = nlohmann::json;

std::string Chunk(Json delta, Json finish = nullptr) {
    return "data: " + Json{{"id", "generation"}, {"model", "fake"},
        {"choices", Json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}}.dump() + "\n\n";
}

std::string TextStream() {
    return Chunk({{"role", "assistant"}, {"content", "中文🙂"}, {"reasoning_content", "推理"}}) +
        Chunk({{"content", " reply"}}, "stop") +
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}}\n\n" +
        "data: [DONE]\n\n";
}

std::unique_ptr<ILlmStreamDecoder> Decoder(LlmProtocolContext context = {}) {
    ChatCompletionRequest request;
    request.stream = true;
    auto result = ChatCompletionsProtocol{}.CreateStreamDecoder(request, context);
    EXPECT_TRUE(result.ok());
    return result.ok() ? std::move(result).value() : nullptr;
}

TEST(LlmStreamDecoderTest, EverySplitIncludingUtf8AndCrLfProducesSameValidatedResponse) {
    const auto bytes = TextStream();
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        auto decoder = Decoder();
        ASSERT_TRUE(decoder->Feed(std::string_view(bytes).substr(0, split)).ok()) << split;
        ASSERT_TRUE(decoder->Feed(std::string_view(bytes).substr(split)).ok()) << split;
        auto response = decoder->Finish();
        ASSERT_TRUE(response.ok()) << split;
        EXPECT_EQ(response.value().content, "中文🙂 reply");
        EXPECT_EQ(response.value().reasoning_content, "推理");
        EXPECT_EQ(response.value().total_tokens, 5);
    }
    std::string crlf;
    for (char value : bytes) { if (value == '\n') crlf += '\r'; crlf += value; }
    auto decoder = Decoder();
    for (char value : crlf) ASSERT_TRUE(decoder->Feed({&value, 1}).ok());
    ASSERT_TRUE(decoder->Finish().ok());
}

TEST(LlmStreamDecoderTest, SseBomMultilineCommentsAndUnknownFieldsFollowFramingRules) {
    std::vector<agent::net::SseEvent> events;
    agent::net::SseDecoder framing([&](const auto& event) { events.push_back(event); return core::Status::Ok(); });
    const std::string bytes = "\xef\xbb\xbf: heartbeat\r\nretry: 15\r\nunknown: x\r\nid: 7\r\nevent: sample\r\ndata: first\r\ndata: second\r\n\r\n";
    for (char value : bytes) ASSERT_TRUE(framing.Feed({&value, 1}).ok());
    ASSERT_TRUE(framing.Finish().ok());
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].data, "first\nsecond");
    EXPECT_EQ(events[0].event, "sample");
    EXPECT_EQ(events[0].id, "7");
}

TEST(LlmStreamDecoderTest, ToolArgumentsAreAssembledAndOnlyCompletedObjectsAccepted) {
    auto bytes = Chunk({{"tool_calls", Json::array({{{"index", 0}, {"id", "call"}, {"type", "function"},
        {"function", {{"name", "lookup"}, {"arguments", "{\"q\":"}}}}})}}) +
        Chunk({{"tool_calls", Json::array({{{"index", 0}, {"function", {{"arguments", "\"中文\"}"}}}}})}}, "tool_calls") +
        "data: [DONE]\n\n";
    auto decoder = Decoder();
    for (char value : bytes) ASSERT_TRUE(decoder->Feed({&value, 1}).ok());
    auto result = decoder->Finish();
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().tool_calls.size(), 1);
    EXPECT_EQ(result.value().tool_calls[0].id, "call");
    EXPECT_EQ(result.value().tool_calls[0].arguments_json, "{\"q\":\"中文\"}");
    auto broken = Decoder();
    EXPECT_EQ(broken->Feed(Chunk({{"tool_calls", Json::array({{{"index", 0}, {"id", "c"},
        {"function", {{"name", "lookup"}, {"arguments", "{"}}}}})}}, "tool_calls") +
        "data: [DONE]\n\n").code(), core::ErrorCode::DataLoss);
}

TEST(LlmStreamDecoderTest, RejectsCorruptionMissingTerminalsAndPostDoneData) {
    const std::vector<std::string> corrupt{
        "data: {bad}\n\n", "data: []\n\n", "data: [DONE]\n\n",
        Chunk({{"content", 3}}), Chunk({{"role", "user"}}),
        Chunk({{"content", "hello"}}) + "data: [DONE]\n\n",
        Chunk({{"content", "hello"}}, "stop"),
        Chunk({{"content", "hello"}}, "stop") + "data: [DONE]\n", // 完整事件必须有空行。
        TextStream() + "data: [DONE]\n\n",
        Chunk({{"content", "hello"}}, "stop") + Chunk({{"content", "late"}}),
        "data: {\"choices\":[{\"index\":-1,\"delta\":{}}]}\n\n",
        "data: {\"choices\":[],\"usage\":{\"total_tokens\":-1}}\n\n",
        "data: {\"choices\":[],\"usage\":{\"total_tokens\":18446744073709551615}}\n\n",
        std::string("data: ") + char(0xff) + "\n\n",
        Chunk({{"content", std::string(1, '\0')}}),
    };
    for (const auto& bytes : corrupt) {
        auto decoder = Decoder();
        auto status = decoder->Feed(bytes);
        if (status.ok()) EXPECT_FALSE(decoder->Finish().ok()) << bytes;
        else EXPECT_EQ(status.code(), core::ErrorCode::DataLoss) << bytes;
    }
}

TEST(LlmStreamDecoderTest, EnforcesAllLimitsAndPreservesSinkFailure) {
    LlmProtocolContext options;
    options.stream_limits.max_event_bytes = 16;
    EXPECT_EQ(Decoder(options)->Feed(std::string(17, ':')).code(), core::ErrorCode::ResourceExhausted);
    options.stream_limits.max_event_bytes = 256 * 1024;
    options.stream_limits.max_generation_bytes = 8;
    EXPECT_EQ(Decoder(options)->Feed(": ping\n\n: ping\n\n").code(), core::ErrorCode::ResourceExhausted);
    options.stream_limits.max_generation_bytes = 1024;
    options.stream_limits.max_tool_arguments_bytes = 2;
    EXPECT_EQ(Decoder(options)->Feed(Chunk({{"tool_calls", Json::array({{{"index", 0},
        {"function", {{"arguments", "abcd"}}}}})}})).code(), core::ErrorCode::ResourceExhausted);
    options.stream_limits.max_tool_calls = 1;
    EXPECT_EQ(Decoder(options)->Feed(Chunk({{"tool_calls", Json::array({{{"index", 1}}})}})).code(),
              core::ErrorCode::ResourceExhausted);
    options.event_sink = [](const auto&) { return core::Status::Error(core::ErrorCode::ResourceExhausted, "slow consumer"); };
    EXPECT_EQ(Decoder(options)->Feed(Chunk({{"content", "one"}})).message(), "slow consumer");
}

TEST(LlmStreamDecoderTest, UsageIsSnapshotAndLengthUsesOrdinaryCompletionValidation) {
    std::vector<LlmStreamEvent> events;
    LlmProtocolContext context;
    context.event_sink = [&](const auto& event) { events.push_back(event); return core::Status::Ok(); };
    const auto usage = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}}\n\n";
    auto decoder = Decoder(context);
    ASSERT_TRUE(decoder->Feed(Chunk({{"content", "one"}}, "stop") + usage + usage + "data: [DONE]\n\n").ok());
    ASSERT_TRUE(decoder->Finish().ok());
    EXPECT_EQ(decoder->Finish().value().total_tokens, 5);
    EXPECT_EQ(events.back().kind, LlmStreamEventKind::GenerationCompleted);
    for (std::size_t i = 0; i < events.size(); ++i) EXPECT_EQ(events[i].sequence, i + 1);
    EXPECT_EQ(Decoder()->Feed(Chunk({{"content", "truncated"}}, "length") + "data: [DONE]\n\n").code(),
              core::ErrorCode::ResourceExhausted);
}
}
