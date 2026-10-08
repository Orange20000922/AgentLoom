#pragma once

#include "skill_registry.h"
#include "../llm/llm_client.h"

#include <memory>
#include <string>
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

// 将执行结果编码为下一轮 Chat Completions 所需的 tool 消息。
llm::ChatMessage MakeToolResultMessage(const SkillResult& result);

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
