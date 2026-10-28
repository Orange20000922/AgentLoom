#include "skill_turn_plan.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_set>
#include <atomic>

namespace agent::skill {
core::Result<SkillTurnPlan> BuildSkillTurnPlan(
    const ISkillRegistry& registry, const std::vector<ToolRecallCandidate>& recalled,
    const SkillToolCallContext& context, std::string_view query) {
    try {
    SkillTurnPlan plan;
    static std::atomic<std::uint64_t> next_prefetch_id{1};
    auto catalog = registry.List();
    if (!catalog.ok()) return catalog.status();
    std::vector<SkillToolRequirement> required;
    if (context.policy) {
        auto result = context.policy->RequiredTools(context);
        if (!result.ok()) return result.status();
        required = std::move(result).value();
    }
    std::unordered_set<std::string> selected;
    auto add = [&](const SkillManifest& manifest) -> core::Status {
        if (selected.insert(manifest.skill_id).second) plan.manifests.push_back(manifest);
        else if (std::none_of(plan.manifests.begin(), plan.manifests.end(), [&](const auto& item) {
            return item.skill_id == manifest.skill_id && item.version == manifest.version;
        })) return core::Status::Error(core::ErrorCode::FailedPrecondition,
            "conflicting registered versions for mandatory tool: " + manifest.skill_id);
        return core::Status::Ok();
    };
    for (const auto& item : required) {
        auto manifest = registry.Require(item.skill_id, item.version);
        if (!manifest.ok()) return manifest.status();
        if (auto status = add(manifest.value()); !status.ok()) return status;
        plan.required_names.push_back(manifest.value().tool_name);
    }
    for (const auto& manifest : catalog.value()) {
        if (manifest.exposure == ToolExposure::Always) {
            if (auto status = add(manifest); !status.ok()) return status;
        }
    }
    for (const auto& hit : recalled) {
        plan.recalled_ids.push_back(hit.skill_id);
        auto manifest = registry.Find(hit.skill_id, hit.version);
        if (!manifest.ok()) return manifest.status();
        if (!manifest.value()) {
            plan.degraded_reasons.push_back("stale_or_unregistered_tool:" + hit.skill_id);
            continue;
        }
        if (selected.contains(manifest.value()->skill_id) &&
            std::none_of(plan.manifests.begin(), plan.manifests.end(), [&](const auto& selected_manifest) {
                return selected_manifest.skill_id == manifest.value()->skill_id && selected_manifest.version == manifest.value()->version;
            })) {
            plan.degraded_reasons.push_back("recalled_version_conflicts_with_required_tool:" + hit.skill_id);
            continue;
        }
        if (auto status = add(*manifest.value()); !status.ok()) return status;
    }
    for (const auto& manifest : plan.manifests) {
        if (context.policy) {
            auto authorization = context.policy->AuthorizeDeclaration(context, manifest);
            if (!authorization.ok()) {
                if (manifest.exposure == ToolExposure::Always ||
                    std::find(plan.required_names.begin(), plan.required_names.end(), manifest.tool_name) != plan.required_names.end())
                    return authorization;
                plan.degraded_reasons.push_back("tool_declaration_denied:" + manifest.skill_id);
                continue;
            }
        }
        plan.tools.push_back({manifest.tool_name, manifest.description, manifest.input_schema_json});
        if (manifest.execution != ToolExecution::Prefetch) continue;
        if (!context.policy) return core::Status::Error(core::ErrorCode::FailedPrecondition,
            "prefetch requires a trusted tool policy and argument binder");
        auto arguments = context.policy->BindPrefetchArguments(context, manifest, query);
        if (!arguments.ok()) return arguments.status();
        if (auto status = ValidateSkillJsonSchema(manifest.input_schema_json, arguments.value()); !status.ok())
            return status;
        llm::ChatToolCall call;
        call.id = "prefetch-" + std::to_string(next_prefetch_id.fetch_add(1));
        call.name = manifest.tool_name;
        call.arguments_json = std::move(arguments).value();
        plan.prefetch.tool_calls.push_back(std::move(call));
        if (std::find(plan.required_names.begin(), plan.required_names.end(), manifest.tool_name) == plan.required_names.end())
            plan.required_names.push_back(manifest.tool_name);
    }
    std::erase_if(plan.manifests, [&](const auto& manifest) {
        return std::none_of(plan.tools.begin(), plan.tools.end(), [&](const auto& tool) { return tool.name == manifest.tool_name; });
    });
    return plan;
    } catch (const std::exception& error) {
        return core::Status::Error(core::ErrorCode::InternalError, std::string("tool planner failed: ") + error.what());
    } catch (...) {
        return core::Status::Error(core::ErrorCode::InternalError, "tool planner failed");
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
