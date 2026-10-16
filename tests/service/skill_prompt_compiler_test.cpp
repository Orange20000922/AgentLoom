#include "../../src/skill/skill_prompt_compiler.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

TEST(SkillPromptCompilerTest, EmitsStaticProtocolAndManifest) {
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    agent::skill::SkillManifest m;
    m.skill_id = "vision.observe";
    m.version = "1.0.0";
    m.description = "observe";
    m.prompt_instruction = "call when observation is required";
    m.executor.reference = m.skill_id;
    ASSERT_TRUE(registry->Register(m).ok());
    agent::skill::SkillPromptCompiler compiler(registry);
    auto prompt = compiler.Compile({{"vision.observe"}, {}, {}});
    ASSERT_TRUE(prompt.ok()) << prompt.status().message();
    EXPECT_NE(prompt.value().find("skill_tool_protocol"), std::string::npos);
    EXPECT_NE(prompt.value().find("vision.observe"), std::string::npos);
    EXPECT_NE(prompt.value().find("input_schema"), std::string::npos);
}

TEST(SkillCallParserTest, ParsesStandardToolCallAndValidatesRequiredArgument) {
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    agent::skill::SkillManifest manifest;
    manifest.skill_id = "vision.observe";
    manifest.version = "1.0.0";
    manifest.input_schema_json = R"({"type":"object","required":["reason"]})";
    manifest.executor.reference = manifest.skill_id;
    ASSERT_TRUE(registry->Register(manifest).ok());
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"call-1", "vision.observe", R"({"reason":"look"})"});
    auto calls = agent::skill::ParseToolCalls(response, *registry);
    ASSERT_TRUE(calls.ok()) << calls.status().message();
    ASSERT_EQ(calls.value().size(), 1u);
    EXPECT_EQ(calls.value()[0].skill_version, "1.0.0");
}

