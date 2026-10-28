#include "skill_turn_plan.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_set>

namespace agent::skill {
core::Result<SkillTurnPlan> BuildSkillTurnPlan(
    const ISkillRegistry& registry, const std::vector<ToolRecallCandidate>& recalled,
    const SkillToolCallContext& context, std::string_view) {
    try {
        SkillTurnPlan plan;
        std::unordered_set<std::string> selected;
        for (const auto& hit : recalled) {
            plan.recalled_ids.push_back(hit.skill_id);
            auto manifest = registry.Find(hit.skill_id, hit.version);
            if (!manifest.ok()) return manifest.status();
            if (!manifest.value()) {
                plan.degraded_reasons.push_back("stale_or_unregistered_tool:" + hit.skill_id);
                continue;
            }
            if (context.policy) {
                auto status = context.policy->AuthorizeDeclaration(context, *manifest.value());
                if (!status.ok()) {
                    plan.degraded_reasons.push_back("tool_declaration_denied:" + hit.skill_id);
                    continue;
                }
            }
            if (!selected.insert(manifest.value()->skill_id).second) continue;
            plan.tools.push_back({manifest.value()->tool_name, manifest.value()->description,
                manifest.value()->input_schema_json});
            plan.manifests.push_back(*manifest.value());
        }
        return plan;
    } catch (const std::exception& error) {
        return core::Status::Error(core::ErrorCode::InternalError, error.what());
    }
}

core::Status AcceptSkillToolResults(SkillTurnPlan& plan,
    const llm::ChatCompletionResponse& calls, const std::vector<llm::ChatMessage>& results) {
    for (const auto& call : calls.tool_calls) {
        if (std::find(plan.required_names.begin(), plan.required_names.end(), call.name) == plan.required_names.end()) continue;
        const auto result = std::find_if(results.begin(), results.end(), [&](const auto& message) {
            return message.tool_call_id == call.id;
        });
        if (result == results.end()) return core::Status::Error(core::ErrorCode::DataLoss, "required tool result missing");
        // 必需工具使用 EnvelopeV1，避免把业务 JSON 的 error 字段误判成控制错误。
        try {
            const auto body = nlohmann::json::parse(result->content);
            if (body.value("schema_version", "") != "agentloom.tool-result.v1")
                return core::Status::Error(core::ErrorCode::DataLoss, "required tool result envelope missing");
            if (body.value("call_id", "") != call.id)
                return core::Status::Error(core::ErrorCode::DataLoss, "required tool result identity mismatch");
            if (body.value("status", "") == "error") {
                const auto code = body.at("error").value("code", static_cast<int>(core::ErrorCode::FailedPrecondition));
                return core::Status::Error(code > 0 && code <= static_cast<int>(core::ErrorCode::DataLoss)
                    ? static_cast<core::ErrorCode>(code) : core::ErrorCode::FailedPrecondition,
                    "required tool failed [source=tool_result]: " + call.name + ": " + body.at("error").value("message", "unknown"));
            }
            if (body.value("status", "") != "success" && body.value("status", "") != "empty")
                return core::Status::Error(core::ErrorCode::DataLoss, "required tool result status invalid");
        } catch (const std::exception&) {
            return core::Status::Error(core::ErrorCode::DataLoss, "invalid required tool result");
        }
        if (std::find(plan.satisfied_names.begin(), plan.satisfied_names.end(), call.name) == plan.satisfied_names.end())
            plan.satisfied_names.push_back(call.name);
    }
    return core::Status::Ok();
}

core::Status ValidateSkillRequirements(const SkillTurnPlan& plan) {
    for (const auto& name : plan.required_names) {
        if (std::find(plan.satisfied_names.begin(), plan.satisfied_names.end(), name) == plan.satisfied_names.end())
            return core::Status::Error(core::ErrorCode::FailedPrecondition, "required tool was not called: " + name);
    }
    return core::Status::Ok();
}
}
