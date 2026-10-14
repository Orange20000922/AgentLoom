#include "skill_executor.h"

#include <condition_variable>
#include <atomic>
#include <algorithm>

namespace agent::skill {

core::Status InMemorySkillExecutorFactory::Register(
    std::string type, std::shared_ptr<ISkillExecutor> executor) {
    if (type.empty() || !executor) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "executor type and implementation are required");
    }
    std::unique_lock lock(mutex_);
    if (executors_.contains(type)) {
        return core::Status::Error(core::ErrorCode::AlreadyExists,
                                   "skill executor type already registered");
    }
    executors_.emplace(std::move(type), std::move(executor));
    return core::Status::Ok();
}

core::Result<std::shared_ptr<ISkillExecutor>> InMemorySkillExecutorFactory::Resolve(
    const SkillManifest& manifest) const {
    std::shared_lock lock(mutex_);
    auto ref = references_.find(manifest.executor.type + ":" + manifest.executor.reference);
    if (ref != references_.end()) return ref->second;
    auto it = executors_.find(manifest.executor.type);
    if (it == executors_.end()) {
        return core::Status::Error(core::ErrorCode::NotFound,
                                   "skill executor type is not registered");
    }
    return it->second;
}

SkillInvocationService::SkillInvocationService(
    std::shared_ptr<const ISkillRegistry> registry,
    std::shared_ptr<const ISkillExecutorFactory> factory,
    std::shared_ptr<service::persona::ISkillSessionManager> sessions)
    : registry_(std::move(registry)), factory_(std::move(factory)), sessions_(std::move(sessions)) {}

core::Result<service::persona::SkillSessionSnapshot> SkillInvocationService::Invoke(
    SkillExecutionRequest request, SkillExecutionCallbacks callbacks) {
    if (!registry_ || !factory_ || !sessions_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "skill invocation dependencies are not configured");
    }
    auto manifest = registry_->Require(request.call.skill_id, request.call.skill_version);
    if (!manifest.ok()) return manifest.status();
    auto executor = factory_->Resolve(manifest.value());
    if (!executor.ok()) return executor.status();
    service::persona::SkillSessionStartRequest start;
    start.execution_id = request.call.call_id;
    start.skill_id = request.call.skill_id;
    start.session_id = request.session_id;
    start.user_uuid = request.user_uuid;
    start.persona_id = request.persona_id;
    start.trace_id = request.trace_id;
    start.source = "llm";
    start.reason = "standard tool call";
    start.arguments_json = request.call.arguments_json;
    auto started = sessions_->Start(start);
    if (!started.ok()) return started.status();
    auto on_result = std::move(callbacks.on_result);
    auto on_observation = std::move(callbacks.on_observation);
    // 用 weak_ptr 捕获自身，避免回调持有裸 this：服务先于在途执行析构时不再产生悬垂引用。
    std::weak_ptr<SkillInvocationService> weak_self = weak_from_this();
    std::shared_ptr<service::persona::ISkillSessionManager> sessions = sessions_;
    callbacks.on_result = [weak_self, sessions, execution_id = started.value().execution_id,
                           session_id = request.session_id,
                           skill_id = request.call.skill_id,
                           trace_id = request.trace_id,
                           on_result = std::move(on_result)](SkillResult result) mutable {
        if (auto self = weak_self.lock()) {
            std::lock_guard lock(self->active_mutex_);
            self->active_executions_.erase(execution_id);
        }
        if (result.execution_id.empty()) result.execution_id = execution_id;
        if (result.skill_id.empty()) result.skill_id = skill_id;
        if (!result.status.ok()) {
            sessions->MarkFailed(session_id, skill_id, result.status.message(), trace_id, execution_id);
        } else {
            sessions->BeginClosing(session_id, skill_id, execution_id,
                                   "skill execution completed", trace_id);
            sessions->CompleteClosing(session_id, skill_id, execution_id,
                                      "skill execution completed", trace_id);
        }
        if (on_result) on_result(std::move(result));
    };
    callbacks.on_observation = [sessions, session_id = request.session_id,
                                skill_id = request.call.skill_id,
                                on_observation = std::move(on_observation)](
                                   service::persona::SkillObservation observation) mutable {
        if (observation.session_id.empty()) observation.session_id = session_id;
        if (observation.skill_id.empty()) observation.skill_id = skill_id;
        sessions->RecordObservation(observation);
        if (on_observation) on_observation(std::move(observation));
    };
    {
        std::lock_guard lock(active_mutex_);
        if (active_executions_.contains(started.value().execution_id)) {
            return core::Status::Error(core::ErrorCode::AlreadyExists,
                                       "skill execution id is already active");
        }
        active_executions_[started.value().execution_id] = executor.value();
    }
    auto execution = executor.value()->Start(request, std::move(callbacks));
    if (!execution.ok()) {
        std::lock_guard lock(active_mutex_);
        active_executions_.erase(started.value().execution_id);
        sessions_->MarkFailed(request.session_id, request.call.skill_id,
                              execution.status().message(), request.trace_id,
                              started.value().execution_id);
        return execution.status();
    }
    return started;
}

