#pragma once

#include "skill_executor.h"

namespace agent::skill {

struct ToolRecallCandidate {
    std::string skill_id;
    std::string version;
};

struct SkillTurnPlan {
    std::vector<SkillManifest> manifests;
    std::vector<llm::ChatCompletionRequest::Tool> tools;
    std::vector<std::string> required_names;
    std::vector<std::string> satisfied_names;
    std::vector<std::string> degraded_reasons;
    std::vector<std::string> recalled_ids;
    llm::ChatCompletionResponse prefetch;
};

// 召回只选择候选，工具说明、名称和 schema 始终来自注册表规范版本。
core::Result<SkillTurnPlan> BuildSkillTurnPlan(
    const ISkillRegistry& registry, const std::vector<ToolRecallCandidate>& recalled,
    const SkillToolCallContext& context, std::string_view query);
core::Status AcceptSkillToolResults(SkillTurnPlan& plan,
    const llm::ChatCompletionResponse& calls, const std::vector<llm::ChatMessage>& results);
core::Status ValidateSkillRequirements(const SkillTurnPlan& plan);

}