TEST(SkillCallParserTest, RejectsMissingRequiredArgument) {
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = agent::skill::SkillManifest{};
    manifest.skill_id = "vision.observe";
    manifest.version = "1.0.0";
    manifest.input_schema_json = R"({"type":"object","required":["reason"]})";
    manifest.executor.reference = manifest.skill_id;
    ASSERT_TRUE(registry->Register(manifest).ok());
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"call-1", "vision.observe", "{}"});
    EXPECT_EQ(agent::skill::ParseToolCalls(response, *registry).status().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(SkillCallParserTest, RejectsArgumentTypeAndUnexpectedField) {
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = agent::skill::SkillManifest{};
    manifest.skill_id = "graph.constraint";
    manifest.version = "1.0.0";
    manifest.input_schema_json =
        R"({"type":"object","properties":{"reason":{"type":"string"}},)"
        R"("required":["reason"],"additionalProperties":false})";
    manifest.executor.reference = manifest.skill_id;
    ASSERT_TRUE(registry->Register(manifest).ok());

    agent::llm::ChatCompletionResponse wrong_type;
    wrong_type.tool_calls.push_back(
        {"call-type", manifest.skill_id, R"({"reason":42})"});
    EXPECT_EQ(agent::skill::ParseToolCalls(wrong_type, *registry).status().code(),
              core::ErrorCode::InvalidArgument);

    agent::llm::ChatCompletionResponse unexpected_field;
    unexpected_field.tool_calls.push_back(
        {"call-extra", manifest.skill_id, R"({"reason":"look","extra":true})"});
    EXPECT_EQ(agent::skill::ParseToolCalls(unexpected_field, *registry).status().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(SkillCallParserTest, RejectsMissingToolCallId) {
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = agent::skill::SkillManifest{};
    manifest.skill_id = "vision.observe";
    manifest.version = "1.0.0";
    manifest.executor.reference = manifest.skill_id;
    ASSERT_TRUE(registry->Register(manifest).ok());
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"", manifest.skill_id, "{}"});
    EXPECT_EQ(agent::skill::ParseToolCalls(response, *registry).status().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(SkillCallParserTest, ValidatesOutputSchemaUsingManifestSubset) {
    EXPECT_TRUE(agent::skill::ValidateSkillJsonSchema(
        R"({"type":"object","properties":{"count":{"type":"integer","minimum":1}},"required":["count"],"additionalProperties":false})",
        R"({"count":3})").ok());
    EXPECT_EQ(agent::skill::ValidateSkillJsonSchema(
        R"({"type":"object","properties":{"count":{"type":"integer","minimum":1}},"required":["count"],"additionalProperties":false})",
        R"({"count":"3"})").code(), core::ErrorCode::InvalidArgument);
}

TEST(SkillCallParserTest, RejectsDuplicateIdsInvalidUtf8AndOversizedArguments) {
    agent::llm::ChatCompletionResponse response;
    response.tool_calls = {{"same", "tool", "{}"}, {"same", "tool", "{}"}};
    EXPECT_EQ(agent::skill::ParseToolCalls(response).status().code(), core::ErrorCode::InvalidArgument);
    response.tool_calls = {{"id", std::string(1, static_cast<char>(0xff)), "{}"}};
    EXPECT_EQ(agent::skill::ParseToolCalls(response).status().code(), core::ErrorCode::InvalidArgument);
    response.tool_calls = {{"id", "tool", std::string(65537, ' ')}};
    EXPECT_EQ(agent::skill::ParseToolCalls(response).status().code(), core::ErrorCode::ResourceExhausted);
}

TEST(SkillCallParserTest, EnforcesNestedSchemaBoundsAndRejectsUnsupportedAssertions) {
    const std::string schema = R"({"type":"object","properties":{"items":{"type":"array","minItems":1,"maxItems":2,"items":{"type":"string","enum":["销售","客户"],"maxLength":2}},"score":{"type":"number","minimum":0,"maximum":1}},"required":["items","score"],"additionalProperties":false})";
    EXPECT_TRUE(agent::skill::ValidateSkillJsonSchema(schema, R"({"items":["销售"],"score":0.5})").ok());
    for (const auto& value : {R"({"items":[],"score":0.5})", R"({"items":["销售","客户","销售"],"score":0.5})",
                             R"({"items":["其他"],"score":0.5})", R"({"items":["销售"],"score":2})"}) {
        EXPECT_FALSE(agent::skill::ValidateSkillJsonSchema(schema, value).ok()) << value;
    }
    EXPECT_TRUE(agent::skill::ValidateSkillJsonSchema(
        R"({"additionalProperties":{"type":"integer"}})", R"({"x":1})").ok());
    EXPECT_FALSE(agent::skill::ValidateSkillJsonSchema(
        R"({"additionalProperties":{"type":"integer"}})", R"({"x":"1"})").ok());
    EXPECT_FALSE(agent::skill::ValidateSkillSchema(R"({"pattern":"^trusted$"})").ok());
    EXPECT_FALSE(agent::skill::ValidateSkillSchema(R"({"properties":{"unused":{"minimum":"bad"}}})").ok());
}

TEST(SkillPromptCompilerTest, BuildsToolResultMessageForFollowUpTurn) {
    agent::skill::SkillResult result;
    result.call_id = "call-1";
    result.skill_id = "vision.observe";
    result.result_json = R"({"observed":true})";
    auto message = agent::skill::MakeToolResultMessage(result);
    EXPECT_EQ(message.role, agent::llm::ChatRole::Tool);
    EXPECT_EQ(message.tool_call_id, "call-1");
    EXPECT_EQ(message.content, R"({"observed":true})");
}

TEST(SkillPromptCompilerTest, AddsProvenanceWithoutChangingResultFields) {
    agent::skill::SkillResult result;
    result.call_id = "call-provenance";
    result.skill_id = "graph.constraint";
    result.result_json = R"({"constraints_text":"approved"})";
    result.provenance_json = R"({"kb_version":"kb-42","evidence_state":"POSITIVE"})";
    const auto message = agent::skill::MakeToolResultMessage(result);
    const auto body = nlohmann::json::parse(message.content);
    EXPECT_EQ(body["constraints_text"], "approved");
    EXPECT_EQ(body["provenance"]["kb_version"], "kb-42");
    EXPECT_EQ(body["provenance"]["evidence_state"], "POSITIVE");
}

TEST(SkillPromptCompilerTest, RejectsMalformedOrConflictingProvenance) {
    agent::skill::SkillResult result;
    result.call_id = "call-source";
    result.result_json = R"({"provenance":{"version":"old"},"private":"must-not-appear"})";
    for (const auto& provenance : {"{", "[]", R"({"version":"new"})"}) {
        result.provenance_json = provenance;
        const auto message = agent::skill::MakeToolResultMessage(result);
        const auto body = nlohmann::json::parse(message.content);
        EXPECT_EQ(body["error"]["code"], static_cast<int>(core::ErrorCode::DataLoss));
        EXPECT_EQ(message.content.find("must-not-appear"), std::string::npos);
    }
}

TEST(SkillPromptCompilerTest, VersionedEnvelopeDistinguishesSuccessEmptyAndFailure) {
    agent::skill::SkillResult result;
    result.call_id = "call-envelope";
    result.skill_id = "graph.constraint";
    const auto encode = [&] {
        return nlohmann::json::parse(agent::skill::MakeToolResultMessage(
            result, agent::skill::ToolResultFormat::EnvelopeV1).content);
    };
    EXPECT_EQ(encode()["status"], "empty");
    result.result_json = R"({"value":1})";
    result.provenance_json = R"({"version":"1"})";
    auto success = encode();
    EXPECT_EQ(success["schema_version"], "agentloom.tool-result.v1");
    EXPECT_EQ(success["status"], "success");
    EXPECT_EQ(success["provenance"]["version"], "1");
    result.status = core::Status::Error(core::ErrorCode::Unavailable, std::string(1, static_cast<char>(0xff)));
    auto failed = encode();
    EXPECT_EQ(failed["status"], "error");
    EXPECT_FALSE(failed.contains("result"));
}
