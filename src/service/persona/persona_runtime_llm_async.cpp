#include "persona_runtime.h"

#include <iterator>
#include <mutex>

namespace agent::service::persona {
namespace {
std::chrono::milliseconds Since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}
}

// 保留原 Turn/Session 生命周期，单独维护异步 LLM 与工具 follow-up 阶段。
core::Status PersonaRuntime::CompleteWithLlmAsync(
    SessionState session,
    PreparedChat prepared,
    std::function<void(core::Result<CompletedChat>)> completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "async chat completion callback is required");
    }
    if (prepared.cancel_requested && prepared.cancel_requested->load(std::memory_order_acquire)) {
        return core::Status::Error(core::ErrorCode::Cancelled, "persona turn cancelled by caller");
    }
    const auto io_stage_start = std::chrono::steady_clock::now();
    std::optional<AnswerCacheLookupRequest> answer_cache_lookup;
    if (answer_cache_provider_ && !prepared.request.stream) {
        const auto cache_start = std::chrono::steady_clock::now();
        prepared.answer_cache.enabled = true;
        AnswerCacheLookupRequest lookup;
        lookup.session_id = prepared.request.session_id;
        lookup.user_uuid = session.user_uuid;
        lookup.persona_id = session.persona_id;
        lookup.trace_id = prepared.request.trace_id;
        lookup.query = prepared.request.user_input;
        lookup.model = prepared.request.model.empty() ? options_.default_model : prepared.request.model;
        lookup.generation = prepared.generation;
        lookup.messages = prepared.messages;

        auto cache_result = answer_cache_provider_->Lookup(lookup);
        prepared.latency.answer_cache = Since(cache_start);
        if (!cache_result.ok()) {
            return cache_result.status();
        }
        if (cache_result.value().hit) {
            prepared.answer_cache.hit = true;
            prepared.answer_cache.source = cache_result.value().source;
            prepared.answer_cache.cache_key = cache_result.value().cache_key;
            prepared.answer_cache.similarity_score = cache_result.value().similarity_score;
            llm::ChatCompletionResponse cached;
            cached.content = cache_result.value().response;
            cached.model = lookup.model;
            prepared.latency.llm_total = std::chrono::milliseconds{0};
            completion(FinalizeLlmCompletion(
                session, std::move(prepared), std::move(cached), std::nullopt, io_stage_start));
            return core::Status::Ok();
        }
        answer_cache_lookup = std::move(lookup);
    }

    llm::ChatCompletionRequest request;
    request.model = prepared.request.model.empty() ? options_.default_model : prepared.request.model;
    request.messages = prepared.messages;
    request.temperature = static_cast<float>(prepared.generation.temperature);
    request.max_tokens = prepared.generation.max_tokens;
    request.top_p = static_cast<float>(prepared.generation.top_p);
    request.tools = prepared.tools;
    if (!request.tools.empty()) request.tool_choice = "auto";
    request.stream = prepared.request.stream;
    llm::LlmEventSink stream_sink;
    if (request.stream) {
        struct PendingText {
            bool saw_tools = false;
            std::size_t bytes = 0;
            std::vector<llm::LlmStreamEvent> events;
        };
        auto pending = std::make_shared<PendingText>();
        const bool defer_text = !request.tools.empty() && prepared.tool_round == 0;
        stream_sink = [sink = prepared.request.event_sink, pending, defer_text,
                       generation = prepared.request.trace_id + ":generation:" + std::to_string(prepared.tool_round)](
                          const llm::LlmStreamEvent& source) {
            auto event = source;
            event.generation_id = generation;
            // 有工具候选时，首轮文本暂存到完整调用已校验；工具轮的过渡文本不对外展示。
            if (defer_text && event.kind == llm::LlmStreamEventKind::TextDelta) {
                if (pending->events.size() >= 4096 || event.text.size() > 1024 * 1024 - pending->bytes)
                    return core::Status::Error(core::ErrorCode::ResourceExhausted, "provisional tool text exceeds buffer limit");
                pending->bytes += event.text.size();
                pending->events.push_back(std::move(event));
                return core::Status::Ok();
            }
            if (event.kind == llm::LlmStreamEventKind::ToolCallProgress) pending->saw_tools = true;
            if (event.kind == llm::LlmStreamEventKind::OutputItemCompleted) {
                if (!pending->saw_tools) {
                    for (const auto& text : pending->events)
                        if (auto status = sink(text); !status.ok()) return status;
                }
                pending->events.clear();
            }
            return sink(event);
        };
    }
    const auto llm_started_at = std::chrono::steady_clock::now();
    std::uint64_t operation_id = 0;
    {
        std::lock_guard lock(async_operations_mutex_);
        if (async_stopping_) {
            return core::Status::Error(core::ErrorCode::Cancelled,
                                       "persona runtime is shutting down");
        }
        operation_id = next_async_operation_id_++;
        async_operations_.emplace(operation_id, AsyncOperationRecord{
            .session_id = prepared.request.session_id,
            .trace_id = prepared.request.trace_id,
            .llm_operation = nullptr,
            .cancel_requested = prepared.cancel_requested,
        });
        async_operation_by_session_[prepared.request.session_id] = operation_id;
    }
    llm::IAsyncLlmClient::Callback on_complete =
        [this, session = std::move(session), prepared = std::move(prepared),
         answer_cache_lookup = std::move(answer_cache_lookup), io_stage_start, llm_started_at,
         operation_id,
         completion = std::move(completion)](
            core::Result<llm::ChatCompletionResponse> result) mutable {
            struct OperationCleanup {
                PersonaRuntime* runtime;
                std::uint64_t id;
                ~OperationCleanup() {
                    {
                        std::lock_guard lock(runtime->async_operations_mutex_);
                        auto it = runtime->async_operations_.find(id);
                        if (it != runtime->async_operations_.end()) {
                            auto index = runtime->async_operation_by_session_.find(it->second.session_id);
                            // 首轮同步回调可能已登记 follow-up，旧请求不能擦掉新句柄索引。
                            if (index != runtime->async_operation_by_session_.end() && index->second == id)
                                runtime->async_operation_by_session_.erase(index);
                            runtime->async_operations_.erase(it);
                        }
                    }
                    runtime->async_operations_drained_.notify_all();
                }
            } cleanup{this, operation_id};
            auto result_holder = std::make_shared<core::Result<llm::ChatCompletionResponse>>(
                std::move(result));
            auto finalize = [this,
                             session = std::move(session),
                             prepared = std::move(prepared),
                             answer_cache_lookup = std::move(answer_cache_lookup),
                             io_stage_start,
                             llm_started_at,
                             result_holder,
                             completion = std::move(completion)]() mutable {
                auto prepared_value = std::move(prepared);
                prepared_value.latency.llm_total = Since(llm_started_at);
                if (prepared_value.cancel_requested &&
                    prepared_value.cancel_requested->load(std::memory_order_acquire)) {
                    completion(core::Status::Error(core::ErrorCode::Cancelled,
                                                  "persona turn cancelled by caller"));
                    return;
                }
                if (!result_holder->ok()) {
                    completion(result_holder->status());
                    return;
                }
                auto llm_completion = std::move(*result_holder).value();
                if (!llm_completion.tool_calls.empty() &&
                    skill_tool_coordinator_ && prepared_value.tool_round == 0) {
                    if (prepared_value.request.stream) {
                        for (const auto& call : llm_completion.tool_calls) {
                            auto status = prepared_value.request.event_sink({
                                .kind = llm::LlmStreamEventKind::ToolExecutionState,
                                .generation_id = prepared_value.request.trace_id + ":generation:0",
                                .item_id = call.id, .text = "started", .tool_call = call});
                            if (!status.ok()) { completion(status); return; }
                        }
                    }
                    prepared_value.prompt_tokens += llm_completion.prompt_tokens;
                    prepared_value.completion_tokens += llm_completion.completion_tokens;
                    prepared_value.total_tokens += llm_completion.total_tokens;
                    llm::ChatMessage assistant;
                    assistant.role = llm::ChatRole::Assistant;
                    assistant.content = llm_completion.content;
                    assistant.reasoning_content = llm_completion.reasoning_content;
                    assistant.tool_calls = llm_completion.tool_calls;
                    prepared_value.messages.push_back(std::move(assistant));
                    prepared_value.tool_round = 1;
                    const auto skill_context = agent::skill::SkillToolCallContext{
                        session.session_id,
                        session.user_uuid,
                        session.persona_id,
                        prepared_value.request.trace_id,
                        std::chrono::seconds(30)};
                    auto completion_holder =
                        std::make_shared<std::function<void(core::Result<CompletedChat>)>>(
                            std::move(completion));
                    auto follow_up = [this,
                                      session = std::move(session),
                                      prepared = std::move(prepared_value),
                                      completion_holder](
                                         core::Result<std::vector<llm::ChatMessage>> tool_messages) mutable {
                        if (prepared.request.stream) {
                            auto status = prepared.request.event_sink({
                                .kind = llm::LlmStreamEventKind::ToolExecutionState,
                                .generation_id = prepared.request.trace_id + ":generation:0",
                                .item_id = "tools", .text = tool_messages.ok() ? "completed" : "failed"});
                            if (!status.ok()) { (*completion_holder)(status); return; }
                        }
                        if (!tool_messages.ok()) {
                            (*completion_holder)(tool_messages.status());
                            return;
                        }
                        prepared.messages.insert(
                            prepared.messages.end(),
                            std::make_move_iterator(tool_messages.value().begin()),
                            std::make_move_iterator(tool_messages.value().end()));
                        auto status = CompleteWithLlmAsync(
                            std::move(session), std::move(prepared), *completion_holder);
                        if (!status.ok()) {
                            // CompleteWithLlmAsync 未接纳请求时没有异步 callback，需立即收口。
                            (*completion_holder)(status);
                        }
                    };
                    auto status = skill_tool_coordinator_->ExecuteAsync(
                        llm_completion, skill_context, std::move(follow_up));
                    if (!status.ok()) {
                        (*completion_holder)(status);
                    }
                    return;
                }
                if (prepared_value.request.stream && !llm_completion.tool_calls.empty()) {
                    completion(core::Status::Error(core::ErrorCode::FailedPrecondition,
                                                   "streaming tool loop budget exhausted or coordinator missing"));
                    return;
                }
                auto completion_holder =
                    std::make_shared<std::function<void(core::Result<CompletedChat>)>>(
                        std::move(completion));
                auto status = FinalizeLlmCompletionAsync(
                    std::move(session),
                    std::move(prepared_value),
                    std::move(llm_completion),
                    std::move(answer_cache_lookup),
                    io_stage_start,
                    *completion_holder);
                if (!status.ok()) {
                    (*completion_holder)(status);
                }
            };
            if (!continuation_pool_) {
                finalize();
                return;
            }
            auto finalize_holder = std::make_shared<std::function<void()>>(
                std::move(finalize));
            auto dispatch_status = continuation_pool_->Submit(
                [finalize_holder](core::ThreadPoolContext&) mutable {
                    (*finalize_holder)();
                    return core::Status::Ok();
                },
                {},
                "persona-llm-continuation");
            if (!dispatch_status.ok()) {
                (*finalize_holder)();
            }
        };
    auto submitted = request.stream
        ? dynamic_cast<llm::IAsyncStreamingLlmClient*>(async_llm_client_.get())->CompleteStreamingAsync(
              std::move(request), std::move(stream_sink), std::move(on_complete))
        : async_llm_client_->CompleteAsync(std::move(request), std::move(on_complete));
    if (!submitted.ok()) {
        {
            std::lock_guard lock(async_operations_mutex_);
            auto it = async_operations_.find(operation_id);
            if (it != async_operations_.end()) {
                auto index = async_operation_by_session_.find(it->second.session_id);
                if (index != async_operation_by_session_.end() && index->second == operation_id)
                    async_operation_by_session_.erase(index);
                async_operations_.erase(it);
            }
        }
        async_operations_drained_.notify_all();
        return submitted.status();
    }

    bool cancel = false;
    {
        std::lock_guard lock(async_operations_mutex_);
        auto operation = async_operations_.find(operation_id);
        if (operation == async_operations_.end()) {
            // callback 允许在 CompleteAsync 返回前同步完成，此时无需再保存句柄。
            return core::Status::Ok();
        }
        operation->second.llm_operation = submitted.value();
        cancel = async_stopping_ || (operation->second.cancel_requested &&
            operation->second.cancel_requested->load(std::memory_order_acquire));
    }
    if (cancel) {
        submitted.value()->Cancel();
    }
    return core::Status::Ok();
}

}
