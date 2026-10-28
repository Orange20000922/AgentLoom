#include "skill_executor.h"

#include <condition_variable>
#include <atomic>
#include <algorithm>
#include "../core/text_validation.h"
#include "../core/logger_adapter.h"
#include <exception>

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
    static std::atomic<std::uint64_t> next_execution{1};
    start.execution_id = "skill-execution-" + std::to_string(next_execution.fetch_add(1));
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
    request.execution_id = started.value().execution_id;
    auto on_result = std::move(callbacks.on_result);
    auto on_observation = std::move(callbacks.on_observation);
    auto terminal = std::make_shared<std::atomic<bool>>(false);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    // 用 weak_ptr 捕获自身，避免回调持有裸 this：服务先于在途执行析构时不再产生悬垂引用。
    std::weak_ptr<SkillInvocationService> weak_self = weak_from_this();
    std::shared_ptr<service::persona::ISkillSessionManager> sessions = sessions_;
    callbacks.on_result = [weak_self, sessions, execution_id = started.value().execution_id,
                           session_id = request.session_id,
                           skill_id = request.call.skill_id,
                           trace_id = request.trace_id,
                           terminal, cancelled, on_result = std::move(on_result)](SkillResult result) mutable {
        if (terminal->exchange(true)) return;
        if (cancelled->load()) result.status = core::Status::Error(core::ErrorCode::Cancelled, "skill invocation cancelled");
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
                                execution_id = started.value().execution_id,
                                terminal, on_observation = std::move(on_observation)](
                                   service::persona::SkillObservation observation) mutable {
        if (terminal->load()) return;
        // 即使终态检查后发生取消，新一轮 Session 也必须拒绝旧 execution 的观察。
        if (!observation.execution_id.empty() && observation.execution_id != execution_id) return;
        observation.execution_id = execution_id;
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
        active_executions_[started.value().execution_id] = {executor.value(), callbacks.on_result, cancelled};
    }
    auto start_completion = callbacks.on_result;
    auto execution = [&]() -> core::Result<service::persona::SkillSessionSnapshot> {
        try { return executor.value()->Start(request, std::move(callbacks)); }
        catch (const std::exception& error) { return core::Status::Error(core::ErrorCode::InternalError, error.what()); }
        catch (...) { return core::Status::Error(core::ErrorCode::InternalError, "tool executor threw"); }
    }();
    if (cancelled->load()) {
        try { executor.value()->Cancel(started.value().execution_id); }
        catch (...) { core::LoggerAdapter::ForModule("skill").warn("executor cancellation after Start failed"); }
    }
    if (!execution.ok()) {
        SkillResult failed;
        failed.status = execution.status();
        start_completion(std::move(failed));
        return execution.status();
    }
    return started;
}

core::Status SkillInvocationService::Cancel(std::string_view execution_id) {
    ActiveExecution execution;
    {
        std::lock_guard lock(active_mutex_);
        auto it = active_executions_.find(std::string(execution_id));
        if (it == active_executions_.end()) {
            return core::Status::Error(core::ErrorCode::NotFound,
                                       std::string("execution not found: ") + std::string(execution_id));
        }
        execution = std::move(it->second);
        active_executions_.erase(it);
    }
    execution.cancelled->store(true);
    core::Status status;
    try { status = execution.executor->Cancel(execution_id); }
    catch (const std::exception& error) { status = core::Status::Error(core::ErrorCode::InternalError, error.what()); }
    catch (...) { status = core::Status::Error(core::ErrorCode::InternalError, "tool cancellation threw"); }
    SkillResult result;
    result.execution_id = execution_id;
    result.status = core::Status::Error(core::ErrorCode::Cancelled, "skill invocation cancelled");
    execution.complete(std::move(result));
    return status;
}

SkillToolCallCoordinator::SkillToolCallCoordinator(
    std::shared_ptr<const ISkillRegistry> registry,
    std::shared_ptr<ISkillInvocationService> invocation)
    : registry_(std::move(registry)), invocation_(std::move(invocation)) {}

