#include "agent_task_runtime.h"
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace agent::runtime {
skill::SkillManifest SubagentSkillManifest() {
    skill::SkillManifest manifest;
    manifest.skill_id = "agent.spawn";
    manifest.tool_name = "spawn_agent";
    manifest.version = "1";
    manifest.description = "Delegate a bounded research or analysis task to a child agent and wait for its summary.";
    manifest.prompt_instruction = "Delegate only a self-contained task with a clear expected result. "
        "The child receives only host-approved tools and context. timeout_ms is a requested upper bound; "
        "timeouts and failures are returned as tool errors. Do not treat a child summary as verified facts without its evidence.";
    manifest.input_schema_json = R"({"type":"object","properties":{"task":{"type":"string","minLength":1,"maxLength":32768},"timeout_ms":{"type":"integer","minimum":1,"maximum":3600000}},"required":["task","timeout_ms"],"additionalProperties":false})";
    manifest.output_schema_json = R"({"type":"object","properties":{"run_id":{"type":"string"},"summary":{"type":"string"},"model_requests":{"type":"integer"},"tool_calls":{"type":"integer"},"total_tokens":{"type":"integer"}},"required":["run_id","summary","model_requests","tool_calls","total_tokens"],"additionalProperties":false})";
    manifest.exposure = skill::ToolExposure::Always;
    manifest.executor.reference = "agent.spawn";
    return manifest;
}

struct SubagentSkillExecutor::State {
    struct Entry {
        std::shared_ptr<IAgentTaskLease> lease;
        bool cancelled = false;
    };
    // 宿主拥有运行时；允许同一 factory 注册 spawn，避免 runtime→factory→executor→runtime 成环。
    std::weak_ptr<IAgentTaskRuntime> runtime;
    std::shared_ptr<const ISubagentContextProvider> context;
    std::mutex mutex;
    bool closed = false;
    std::unordered_map<std::string, Entry> active;
    std::shared_ptr<IAgentTaskLease> Remove(std::string_view id) {
        std::lock_guard lock(mutex);
        auto found = active.find(std::string(id));
        if (found == active.end()) return {};
        auto lease = std::move(found->second.lease);
        active.erase(found);
        return lease;
    }
};
SubagentSkillExecutor::SubagentSkillExecutor(std::shared_ptr<IAgentTaskRuntime> runtime,
    std::shared_ptr<const ISubagentContextProvider> context) : state_(std::make_shared<State>()) {
    state_->runtime = std::move(runtime);
    state_->context = std::move(context);
}
SubagentSkillExecutor::~SubagentSkillExecutor() {
    std::vector<std::shared_ptr<IAgentTaskLease>> leases;
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        for (auto& [id, entry] : state_->active) {
            entry.cancelled = true;
            if (entry.lease) leases.push_back(entry.lease);
        }
    }
    for (const auto& lease : leases) lease->Cancel();
}
core::Result<service::persona::SkillSessionSnapshot> SubagentSkillExecutor::Start(
    const skill::SkillExecutionRequest& request, skill::SkillExecutionCallbacks callbacks) try {
    auto runtime = state_->runtime.lock();
    if (!runtime || !state_->context || !callbacks.on_result || request.execution_id.empty())
        return core::Status::Error(core::ErrorCode::InvalidArgument, "subagent executor dependencies or execution ID missing");
    if (auto status = skill::ValidateSkillJsonSchema(SubagentSkillManifest().input_schema_json,
        request.call.arguments_json); !status.ok()) return status;
    auto prepared = state_->context->Prepare(request);
    if (!prepared.ok()) return prepared.status();
    auto task = std::move(prepared).value();
    const auto args = nlohmann::json::parse(request.call.arguments_json);
    task.task = args.at("task").get<std::string>();
    task.timeout = std::min(task.timeout, std::chrono::milliseconds(args.at("timeout_ms").get<std::int64_t>()));
    // 身份来自已授权调用；绑定器只补充权限与快照，不允许模型重写 Session/trace。
    task.context.session_id = request.session_id;
    task.context.user_uuid = request.user_uuid;
    task.context.persona_id = request.persona_id;
    task.context.trace_id = request.trace_id;
    service::persona::SkillSessionSnapshot snapshot;
    snapshot.execution_id = request.execution_id;
    snapshot.session_id = request.session_id;
    snapshot.skill_id = request.call.skill_id;
    snapshot.state = service::persona::SkillSessionState::Running;
    auto state = state_;
    {
        std::lock_guard lock(state->mutex);
        if (state->closed) return core::Status::Error(core::ErrorCode::Unavailable, "subagent executor closed");
        if (!state->active.emplace(request.execution_id, State::Entry{}).second)
            return core::Status::Error(core::ErrorCode::AlreadyExists, "subagent execution already active");
    }
    auto lease = runtime->Start(std::move(task),
        [state, request, complete = std::move(callbacks.on_result)](core::Result<AgentTaskResult> outcome) mutable {
            std::shared_ptr<IAgentTaskLease> released;
            {
                std::lock_guard lock(state->mutex);
                auto found = state->active.find(request.execution_id);
                if (found == state->active.end()) return;
                released = std::move(found->second.lease);
                state->active.erase(found);
            }
            skill::SkillResult result;
            result.call_id = request.call.call_id;
            result.skill_id = request.call.skill_id;
            result.execution_id = request.execution_id;
            if (!outcome.ok()) result.status = outcome.status();
            else try {
                const auto& value = outcome.value();
                result.result_json = nlohmann::json{{"run_id", value.run_id}, {"summary", value.summary},
                    {"model_requests", value.model_requests}, {"tool_calls", value.tool_calls},
                    {"total_tokens", value.total_tokens}}.dump();
            } catch (const std::exception& exception) {
                result.status = core::Status::Error(core::ErrorCode::InternalError, exception.what());
                core::LoggerAdapter::ForModule("agent-task").warn("subagent result encoding failed: {}", exception.what());
            }
            complete(std::move(result));
        });
    if (!lease.ok()) {
        std::lock_guard lock(state->mutex);
        state->active.erase(request.execution_id);
        return lease.status();
    }
    bool cancel = false;
    {
        std::lock_guard lock(state->mutex);
        auto found = state->active.find(request.execution_id);
        if (found != state->active.end()) {
            found->second.lease = lease.value();
            cancel = found->second.cancelled;
        }
    }
    if (cancel) lease.value()->Cancel();
    return snapshot;
} catch (const std::exception& exception) {
    core::LoggerAdapter::ForModule("agent-task").warn("subagent submission failed: {}", exception.what());
    auto released = state_->Remove(request.execution_id);
    return core::Status::Error(core::ErrorCode::InternalError, exception.what());
} catch (...) {
    core::LoggerAdapter::ForModule("agent-task").warn("subagent submission failed");
    auto released = state_->Remove(request.execution_id);
    return core::Status::Error(core::ErrorCode::InternalError, "subagent submission threw");
}
core::Status SubagentSkillExecutor::Cancel(std::string_view execution_id) {
    std::shared_ptr<IAgentTaskLease> lease;
    {
        std::lock_guard lock(state_->mutex);
        auto found = state_->active.find(std::string(execution_id));
        if (found == state_->active.end()) return core::Status::Ok();
        found->second.cancelled = true;
        lease = found->second.lease;
    }
    if (lease) lease->Cancel();
    return core::Status::Ok();
}
}