core::Status SkillInvocationService::Cancel(std::string_view execution_id) {
    std::shared_ptr<ISkillExecutor> executor;
    {
        std::lock_guard lock(active_mutex_);
        auto it = active_executions_.find(std::string(execution_id));
        if (it == active_executions_.end()) {
            return core::Status::Error(core::ErrorCode::NotFound,
                                       std::string("execution not found: ") + std::string(execution_id));
        }
        executor = it->second;
        active_executions_.erase(it);
    }
    return executor->Cancel(execution_id);
}

SkillToolCallCoordinator::SkillToolCallCoordinator(
    std::shared_ptr<const ISkillRegistry> registry,
    std::shared_ptr<ISkillInvocationService> invocation)
    : registry_(std::move(registry)), invocation_(std::move(invocation)) {}

namespace {
bool IsAllowedTool(std::string_view tool_name, const std::vector<std::string>& allowed) {
    return allowed.empty() || std::find(allowed.begin(), allowed.end(), tool_name) != allowed.end();
}
}

core::Result<std::vector<llm::ChatMessage>> SkillToolCallCoordinator::Execute(
    const llm::ChatCompletionResponse& response,
    const SkillToolCallContext& context) {
    if (!registry_ || !invocation_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "skill tool call coordinator is not configured");
    }
    auto calls = ParseToolCalls(response, *registry_);
    if (!calls.ok()) return calls.status();
    std::vector<llm::ChatMessage> messages;
    messages.reserve(calls.value().size());
    for (const auto& call : calls.value()) {
        if (!IsAllowedTool(call.tool_name, context.allowed_tool_names)) {
            SkillResult rejected;
            rejected.call_id = call.call_id;
            rejected.skill_id = call.skill_id;
            rejected.status = core::Status::Error(
                core::ErrorCode::PermissionDenied,
                "tool call is not allowed for this turn");
            messages.push_back(MakeToolResultMessage(rejected));
            continue;
        }
        struct CompletionState {
            std::mutex mutex;
            std::condition_variable condition;
            bool done = false;
            std::optional<SkillResult> result;
        };
        auto completion = std::make_shared<CompletionState>();
        SkillExecutionRequest request;
        request.call = call;
        request.session_id = context.session_id;
        request.user_uuid = context.user_uuid;
        request.persona_id = context.persona_id;
        request.trace_id = context.trace_id;
        SkillExecutionCallbacks callbacks;
        callbacks.on_result = [completion](SkillResult completed) {
            std::lock_guard lock(completion->mutex);
            if (!completion->done) {
                completion->result = std::move(completed);
                completion->done = true;
            }
            completion->condition.notify_one();
        };
        auto started = invocation_->Invoke(std::move(request), std::move(callbacks));
        if (!started.ok()) {
            SkillResult failed;
            failed.call_id = call.call_id;
            failed.skill_id = call.skill_id;
            failed.status = started.status();
            messages.push_back(MakeToolResultMessage(failed));
            continue;
        }
        std::unique_lock lock(completion->mutex);
        if (!completion->condition.wait_for(lock, context.timeout, [&] { return completion->done; })) {
            SkillResult timed_out;
            timed_out.call_id = call.call_id;
            timed_out.skill_id = call.skill_id;
            timed_out.execution_id = started.value().execution_id;
            timed_out.status = core::Status::Error(core::ErrorCode::Timeout,
                                                    "skill execution timed out");
            invocation_->Cancel(started.value().execution_id);
            messages.push_back(MakeToolResultMessage(timed_out));
            continue;
        }
        messages.push_back(MakeToolResultMessage(*completion->result));
    }
    return messages;
}

