#pragma once

#include "skill_registry.h"
#include "../llm/llm_client.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agent::skill {

struct SkillCall {
    std::string call_id;
    // LLM 返回的 function name（解析前）；经注册表解析后才回填 skill_id / skill_version。
    std::string tool_name;
    std::string skill_id;
    std::string skill_version;
    std::string arguments_json;
};

struct SkillResult {
    std::string call_id;
    std::string skill_id;
    std::string execution_id;
    core::Status status = core::Status::Ok();
    std::string result_json;
    std::string provenance_json;
};

core::Result<std::vector<SkillCall>> ParseToolCalls(
    const llm::ChatCompletionResponse& response);
core::Result<std::vector<SkillCall>> ParseToolCalls(
    const llm::ChatCompletionResponse& response,
    const ISkillRegistry& registry);

// 校验 Skill 支持的 schema 子集；不支持的断言必须显式拒绝，不能悄悄放行。
core::Status ValidateSkillSchema(std::string_view schema_json);
core::Status ValidateSkillJsonSchema(
    std::string_view schema_json,
    std::string_view value_json);

// 将执行结果编码为下一轮 Chat Completions 所需的 tool 消息。
enum class ToolResultFormat { LegacyInline, EnvelopeV1 };
llm::ChatMessage MakeToolResultMessage(
    const SkillResult& result, ToolResultFormat format = ToolResultFormat::LegacyInline);

struct SkillPromptRequest {
    std::vector<std::string> skill_ids;
    std::string external_context;
    std::string session_state;
};

class ISkillPromptCompiler {
public:
    virtual ~ISkillPromptCompiler() = default;
    virtual core::Result<std::string> Compile(const SkillPromptRequest& request) const = 0;
};

class SkillPromptCompiler final : public ISkillPromptCompiler {
public:
    explicit SkillPromptCompiler(std::shared_ptr<const ISkillRegistry> registry);
    core::Result<std::string> Compile(const SkillPromptRequest& request) const override;

private:
    std::shared_ptr<const ISkillRegistry> registry_;
};

} // namespace agent::skill
