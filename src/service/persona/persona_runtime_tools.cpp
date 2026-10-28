#include "persona_runtime.h"
#include <nlohmann/json.hpp>
#include <iterator>

namespace agent::service::persona {
agent::skill::SkillToolCallContext PersonaRuntime::ToolCallContext(
    const SessionState& session, const PreparedChat& prepared) const {
    agent::skill::SkillToolCallContext context;
    context.session_id = session.session_id;
    context.user_uuid = session.user_uuid;
    context.persona_id = session.persona_id;
    context.trace_id = prepared.request.trace_id;
    context.allowed_tool_names.emplace();
    for (const auto& tool : prepared.tools) context.allowed_tool_names->push_back(tool.name);
    context.max_result_bytes = options_.max_tool_result_bytes;
    context.result_format = prepared.tool_plan.required_names.empty()
        ? options_.tool_result_format : agent::skill::ToolResultFormat::EnvelopeV1;
    context.policy = options_.skill_tool_policy;
    context.deadline_scheduler = options_.skill_deadline_scheduler;
    context.cancellation = prepared.cancellation;
    context.required_tool_names = prepared.tool_plan.required_names;
    return context;
}

core::Status PersonaRuntime::ResolveToolPlan(const SessionState& session,
    PreparedChat& prepared, const ToolMemoryContext& recalled) const {
    if (options_.trusted_context_provider) {
        auto trusted = options_.trusted_context_provider->GetContext({session.tenant_id, session.user_uuid,
            session.session_id, session.persona_id, prepared.request.trace_id});
        if (!trusted.ok()) return trusted.status();
        for (auto& item : trusted.value()) {
            item.source = ContextSource::Trusted;
            item.required = true;
            prepared.memory.items.push_back(std::move(item));
        }
    }
    if (!options_.skill_registry) return core::Status::Ok();
    std::vector<agent::skill::ToolRecallCandidate> candidates;
    for (const auto& hit : recalled.hits) candidates.push_back({hit.tool_id, hit.skill_version});
    // 兼容仅返回 tools 的旧 provider，但内容仍从规范注册表读取。
    if (candidates.empty()) for (const auto& tool : recalled.tools) {
        auto manifest = options_.skill_registry->RequireByToolName(tool.name);
        if (manifest.ok()) candidates.push_back({manifest.value().skill_id, manifest.value().version});
    }
    auto plan = agent::skill::BuildSkillTurnPlan(*options_.skill_registry, candidates,
        ToolCallContext(session, prepared), prepared.request.user_input);
    if (!plan.ok()) {
        logger_.warn("[trace={}] tool planning failed reason={}", prepared.request.trace_id, plan.status().message());
        return plan.status();
    }
    prepared.tool_plan = std::move(plan).value();
    prepared.tool_plan.degraded_reasons.insert(prepared.tool_plan.degraded_reasons.end(),
        recalled.degraded_reasons.begin(), recalled.degraded_reasons.end());
    if (!recalled.hit && recalled.degraded_reasons.empty()) prepared.tool_plan.degraded_reasons.push_back("recall_empty");
    prepared.tools = prepared.tool_plan.tools;
    nlohmann::json instructions = nlohmann::json::array();
    for (const auto& manifest : prepared.tool_plan.manifests) {
        instructions.push_back({{"skill_id", manifest.skill_id}, {"version", manifest.version},
            {"tool_name", manifest.tool_name}, {"instruction", manifest.prompt_instruction},
            {"provenance", {{"source", "skill_registry"}, {"uncertainty", "not_applicable"}}}});
    }
    if (!instructions.empty()) {
        prepared.memory.items.push_back({ContextSource::L4, "registered-tools",
            "<registered_tools>\n" + instructions.dump() + "\n</registered_tools>", "{}", true, true});
    }
    return core::Status::Ok();
}

LlmCallObservation PersonaRuntime::MakeObservation(const PreparedChat& prepared) const {
    LlmCallObservation item;
    item.trace_id = prepared.request.trace_id;
    item.tool_round = prepared.tool_round;
    item.context = prepared.memory.budget_report;
    item.max_tokens = prepared.generation.max_tokens;
    item.required_tools = prepared.tool_plan.required_names;
    item.satisfied_tools = prepared.tool_plan.satisfied_names;
    item.tools_recalled = prepared.tool_plan.recalled_ids;
    item.degraded_reasons = prepared.tool_plan.degraded_reasons;
    for (const auto& tool : prepared.tools) {
        item.tools_declared.push_back(tool.name);
        item.tool_schema_bytes += tool.name.size() + tool.description.size() + tool.parameters_json.size();
    }
    return item;
}
void PersonaRuntime::ObservePrompt(const PreparedChat& prepared) const noexcept {
    try {
        auto item = MakeObservation(prepared);
        logger_.info("[trace={}] PromptReady round={} tools_declared={} tools_recalled={} required_tools={} degraded_reason={} schema_bytes={} max_tokens={}",
            item.trace_id, item.tool_round, nlohmann::json(item.tools_declared).dump(), nlohmann::json(item.tools_recalled).dump(),
            nlohmann::json(item.required_tools).dump(), nlohmann::json(item.degraded_reasons).dump(), item.tool_schema_bytes, item.max_tokens);
        if (options_.observer) options_.observer->PromptReady(item);
    } catch (...) { logger_.warn("runtime prompt observer failed"); }
}
void PersonaRuntime::ObserveTurn(const PreparedChat& prepared, core::Status status) const noexcept {
    try {
        auto item = MakeObservation(prepared);
        item.status = std::move(status);
        for (const auto& call : prepared.llm_calls) {
            item.prompt_tokens += call.prompt_tokens;
            item.completion_tokens += call.completion_tokens;
            item.total_tokens += call.total_tokens;
            item.finish_reason = call.finish_reason;
            item.tool_calls.insert(item.tool_calls.end(), call.tool_calls.begin(), call.tool_calls.end());
        }
        logger_.info("[trace={}] TurnCompleted required_tools={} mandatory_called={} tool_calls={} degraded_reason={} code={} reason={}",
            item.trace_id, nlohmann::json(item.required_tools).dump(), nlohmann::json(item.satisfied_tools).dump(),
            nlohmann::json(item.tool_calls).dump(), nlohmann::json(item.degraded_reasons).dump(),
            static_cast<int>(item.status.code()), item.status.message());
        if (options_.observer) options_.observer->TurnCompleted(item);
    } catch (...) { logger_.warn("runtime turn observer failed"); }
}
core::Status PersonaRuntime::CheckToolRequirements(const PreparedChat& prepared) const {
    auto status = agent::skill::ValidateSkillRequirements(prepared.tool_plan);
    if (!status.ok()) ObserveTurn(prepared, status);
    return status;
}
void PersonaRuntime::ObserveLlmResult(PreparedChat& prepared,
    const llm::ChatCompletionResponse* response, core::Status status) const noexcept {
    try {
        auto item = MakeObservation(prepared);
        item.status = std::move(status);
        if (response) {
            for (const auto& call : response->tool_calls) item.tool_calls.push_back(call.name);
            item.finish_reason = response->finish_reason;
            item.prompt_tokens = response->prompt_tokens;
            item.completion_tokens = response->completion_tokens;
            item.total_tokens = response->total_tokens;
        }
        prepared.llm_calls.push_back(item);
        logger_.info("[trace={}] LlmCompleted round={} tool_calls={} finish_reason={} prompt_tokens={} completion_tokens={} code={}",
            item.trace_id, item.tool_round, nlohmann::json(item.tool_calls).dump(), item.finish_reason, item.prompt_tokens, item.completion_tokens,
            static_cast<int>(item.status.code()));
        if (options_.observer) options_.observer->LlmCompleted(item);
    } catch (...) { logger_.warn("runtime completion observer failed"); }
}

core::Status PersonaRuntime::AppendToolResults(PreparedChat& prepared,
    const llm::ChatCompletionResponse& calls, std::vector<llm::ChatMessage> messages) const {
    const auto source = static_cast<std::size_t>(ContextSource::ToolResult);
    auto& report = prepared.memory.budget_report;
    for (const auto& message : messages) {
        ++report.counts[source];
        report.input_bytes[source] += message.content.size();
        const auto limit = options_.context_budgets[source].max_bytes;
        // tool 协议消息必须与 call 成对；超限不能仅丢掉部分结果。
        if (message.content.size() > limit - std::min(limit, report.retained_bytes[source])) {
            auto status = core::Status::Error(core::ErrorCode::ResourceExhausted, "context source budget exceeded: tool_result");
            ObserveTurn(prepared, status);
            return status;
        }
        report.retained_bytes[source] += message.content.size();
    }
    auto status = agent::skill::AcceptSkillToolResults(prepared.tool_plan, calls, messages);
    if (!status.ok()) {
        ObserveTurn(prepared, status);
        logger_.warn("[trace={}] required tool rejected reason={}", prepared.request.trace_id, status.message());
        return status;
    }
    prepared.messages.insert(prepared.messages.end(), std::make_move_iterator(messages.begin()),
        std::make_move_iterator(messages.end()));
    auto budget = ValidatePromptCharacterBudget(prepared.messages, prepared.tools,
        prepared.request.session_id, prepared.request.trace_id);
    if (!budget.ok()) ObserveTurn(prepared, budget);
    return budget;
}
}