core::Status SkillToolCallCoordinator::ExecuteAsync(
    const llm::ChatCompletionResponse& response,
    SkillToolCallContext context,
    Completion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "skill tool call completion is required");
    }
    if (!registry_ || !invocation_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "skill tool call coordinator is not configured");
    }
    auto calls = ParseToolCalls(response, *registry_);
    if (!calls.ok()) {
        completion(calls.status());
        return core::Status::Ok();
    }

    struct State {
        std::vector<SkillCall> calls;
        SkillToolCallContext context;
        std::shared_ptr<ISkillInvocationService> invocation;
        std::vector<llm::ChatMessage> messages;
        std::size_t next = 0;
        std::function<void(core::Result<std::vector<llm::ChatMessage>>)> completion;
        std::atomic<bool> completed{false};
    };
    auto state = std::make_shared<State>();
    state->calls = std::move(calls).value();
    state->context = std::move(context);
    state->invocation = invocation_;
    state->completion = std::move(completion);
    auto advance = std::make_shared<std::function<void()>>();
    // 自引用递归用 weak_ptr 打破 shared_ptr 环：否则 advance → lambda → advance 成环，
    // ExecuteAsync 返回后状态机（State 及其 messages/completion/context）无法释放。
    std::weak_ptr<std::function<void()>> weak_advance = advance;
    *advance = [state, weak_advance]() mutable {
        auto self = weak_advance.lock();
        if (!self) return;
        if (state->next >= state->calls.size()) {
            if (!state->completed.exchange(true, std::memory_order_acq_rel)) {
                state->completion(std::move(state->messages));
            }
            return;
        }
        const auto call = state->calls[state->next++];
        if (!IsAllowedTool(call.tool_name, state->context.allowed_tool_names)) {
            SkillResult rejected;
            rejected.call_id = call.call_id;
            rejected.skill_id = call.skill_id;
            rejected.status = core::Status::Error(
                core::ErrorCode::PermissionDenied,
                "tool call is not allowed for this turn");
            state->messages.push_back(MakeToolResultMessage(rejected));
            (*self)();
            return;
        }
        SkillExecutionRequest request;
        request.call = call;
        request.session_id = state->context.session_id;
        request.user_uuid = state->context.user_uuid;
        request.persona_id = state->context.persona_id;
        request.trace_id = state->context.trace_id;
        auto call_finished = std::make_shared<std::atomic<bool>>(false);
        SkillExecutionCallbacks callbacks;
        // on_result 持有 self 强引用，保证执行器回调完成前状态机存活（与上方 weak_ptr 配合无环）。
        callbacks.on_result = [state, self, call_finished](SkillResult result) mutable {
            if (call_finished->exchange(true, std::memory_order_acq_rel)) return;
            state->messages.push_back(MakeToolResultMessage(result));
            (*self)();
        };
        auto started = state->invocation->Invoke(std::move(request), std::move(callbacks));
        if (!started.ok()) {
            if (call_finished->exchange(true, std::memory_order_acq_rel)) return;
            SkillResult result;
            result.call_id = call.call_id;
            result.skill_id = call.skill_id;
            result.status = started.status();
            state->messages.push_back(MakeToolResultMessage(result));
            (*self)();
        }
    };
    (*advance)();
    return core::Status::Ok();
}

core::Status InMemorySkillExecutorFactory::RegisterReference(
    std::string type, std::string reference, std::shared_ptr<ISkillExecutor> executor) {
    if (type.empty() || reference.empty() || !executor) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "executor reference is required");
    }
    std::unique_lock lock(mutex_);
    const auto key = type + ":" + reference;
    if (references_.contains(key)) return core::Status::Error(core::ErrorCode::AlreadyExists, "executor reference already registered");
    references_.emplace(std::move(key), std::move(executor));
    return core::Status::Ok();
}

} // namespace agent::skill