namespace {
bool IsAllowedTool(
    std::string_view tool_name,
    const std::optional<std::vector<std::string>>& allowed) {
    return !allowed ||
        std::find(allowed->begin(), allowed->end(), tool_name) != allowed->end();
}

void ValidateSkillOutput(
    SkillResult& result,
    const SkillManifest& manifest,
    const SkillCall& call,
    const SkillToolCallContext& context) {
    if ((!result.call_id.empty() && result.call_id != call.call_id) ||
        (!result.skill_id.empty() && result.skill_id != call.skill_id)) {
        result.status = core::Status::Error(core::ErrorCode::DataLoss,
                                           "tool result identity does not match invocation");
    }
    // 执行器不能更换 tool_call_id；空 ID 按真实调用补齐，失败也必须关联原调用。
    result.call_id = call.call_id;
    result.skill_id = call.skill_id;
    if (!result.status.ok()) return;
    if (context.max_result_bytes == 0 || result.result_json.size() > context.max_result_bytes ||
        result.provenance_json.size() > context.max_result_bytes - result.result_json.size()) {
        result.status = core::Status::Error(core::ErrorCode::ResourceExhausted,
                                           "tool result exceeds configured byte budget");
        result.result_json.clear();
        result.provenance_json.clear();
        return;
    }
    const auto output = result.result_json.empty() ? std::string("{}") : result.result_json;
    if (auto status = ValidateSkillJsonSchema(manifest.output_schema_json, output); !status.ok()) {
        result.status = std::move(status);
        result.result_json.clear();
        result.provenance_json.clear();
    }
    if (result.status.ok() && context.policy) {
        result.status = context.policy->ValidateResult(context, call, manifest, result);
    }
}

llm::ChatMessage CheckedToolMessage(
    SkillResult result, const SkillManifest& manifest,
    const SkillCall& call, const SkillToolCallContext& context) {
    try {
        ValidateSkillOutput(result, manifest, call, context);
    } catch (const std::exception&) {
        result.call_id = call.call_id;
        result.skill_id = call.skill_id;
        result.status = core::Status::Error(core::ErrorCode::InternalError,
                                           "tool result validator threw an exception");
    } catch (...) {
        result.call_id = call.call_id;
        result.skill_id = call.skill_id;
        result.status = core::Status::Error(core::ErrorCode::InternalError, "tool result validator threw");
    }
    auto message = MakeToolResultMessage(result, context.result_format);
    if (!result.status.ok()) {
        core::LoggerAdapter::ForModule("skill").warn(
            "[trace={}] tool result rejected skill={} code={}",
            context.trace_id, call.skill_id, static_cast<int>(result.status.code()));
    }
    if (message.content.size() > context.max_result_bytes) {
        result.status = core::Status::Error(core::ErrorCode::ResourceExhausted,
                                           "serialized tool message exceeds configured byte budget");
        // 错误控制消息保留原调用 ID，允许小预算下固定错误信封略大于业务 payload 配额。
        message = MakeToolResultMessage(result, context.result_format);
    }
    return message;
}

core::Status AuthorizeTool(
    const SkillToolCallContext& context, const SkillCall& call, const SkillManifest& manifest) {
    if (!IsAllowedTool(call.tool_name, context.allowed_tool_names)) {
        core::LoggerAdapter::ForModule("skill").warn(
            "[trace={}] undeclared tool rejected name={}", context.trace_id, call.tool_name);
        return core::Status::Error(core::ErrorCode::PermissionDenied,
                                   "tool call is not allowed for this turn");
    }
    try {
        auto status = context.policy ? context.policy->Authorize(context, call, manifest) : core::Status::Ok();
        if (!status.ok()) {
            core::LoggerAdapter::ForModule("skill").warn(
                "[trace={}] tool authorization rejected skill={} code={}",
                context.trace_id, call.skill_id, static_cast<int>(status.code()));
        }
        return status;
    } catch (const std::exception&) {
        return core::Status::Error(core::ErrorCode::InternalError, "tool authorizer threw an exception");
    } catch (...) {
        return core::Status::Error(core::ErrorCode::InternalError, "tool authorizer threw");
    }
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
    const auto deadline = std::chrono::steady_clock::now() + context.timeout;
    std::vector<llm::ChatMessage> messages;
    messages.reserve(calls.value().size());
    for (const auto& call : calls.value()) {
        if (context.cancellation.stop_requested()) return core::Status::Error(core::ErrorCode::Cancelled, "tool phase cancelled");
        auto manifest = registry_->Require(call.skill_id, call.skill_version);
        if (!manifest.ok()) return manifest.status();
        if (std::chrono::steady_clock::now() >= deadline) {
            SkillResult timed_out;
            timed_out.status = core::Status::Error(core::ErrorCode::Timeout, "skill execution timed out");
            messages.push_back(CheckedToolMessage(std::move(timed_out), manifest.value(), call, context));
            continue;
        }
        if (auto status = AuthorizeTool(context, call, manifest.value()); !status.ok()) {
            SkillResult rejected;
            rejected.call_id = call.call_id;
            rejected.skill_id = call.skill_id;
            rejected.status = std::move(status);
            messages.push_back(CheckedToolMessage(std::move(rejected), manifest.value(), call, context));
            continue;
        }
        struct CompletionState {
            std::mutex mutex;
            std::condition_variable condition;
            bool done = false;
            std::optional<SkillResult> result;
        };
        auto completion = std::make_shared<CompletionState>();
        std::stop_callback cancellation(context.cancellation, [completion, call] {
            std::lock_guard lock(completion->mutex);
            if (completion->done) return;
            SkillResult result;
            result.call_id = call.call_id;
            result.skill_id = call.skill_id;
            result.status = core::Status::Error(core::ErrorCode::Cancelled, "tool phase cancelled");
            completion->result = std::move(result);
            completion->done = true;
            completion->condition.notify_all();
        });
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
            messages.push_back(CheckedToolMessage(std::move(failed), manifest.value(), call, context));
            continue;
        }
        std::unique_lock lock(completion->mutex);
        if (!completion->condition.wait_until(lock, deadline, [&] { return completion->done; })) {
            completion->done = true;
            // Cancel 允许同步触发 on_result；持有完成锁调用它会发生自锁。
            lock.unlock();
            SkillResult timed_out;
            timed_out.call_id = call.call_id;
            timed_out.skill_id = call.skill_id;
            timed_out.execution_id = started.value().execution_id;
            timed_out.status = core::Status::Error(core::ErrorCode::Timeout,
                                                    "skill execution timed out");
            invocation_->Cancel(started.value().execution_id);
            messages.push_back(CheckedToolMessage(std::move(timed_out), manifest.value(), call, context));
            continue;
        }
        auto result = std::move(*completion->result);
        lock.unlock();
        if (context.cancellation.stop_requested()) {
            invocation_->Cancel(started.value().execution_id);
            return core::Status::Error(core::ErrorCode::Cancelled, "tool phase cancelled");
        }
        messages.push_back(CheckedToolMessage(std::move(result), manifest.value(), call, context));
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
        std::mutex mutex;
        std::vector<SkillCall> calls;
        SkillToolCallContext context;
        std::shared_ptr<const ISkillRegistry> registry;
        std::shared_ptr<ISkillInvocationService> invocation;
        std::vector<llm::ChatMessage> messages;
        std::size_t next = 0;
        std::function<void(core::Result<std::vector<llm::ChatMessage>>)> completion;
        std::atomic<bool> completed{false};
        std::string active_execution;
        std::shared_ptr<ISkillDeadline> deadline;
        std::optional<std::stop_callback<std::function<void()>>> cancellation;

        void Abort(core::Status status) {
            Completion callback;
            std::string execution;
            std::shared_ptr<ISkillDeadline> timer;
            std::vector<llm::ChatMessage> results;
            std::vector<SkillCall> unfinished;
            {
                std::lock_guard lock(mutex);
                if (completed.exchange(true)) return;
                callback = std::move(completion);
                execution = std::move(active_execution);
                timer = std::move(deadline);
                results = std::move(messages);
                unfinished.assign(calls.begin() + results.size(), calls.end());
            }
            if (timer) timer->Cancel();
            // 不持完成锁调用 executor；Cancel 允许同步触发结果回调。
            if (!execution.empty()) invocation->Cancel(execution);
            core::LoggerAdapter::ForModule("skill").warn(
                "[trace={}] tool phase aborted code={}", context.trace_id, static_cast<int>(status.code()));
            if (!callback) return;
            if (status.code() == core::ErrorCode::Timeout) {
                // 与同步路径一致：可选工具超时回填工具错误；必需工具由 Turn 验收拒绝。
                for (const auto& call : unfinished) {
                    SkillResult timed_out;
                    timed_out.status = core::Status::Error(core::ErrorCode::Timeout, "skill execution timed out");
                    auto manifest = registry->Require(call.skill_id, call.skill_version);
                    results.push_back(CheckedToolMessage(std::move(timed_out),
                        manifest.ok() ? manifest.value() : SkillManifest{}, call, context));
                }
                callback(std::move(results));
            } else callback(std::move(status));
        }
    };
    auto state = std::make_shared<State>();
    state->calls = std::move(calls).value();
    state->context = std::move(context);
    state->registry = registry_;
    state->invocation = invocation_;
    state->completion = std::move(completion);
    if (state->context.timeout.count() <= 0) {
        state->Abort(core::Status::Error(core::ErrorCode::Timeout, "tool deadline expired"));
        return core::Status::Ok();
    }
    if (state->context.deadline_scheduler) {
        std::weak_ptr<State> weak = state;
        auto timer = state->context.deadline_scheduler->Schedule(
            std::chrono::steady_clock::now() + state->context.timeout, [weak] {
                if (auto active = weak.lock()) active->Abort(
                    core::Status::Error(core::ErrorCode::Timeout, "tool phase deadline exceeded"));
            });
        if (!timer.ok()) return timer.status();
        std::lock_guard lock(state->mutex);
        if (state->completed.load()) timer.value()->Cancel();
        else state->deadline = std::move(timer).value();
    }
    // stop_callback 构造时可能立即同步执行，不能持 State 锁。
    state->cancellation.emplace(state->context.cancellation, [weak = std::weak_ptr<State>(state)] {
        if (auto active = weak.lock()) active->Abort(
            core::Status::Error(core::ErrorCode::Cancelled, "tool phase cancelled"));
    });
    auto advance = std::make_shared<std::function<void()>>();
    // 自引用递归用 weak_ptr 打破 shared_ptr 环：否则 advance → lambda → advance 成环，
    // ExecuteAsync 返回后状态机（State 及其 messages/completion/context）无法释放。
    std::weak_ptr<std::function<void()>> weak_advance = advance;
    *advance = [state, weak_advance]() mutable {
        auto self = weak_advance.lock();
        if (!self) return;
        Completion finished;
        std::vector<llm::ChatMessage> messages;
        std::optional<SkillCall> next_call;
        {
            std::lock_guard lock(state->mutex);
            if (state->completed.load()) return;
            if (state->next >= state->calls.size()) {
                state->completed.store(true);
                finished = std::move(state->completion);
                messages = std::move(state->messages);
                if (state->deadline) state->deadline->Cancel();
            } else {
                next_call = state->calls[state->next++];
            }
        }
        if (finished) { finished(std::move(messages)); return; }
        if (!next_call) return;
        const auto call = std::move(*next_call);
        auto manifest = state->registry->Require(call.skill_id, call.skill_version);
        const auto authorization = manifest.ok()
            ? AuthorizeTool(state->context, call, manifest.value()) : manifest.status();
        if (!authorization.ok()) {
            SkillResult rejected;
            rejected.call_id = call.call_id;
            rejected.skill_id = call.skill_id;
            rejected.status = authorization;
            auto message = CheckedToolMessage(
                std::move(rejected), manifest.ok() ? manifest.value() : SkillManifest{}, call, state->context);
            {
                std::lock_guard lock(state->mutex);
                if (state->completed.load()) return;
                state->messages.push_back(std::move(message));
            }
            (*self)();
            return;
        }
        SkillExecutionRequest request;
        request.call = call;
        request.session_id = state->context.session_id;
        request.user_uuid = state->context.user_uuid;
        request.persona_id = state->context.persona_id;
        request.trace_id = state->context.trace_id;
        const auto declared_manifest = manifest.value();
        auto call_finished = std::make_shared<std::atomic<bool>>(false);
        SkillExecutionCallbacks callbacks;
        // on_result 持有 self 强引用，保证执行器回调完成前状态机存活（与上方 weak_ptr 配合无环）。
        callbacks.on_result = [state, self, call_finished, declared_manifest, call](SkillResult result) mutable {
            if (call_finished->exchange(true, std::memory_order_acq_rel)) return;
            if (state->completed.load()) return;
            auto message = CheckedToolMessage(std::move(result), declared_manifest, call, state->context);
            {
                std::lock_guard lock(state->mutex);
                if (state->completed.load()) return;
                state->active_execution.clear();
                state->messages.push_back(std::move(message));
            }
            (*self)();
        };
        {
            std::lock_guard lock(state->mutex);
            if (state->completed.load()) return;
            // 若在 Invoke 返回前取消，待其返回后再取消实际启动的执行实例。
            state->active_execution.clear();
        }
        auto started = state->invocation->Invoke(std::move(request), std::move(callbacks));
        if (started.ok() && !call_finished->load()) {
            std::lock_guard lock(state->mutex);
            if (!state->completed.load()) state->active_execution = started.value().execution_id;
        }
        // 取消时执行实例可能尚未启动；Invoke 返回后补发取消，避免遗漏。
        if (started.ok() && state->completed.load() && !call_finished->load()) {
            state->invocation->Cancel(started.value().execution_id);
            return;
        }
        if (!started.ok()) {
            if (call_finished->exchange(true, std::memory_order_acq_rel)) return;
            SkillResult result;
            result.call_id = call.call_id;
            result.skill_id = call.skill_id;
            result.status = started.status();
            auto message = CheckedToolMessage(std::move(result), declared_manifest, call, state->context);
            {
                std::lock_guard lock(state->mutex);
                if (state->completed.load()) return;
                state->active_execution.clear();
                state->messages.push_back(std::move(message));
            }
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
