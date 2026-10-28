#include "persona_runtime.h"
#include "../../core/text_validation.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
#include <nlohmann/json.hpp>
namespace agent::service::persona {
namespace {
core::Result<std::size_t> PromptRequestCharacters(
    const std::vector<llm::ChatMessage>& messages,
    const std::vector<llm::ChatCompletionRequest::Tool>& tools) {
    std::size_t characters = 0;
    const auto add = [&characters](std::string_view value) -> core::Status {
        if (!core::IsValidUtf8(value)) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "prompt contains invalid UTF-8 text");
        }
        const auto count = static_cast<std::size_t>(std::count_if(
            value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xC0) != 0x80; }));
        if (count > std::numeric_limits<std::size_t>::max() - characters) {
            return core::Status::Error(core::ErrorCode::ResourceExhausted,
                                       "prompt character count overflow");
        }
        characters += count;
        return core::Status::Ok();
    };
    for (const auto& message : messages) {
        if (auto status = add(message.content); !status.ok()) return status;
        if (message.reasoning_content) {
            if (auto status = add(*message.reasoning_content); !status.ok()) return status;
        }
        if (auto status = add(message.tool_call_id); !status.ok()) return status;
        for (const auto& part : message.parts) {
            if (auto status = add(part.text); !status.ok()) return status;
            if (auto status = add(part.image_url); !status.ok()) return status;
        }
        for (const auto& call : message.tool_calls) {
            if (auto status = add(call.id); !status.ok()) return status;
            if (auto status = add(call.name); !status.ok()) return status;
            if (auto status = add(call.arguments_json); !status.ok()) return status;
        }
    }
    for (const auto& tool : tools) {
        if (auto status = add(tool.name); !status.ok()) return status;
        if (auto status = add(tool.description); !status.ok()) return status;
        if (auto status = add(tool.parameters_json); !status.ok()) return status;
    }
    return characters;
}
std::chrono::milliseconds Since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
}
GenerationParams ApplyEmotionAdaptiveGeneration(const GenerationParams& base,
                                                const EmotionAnalysis& emotion,
                                                const EmotionGenerationOptions& options) {
    auto adjusted = base;
    const auto weight_it = options.token_weights.find(emotion.emotion.primary);
    double weight = weight_it == options.token_weights.end()
        ? options.default_token_weight
        : weight_it->second;
    if (emotion.emotion.intensity >= options.high_intensity_threshold) {
        weight *= options.high_intensity_multiplier;
    }
    const int maximum = std::max(
        1,
        static_cast<int>(std::floor(base.max_tokens * options.max_token_ratio)));
    const int minimum = std::min(options.min_tokens, maximum);
    const int adaptive_tokens = static_cast<int>(std::lround(base.max_tokens * weight));
    adjusted.max_tokens = std::clamp(adaptive_tokens, minimum, maximum);
    return adjusted;
}
core::Status ValidateEmotionGenerationOptions(const EmotionGenerationOptions& options) {
    if (options.default_generation.max_tokens <= 0 || options.min_tokens <= 0 ||
        !std::isfinite(options.max_token_ratio) || options.max_token_ratio < 1.0 ||
        !std::isfinite(options.default_token_weight) || options.default_token_weight <= 0.0 ||
        !std::isfinite(options.high_intensity_threshold) ||
        options.high_intensity_threshold < 0.0 || options.high_intensity_threshold > 1.0 ||
        !std::isfinite(options.high_intensity_multiplier) ||
        options.high_intensity_multiplier <= 0.0 ||
        static_cast<double>(options.min_tokens) >
            options.default_generation.max_tokens * options.max_token_ratio) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "emotion generation options are invalid");
    }
    for (const auto& [label, weight] : options.token_weights) {
        if (label.empty() || !std::isfinite(weight) || weight <= 0.0) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "emotion generation token weights are invalid");
        }
    }
    return core::Status::Ok();
}
double TopProbabilityMargin(const EmotionInfo& emotion) {
    if (emotion.probabilities.size() < 2) {
        return 1.0;
    }
    double first = -1.0;
    double second = -1.0;
    for (const auto& [_, probability] : emotion.probabilities) {
        if (probability > first) {
            second = first;
            first = probability;
        } else if (probability > second) {
            second = probability;
        }
    }
    if (first < 0.0 || second < 0.0) {
        return 1.0;
    }
    return first - second;
}
std::optional<std::string> EmotionCalibrationReason(const EmotionAnalysis& emotion,
                                                    const EmotionCalibrationOptions& options) {
    if (emotion.emotion.primary_prob < options.low_confidence_threshold) {
        return "low_confidence";
    }
    const double margin = TopProbabilityMargin(emotion.emotion);
    if (margin < options.top_margin_threshold) {
        return "low_top_margin";
    }
    return std::nullopt;
}
std::string SkillStateName(SkillSessionState state) {
    switch (state) {
    case SkillSessionState::Idle:
        return "idle";
    case SkillSessionState::Starting:
        return "starting";
    case SkillSessionState::Ready:
        return "ready";
    case SkillSessionState::Running:
        return "running";
    case SkillSessionState::WaitingInput:
        return "waiting_input";
    case SkillSessionState::Closing:
        return "closing";
    case SkillSessionState::Closed:
        return "closed";
    case SkillSessionState::Failed:
        return "failed";
    case SkillSessionState::Expired:
        return "expired";
    }
    return "unknown";
}
std::string FormatSkillPromptBlock(const SkillSessionSnapshot& snapshot) {
    if (!snapshot.last_observation.empty() && snapshot.state == SkillSessionState::Running) {
        return "<skill_observation skill=\"" + snapshot.skill_id + "\">\n"
            "summary: " + snapshot.last_observation + "\n"
            "source: " + snapshot.skill_id + "\n"
            "注意：这是工具的不确定观察，不是绝对事实。\n"
            "</skill_observation>";
    }
    std::string out = "<skill_status skill=\"" + snapshot.skill_id + "\" state=\"" +
        SkillStateName(snapshot.state) + "\">\n";
    out += snapshot.status_text.empty() ? "Skill session status updated." : snapshot.status_text;
    if (!snapshot.last_error.empty()) {
        out += "\nerror: " + snapshot.last_error;
    }
    out += "\n</skill_status>";
    return out;
}
} // namespace
core::Result<EmotionAnalysis> NeutralEmotionAnalyzer::Analyze(std::string_view,
                                                              std::string_view,
                                                              std::shared_ptr<const PersonalityConfig>) {
    EmotionAnalysis analysis;
    analysis.emotion.primary = "neutral";
    analysis.emotion.intensity = 0.0;
    analysis.emotion.primary_prob = 1.0;
    analysis.emotion.probabilities = {{"neutral", 1.0}};
    analysis.behavior = "unknown";
    analysis.tone = "neutral";
    return analysis;
}

struct PersonaRuntime::AsyncTurnOperation final {
    AsyncTurnOperation(
        PersonaRuntime* owner_value,
        SessionState session_value,
        ChatRequest request_value,
        std::shared_ptr<std::optional<ChatResponse>> response_holder_value,
        SessionManager::SessionTurnAsyncFinish finish_value,
        std::chrono::steady_clock::time_point submitted_at_value)
        : owner(owner_value),
          session(std::move(session_value)),
          request(std::move(request_value)),
          response_holder(std::move(response_holder_value)),
          finish(std::move(finish_value)),
          user_input(request.user_input),
          context_id(request.context_id),
          turn_id(session.session_id + ":" + std::to_string(session.metrics.turn_count + 1)),
          persona_id(session.persona_id),
          submitted_at(submitted_at_value),
          compute_started_at(std::chrono::steady_clock::now()),
          compute_queue_wait(std::chrono::duration_cast<std::chrono::milliseconds>(
              compute_started_at - submitted_at)),
          logger(owner_value->logger_) {}

    ~AsyncTurnOperation() {
        ReleaseRuntimeLease();
    }

    bool Transition(AsyncTurnPhase expected, AsyncTurnPhase next) noexcept {
        if (cancel_requested->load(std::memory_order_acquire)) {
            Finish(core::Status::Error(core::ErrorCode::Cancelled,
                                      "persona turn cancelled by caller"));
            return false;
        }
        return phase.compare_exchange_strong(
            expected, next, std::memory_order_acq_rel);
    }

    void Finish(core::Result<SessionTurnCommit> result) noexcept {
        {
            // Cancel 与最终提交共用短锁，取消先被接纳时不能再提交成功结果。
            std::lock_guard lock(completion_mutex);
            if (finished.exchange(true, std::memory_order_acq_rel)) return;
            if (cancel_requested->load(std::memory_order_acquire)) {
                result = core::Status::Error(core::ErrorCode::Cancelled,
                                            "persona turn cancelled by caller");
            }
        }
        phase.store(result.ok() ? AsyncTurnPhase::Completed : AsyncTurnPhase::Failed,
                    std::memory_order_release);
        auto callback = std::move(finish);
        // Provider 允许在完成后继续持有 callback；Runtime drain 不能依赖
        // operation 对象析构，否则已完成 Turn 会被误判为仍在途。
        ReleaseRuntimeLease();
        try {
            callback(std::move(result));
        } catch (const std::exception& exception) {
            logger.error(
                "[trace={}] [persona_runtime] async turn finish exception: {}",
                request.trace_id, exception.what());
        } catch (...) {
            logger.error(
                "[trace={}] [persona_runtime] async turn finish exception: unknown",
                request.trace_id);
        }
    }

    void ReleaseRuntimeLease() noexcept {
        if (owner && armed.exchange(false, std::memory_order_acq_rel)) {
            owner->FinishAsyncTurnOperation(*this);
        }
    }

    PersonaRuntime* owner = nullptr;
    SessionState session;
    ChatRequest request;
    std::shared_ptr<std::optional<ChatResponse>> response_holder;
    SessionManager::SessionTurnAsyncFinish finish;
    std::string user_input;
    std::string context_id;
    std::string turn_id;
    std::string persona_id;
    std::chrono::steady_clock::time_point submitted_at;
    std::chrono::steady_clock::time_point compute_started_at;
    std::chrono::milliseconds compute_queue_wait{0};
    std::optional<EmotionAnalysis> user_emotion;
    std::atomic<AsyncTurnPhase> phase{AsyncTurnPhase::Admitted};
    std::atomic<bool> finished{false};
    std::atomic<bool> armed{false};
    std::mutex completion_mutex;
    std::shared_ptr<std::atomic<bool>> cancel_requested =
        std::make_shared<std::atomic<bool>>(false);
    std::stop_source cancellation;
    core::LoggerAdapter logger;
};

PersonaRuntime::PersonaRuntime(SessionManager& sessions,
                               std::shared_ptr<IMemoryContextProvider> memory_provider,
                               std::shared_ptr<IEmotionAnalyzer> emotion_analyzer,
                               std::shared_ptr<llm::ILlmClient> llm_client,
                               PersonaRuntimeOptions options,
                               std::shared_ptr<IAnswerCacheProvider> answer_cache_provider,
                               std::shared_ptr<IToolMemoryProvider> tool_memory_provider,
                               std::shared_ptr<ISkillSessionManager> skill_session_manager,
                               core::LoggerAdapter logger,
                               std::shared_ptr<IEmotionCalibrationSampleSink> emotion_calibration_sink,
                               std::shared_ptr<llm::IAsyncLlmClient> async_llm_client,
                               core::ThreadPool* continuation_pool,
                               std::shared_ptr<agent::skill::ISkillToolCallCoordinator> skill_tool_coordinator,
                               std::shared_ptr<IStatefulSkillExecutionRouter> stateful_skill_router)
    : sessions_(sessions),
      memory_provider_(std::move(memory_provider)),
      async_memory_provider_(
          std::dynamic_pointer_cast<IAsyncMemoryContextProvider>(memory_provider_)),
      emotion_analyzer_(std::move(emotion_analyzer)),
      async_emotion_analyzer_(
          std::dynamic_pointer_cast<IAsyncEmotionAnalyzer>(emotion_analyzer_)),
      llm_client_(std::move(llm_client)),
      async_llm_client_(std::move(async_llm_client)),
      continuation_pool_(continuation_pool),
      answer_cache_provider_(std::move(answer_cache_provider)),
      tool_memory_provider_(std::move(tool_memory_provider)),
      skill_session_manager_(std::move(skill_session_manager)),
      skill_tool_coordinator_(std::move(skill_tool_coordinator)),
      stateful_skill_router_(std::move(stateful_skill_router)),
      emotion_calibration_sink_(std::move(emotion_calibration_sink)),
      options_(std::move(options)),
      logger_(std::move(logger)) {}

PersonaRuntime::~PersonaRuntime() {
    Shutdown();
}

core::Status PersonaRuntime::CancelAsyncTurn(std::string_view session_id,
                                             std::string_view trace_id) {
    if (session_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "session_id is required");
    }
    std::shared_ptr<llm::IAsyncLlmOperation> llm_operation;
    std::shared_ptr<AsyncTurnOperation> turn_operation;
    {
        std::lock_guard lock(async_operations_mutex_);
        auto turn = async_turns_by_session_.find(std::string(session_id));
        if (turn != async_turns_by_session_.end()) turn_operation = turn->second.lock();
        if (!turn_operation) {
            return core::Status::Error(core::ErrorCode::NotFound,
                                       "no async turn is active for session");
        }
        if (!trace_id.empty() && turn_operation->request.trace_id != trace_id) {
            return core::Status::Error(core::ErrorCode::NotFound,
                                       "trace_id does not identify the active turn");
        }
        {
            std::lock_guard completion_lock(turn_operation->completion_mutex);
            if (turn_operation->finished.load(std::memory_order_acquire)) {
                return core::Status::Error(core::ErrorCode::NotFound, "async turn already completed");
            }
            turn_operation->cancel_requested->store(true, std::memory_order_release);
        }
        auto index = async_operation_by_session_.find(std::string(session_id));
        if (index != async_operation_by_session_.end()) {
            auto operation = async_operations_.find(index->second);
            if (operation != async_operations_.end() &&
                operation->second.trace_id == turn_operation->request.trace_id) {
                llm_operation = operation->second.llm_operation;
            }
        }
    }
    // 不持 Runtime 锁调用 Provider：Cancel 允许同步触发完成回调。
    turn_operation->cancellation.request_stop();
    if (llm_operation) {
        llm_operation->Cancel();
    }
    return core::Status::Ok();
}

core::Status PersonaRuntime::SubmitChat(ChatRequest request, ChatCallback callback) {
    if (!callback) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "chat callback is required");
    }
    if (!memory_provider_ || !emotion_analyzer_ || !llm_client_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "persona runtime dependencies are incomplete");
    }
    if (request.session_id.empty() ||
        (request.user_input.empty() &&
         (!request.proactive_trigger || request.proactive_trigger->empty()))) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "session_id and user_input or proactive_trigger are required");
    }
    if (auto status = ValidateEmotionGenerationOptions(options_.emotion_generation); !status.ok()) {
        logger_.warn("[persona_runtime] generation options rejected: {}", status.message());
        return status;
    }
    if (request.generation_override && request.generation_override->max_tokens <= 0) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "generation override max_tokens must be positive");
    }
    if (request.trace_id.empty()) {
        request.trace_id = core::GenerateTraceId();
    }
    if (request.stream) {
        if (!request.event_sink || !dynamic_cast<llm::IAsyncStreamingLlmClient*>(async_llm_client_.get()))
            return core::Status::Error(core::ErrorCode::Unimplemented,
                                       "streaming requires an async streaming LLM and event sink");
        struct TurnEventState {
            std::mutex mutex;
            std::uint64_t sequence = 0;
            llm::LlmEventSink sink;
        };
        auto state = std::make_shared<TurnEventState>();
        state->sink = std::move(request.event_sink);
        // sequence 属于整轮 Turn；工具 follow-up 复用同一 sink，不从零开始。
        request.event_sink = [state, id = request.trace_id, session_id = request.session_id](
                                 const llm::LlmStreamEvent& source) {
            std::lock_guard lock(state->mutex);
            auto event = source;
            event.request_id = id;
            event.turn_id = session_id + ":" + id;
            event.sequence = ++state->sequence;
            try { return state->sink(event); }
            catch (...) { return core::Status::Error(core::ErrorCode::InternalError, "persona stream sink threw an exception"); }
        };
    }
    if (async_llm_client_) {
        std::lock_guard lock(async_operations_mutex_);
        if (async_stopping_) {
            return core::Status::Error(core::ErrorCode::Cancelled,
                                       "persona runtime is shutting down");
        }
    }

    const auto trace_id = request.trace_id;
    auto tracked = callback_lifetime_.Track<core::Result<ChatResponse>>(std::move(callback));
    if (!tracked.ok()) {
        logger_.warn("[persona_runtime] callback admission rejected: {}", tracked.status().message());
        return tracked.status();
    }
    callback = std::move(tracked).value();
    const auto submitted_at = std::chrono::steady_clock::now();
    auto response_holder = std::make_shared<std::optional<ChatResponse>>();
    DispatchOptions dispatch;
    dispatch.session_id = request.session_id;
    dispatch.trace_id = request.trace_id;
    dispatch.module = "persona_runtime";
    dispatch.operation = "chat_turn";
    if (async_llm_client_) {
        return sessions_.SubmitTurnAsync(
            std::move(dispatch),
            [this, request = std::move(request), submitted_at, response_holder](
                const SessionTurnSnapshot& snapshot,
                core::ThreadPoolContext&,
                SessionManager::SessionTurnAsyncFinish finish) mutable -> core::Status {
                auto operation = BeginAsyncTurnOperation(
                    snapshot.state,
                    std::move(request),
                    response_holder,
                    std::move(finish),
                    submitted_at);
                if (!operation.ok()) {
                    return operation.status();
                }
                return StartAsyncTurn(operation.value());
            },
            [response_holder, callback = std::move(callback)](
                core::Result<SessionTurnReceipt> receipt) mutable {
                if (!receipt.ok()) {
                    callback(receipt.status());
                    return;
                }
                if (!response_holder->has_value()) {
                    callback(core::Status::Error(core::ErrorCode::InternalError,
                                                 "async session turn response is missing"));
                    return;
                }
                response_holder->value().turn_index = receipt.value().turn_index;
                callback(std::move(response_holder->value()));
            });
    }
    return sessions_.SubmitTurn(
        std::move(dispatch),
        [this, request = std::move(request), submitted_at, response_holder](
            const SessionTurnSnapshot& snapshot,
            SessionTurnCommit& commit,
            core::ThreadPoolContext&) mutable -> core::Status {
            auto session = snapshot.state;
            const auto user_input = request.user_input;
            const auto context_id = request.context_id;
            const auto turn_id = session.session_id + ":" +
                std::to_string(session.metrics.turn_count + 1);
            const auto compute_started_at = std::chrono::steady_clock::now();
            auto prepared = PrepareChat(session, std::move(request));
            if (!prepared.ok()) {
                return prepared.status();
            }
            prepared.value().started_at = submitted_at;
            prepared.value().latency.compute_queue_wait = Since(submitted_at);
            prepared.value().latency.compute_stage = Since(compute_started_at);
            prepared.value().io_submitted_at = std::chrono::steady_clock::now();

            auto completed = CompleteWithLlm(session, std::move(prepared).value());
            if (!completed.ok()) {
                return completed.status();
            }
            commit.trace_id = completed.value().trace_id;
            commit.turn.user_input = user_input;
            commit.turn.emotion = completed.value().user_emotion.emotion.primary;
            commit.turn.intensity = completed.value().user_emotion.emotion.intensity;
            commit.turn.behavior = completed.value().user_emotion.behavior;
            commit.turn.tone = completed.value().user_emotion.tone;
            commit.turn.response = completed.value().response;
            commit.turn.context_id = context_id;
            commit.turn.turn_id = turn_id;
            commit.turn.persona_id = session.persona_id;
            commit.turn.valence = session.emotion_state.state().valence;
            commit.turn.arousal = session.emotion_state.state().arousal;
            commit.emotion_state = session.emotion_state;
            commit.latency = completed.value().latency.total;
            *response_holder = std::move(completed).value();
            return core::Status::Ok();
        },
        [response_holder, callback = std::move(callback)](
            core::Result<SessionTurnReceipt> receipt) mutable {
            if (!receipt.ok()) {
                callback(receipt.status());
                return;
            }
            if (!response_holder->has_value()) {
                callback(core::Status::Error(core::ErrorCode::InternalError,
                                             "session turn response is missing"));
                return;
            }
            response_holder->value().turn_index = receipt.value().turn_index;
            callback(std::move(response_holder->value()));
        });
}

core::Result<PersonaRuntime::PreparedChat> PersonaRuntime::PrepareChat(SessionState& session,
                                                                       ChatRequest request) {
    const auto started = std::chrono::steady_clock::now();
    PreparedChat prepared;
    prepared.started_at = started;
    prepared.request = std::move(request);

    auto emotion = emotion_analyzer_->Analyze(
        prepared.request.user_input,
        prepared.request.trace_id,
        session.personality);
    if (!emotion.ok()) {
        logger_.warn("[trace={}] [persona_runtime] user emotion failed session={} code={} reason={}",
                     prepared.request.trace_id,
                     session.session_id,
                     static_cast<int>(emotion.status().code()),
                     emotion.status().message());
        return emotion.status();
    }
    logger_.info("[trace={}] [persona_runtime] user emotion done session={}",
                 prepared.request.trace_id,
                 session.session_id);

    const auto memory_start = std::chrono::steady_clock::now();
    std::vector<ConversationTurn> recent_copy(session.recent_history.begin(), session.recent_history.end());
    MemoryContextRequest memory_req;
    memory_req.session_id = session.session_id;
    memory_req.tenant_id = session.tenant_id;
    memory_req.user_uuid = session.user_uuid;
    memory_req.persona_id = session.persona_id;
    memory_req.query = prepared.request.user_input;
    memory_req.trace_id = prepared.request.trace_id;
    memory_req.current_session_recent = std::span<const ConversationTurn>(recent_copy.data(), recent_copy.size());
    memory_req.max_recent_turns = options_.recent_raw_turns;
    logger_.info("[trace={}] [persona_runtime] memory context start session={} user={} recent={}",
                 prepared.request.trace_id,
                 session.session_id,
                 session.user_uuid,
                 recent_copy.size());
    auto memory = memory_provider_->BuildContext(memory_req);
    if (!memory.ok()) {
        logger_.warn("[trace={}] [persona_runtime] memory context failed session={} code={} reason={}",
                     prepared.request.trace_id,
                     session.session_id,
                     static_cast<int>(memory.status().code()),
                     memory.status().message());
        return memory.status();
    }
    auto after_memory = PrepareChatBeforeEmotion(
        session,
        std::move(prepared.request),
        std::move(memory).value(),
        memory_start);
    if (!after_memory.ok()) {
        return after_memory.status();
    }
    return PrepareChatAfterEmotion(
        session,
        std::move(after_memory).value(),
        std::move(recent_copy),
        std::move(emotion).value());
}

core::Result<PersonaRuntime::PreparedChat> PersonaRuntime::PrepareChatBeforeEmotion(
    SessionState& session,
    ChatRequest request,
    RecalledContext memory,
    std::chrono::steady_clock::time_point memory_start) {
    PreparedChat prepared;
    prepared.started_at = memory_start;
    prepared.request = std::move(request);
    prepared.memory = std::move(memory);
    if (!prepared.memory.items.empty()) prepared.memory.system_context.clear();
    else if (!prepared.memory.system_context.empty()) {
        prepared.memory.items.push_back({ContextSource::Legacy, {}, std::move(prepared.memory.system_context)});
        prepared.memory.system_context.clear();
    }
    std::vector<std::string> triggered_skills;
    ToolMemoryContext recalled_tools;
    if (prepared.memory.tool_lookup_done && prepared.memory.recalled_tools) {
        recalled_tools = std::move(*prepared.memory.recalled_tools);
    }
    if (tool_memory_provider_ && !prepared.memory.tool_lookup_done) {
        ToolMemoryQuery tool_query;
        tool_query.session_id = session.session_id;
        tool_query.user_uuid = session.user_uuid;
        tool_query.persona_id = session.persona_id;
        tool_query.trace_id = prepared.request.trace_id;
        tool_query.query = prepared.request.user_input;
        auto tool_context = tool_memory_provider_->Query(tool_query);
        if (!tool_context.ok()) {
            recalled_tools.degraded_reasons.push_back("recall_unavailable");
            logger_.warn("[trace={}] [persona_runtime] L4 tool memory skipped session={} reason={}",
                         prepared.request.trace_id,
                         session.session_id,
                         tool_context.status().message());
        } else if (tool_context.value().hit && (options_.skill_registry || !tool_context.value().prompt_block.empty())) {
            if (!options_.skill_registry) prepared.tools = tool_context.value().tools;
            for (const auto& hit : tool_context.value().hits) {
                if (!hit.tool_id.empty() &&
                    StatefulSkillRegistry::Instance().Contains(hit.tool_id)) {
                    triggered_skills.push_back(hit.tool_id);
                }
            }
            if (!prepared.memory.system_context.empty()) {
                prepared.memory.system_context += "\n";
            }
            if (!options_.skill_registry) prepared.memory.items.push_back(
                {ContextSource::L4, {}, tool_context.value().prompt_block});
            prepared.memory.l4_hit = true;
            recalled_tools = std::move(tool_context).value();
        } else {
            recalled_tools = std::move(tool_context).value();
        }
    }
    if (prepared.memory.tool_lookup_done && recalled_tools.hit) {
        prepared.memory.l4_hit = true;
        if (!options_.skill_registry) {
            prepared.tools = recalled_tools.tools;
            prepared.memory.items.push_back({ContextSource::L4, {}, recalled_tools.prompt_block});
        }
        for (const auto& hit : recalled_tools.hits) {
            if (StatefulSkillRegistry::Instance().Contains(hit.tool_id)) triggered_skills.push_back(hit.tool_id);
        }
    }
    if (auto status = ResolveToolPlan(session, prepared, recalled_tools); !status.ok()) return status;
    if (skill_session_manager_) {
        for (const auto& skill_id : triggered_skills) {
            SkillSessionStartRequest skill_start;
            skill_start.skill_id = skill_id;
            skill_start.session_id = session.session_id;
            skill_start.user_uuid = session.user_uuid;
            skill_start.persona_id = session.persona_id;
            skill_start.trace_id = prepared.request.trace_id;
            skill_start.source = "l4";
            skill_start.reason = "用户输入触发有状态 Skill";
            auto started_skill = stateful_skill_router_
                ? stateful_skill_router_->Start(skill_start)
                : skill_session_manager_->Start(skill_start);
            if (!started_skill.ok()) {
                logger_.warn("[trace={}] [persona_runtime] skill session start skipped session={} reason={}",
                             prepared.request.trace_id,
                             session.session_id,
                             started_skill.status().message());
            }
        }
        auto active_skills = skill_session_manager_->List(session.session_id, session.user_uuid);
        if (!active_skills.ok()) {
            logger_.warn("[trace={}] [persona_runtime] skill session query skipped session={} reason={}",
                         prepared.request.trace_id,
                         session.session_id,
                         active_skills.status().message());
        } else {
            for (const auto& skill : active_skills.value()) {
                if (!prepared.memory.system_context.empty()) {
                    prepared.memory.system_context += "\n";
                }
                prepared.memory.items.push_back({ContextSource::Observation, skill.skill_id,
                    FormatSkillPromptBlock(skill), nlohmann::json{{"execution_id", skill.execution_id},
                        {"source_role", "tool_observation"}, {"uncertainty", "unknown"}}.dump()});
            }
        }
    }
    auto compiled = CompileContext(prepared.memory.items, options_.context_budgets, prepared.memory.budget_report);
    logger_.info("[trace={}] context planned l0_bytes={} l3_bytes={} l4_bytes={} observation_bytes={} dropped={} duplicates={}",
        prepared.request.trace_id, prepared.memory.budget_report.input_bytes[0], prepared.memory.budget_report.input_bytes[1],
        prepared.memory.budget_report.input_bytes[2], prepared.memory.budget_report.input_bytes[3],
        prepared.memory.budget_report.dropped_ids.size(), prepared.memory.budget_report.duplicate_ids.size());
    if (!compiled.ok()) { ObserveTurn(prepared, compiled.status()); return compiled.status(); }
    prepared.memory.system_context = std::move(compiled).value();
    prepared.latency.memory_context = Since(memory_start);
    logger_.info("[trace={}] [persona_runtime] memory context done session={} latency_ms={} l0_hit={} l3_hit={} l4_hit={}",
                 prepared.request.trace_id,
                 session.session_id,
                 prepared.latency.memory_context.count(),
                 prepared.memory.l0_hit,
                 prepared.memory.l3_hit,
                 prepared.memory.l4_hit);

    return prepared;
}

core::Result<PersonaRuntime::PreparedChat> PersonaRuntime::PrepareChatAfterEmotion(
    SessionState& session,
    PreparedChat prepared,
    std::vector<ConversationTurn> recent_copy,
    EmotionAnalysis emotion) {
    prepared.user_emotion = std::move(emotion);

    auto calibration_status = MaybeRecordEmotionCalibrationSample(
        session,
        prepared.request,
        prepared.user_emotion,
        recent_copy);
    if (!calibration_status.ok()) {
        logger_.warn("[trace={}] [persona_runtime] emotion calibration sample skipped session={} reason={}",
                     prepared.request.trace_id,
                     session.session_id,
                     calibration_status.message());
    }

    const auto base_generation = prepared.request.generation_override.value_or(
        options_.emotion_generation.default_generation);
    auto adjusted = ApplyEmotionAdaptiveGeneration(
        base_generation, prepared.user_emotion, options_.emotion_generation);
    adjusted = session.emotion_state.GetParamAdjustments(adjusted);
    const int maximum_tokens = std::max(
        1,
        static_cast<int>(std::floor(
            base_generation.max_tokens * options_.emotion_generation.max_token_ratio)));
    const int minimum_tokens = std::min(
        options_.emotion_generation.min_tokens, maximum_tokens);
    adjusted.max_tokens = std::clamp(
        adjusted.max_tokens, minimum_tokens, maximum_tokens);
    prepared.generation = adjusted;
    auto hint = session.emotion_state.GetPromptHint();

    const auto prompt_start = std::chrono::steady_clock::now();
    auto messages = BuildMessages(session,
                                  prepared.memory,
                                  prepared.user_emotion,
                                  prepared.request.user_input,
                                  prepared.request.proactive_trigger,
                                  hint);
    if (!messages.ok()) {
        logger_.warn("[trace={}] [persona_runtime] prompt build failed session={} code={} reason={}",
                     prepared.request.trace_id,
                     session.session_id,
                     static_cast<int>(messages.status().code()),
                     messages.status().message());
        return messages.status();
    }
    prepared.messages = std::move(messages).value();
    if (auto status = ValidatePromptCharacterBudget(
            prepared.messages, prepared.tools, session.session_id, prepared.request.trace_id);
        !status.ok()) { ObserveTurn(prepared, status); return status; }
    prepared.latency.prompt_build = Since(prompt_start);
    logger_.info("[trace={}] [persona_runtime] prompt build done session={} latency_ms={} messages={}",
                 prepared.request.trace_id,
                 session.session_id,
                 prepared.latency.prompt_build.count(),
                 prepared.messages.size());

    logger_.info("[trace={}] [persona_runtime] prepared chat session={} user={} recent={} l0_hit={} l3_hit={}",
                 prepared.request.trace_id,
                 session.session_id,
                 session.user_uuid,
                 prepared.memory.recent_turns.size(),
                 prepared.memory.l0_hit,
                 prepared.memory.l3_hit);
    return prepared;
}

core::Result<std::vector<llm::ChatMessage>> PersonaRuntime::BuildMessages(
    SessionState& session,
    const RecalledContext& memory,
    const EmotionAnalysis& emotion,
    std::string_view user_input,
    const std::optional<std::string>& proactive_trigger,
    const std::optional<std::string>& emotion_hint) const {
    if (!session.prompt_builder) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "session prompt builder is missing");
    }

    auto system = session.prompt_builder->BuildSystemPrompt(
        memory.system_context,
        emotion,
        emotion_hint);
    if (!system.ok()) {
        return system.status();
    }

    auto system_prompt = std::move(system).value();
    const bool proactive = proactive_trigger.has_value();
    if (proactive && !proactive_trigger->empty()) {
        system_prompt += "\n<proactive_trigger>";
        system_prompt.append(*proactive_trigger);
        system_prompt += "</proactive_trigger>";
        system_prompt += "\n你可以主动找话题聊，或者接上之前的对话继续说。如果实在没什么好说的，回复空字符串即可。";
    }

    std::vector<llm::ChatMessage> messages;
    messages.reserve(2 + memory.recent_turns.size() * 2);
    messages.push_back(llm::ChatMessage{llm::ChatRole::System, std::move(system_prompt)});
    for (const auto& turn : memory.recent_turns) {
        if (!turn.user_input.empty()) {
            messages.push_back(llm::ChatMessage{llm::ChatRole::User, turn.user_input});
        }
        if (!turn.response.empty()) {
            messages.push_back(llm::ChatMessage{llm::ChatRole::Assistant, turn.response});
        }
    }
    messages.push_back(llm::ChatMessage{
        llm::ChatRole::User,
        proactive ? std::string("...") : std::string(user_input)});
    return messages;
}

core::Status PersonaRuntime::ValidatePromptCharacterBudget(
    const std::vector<llm::ChatMessage>& messages,
    const std::vector<llm::ChatCompletionRequest::Tool>& tools,
    std::string_view session_id,
    std::string_view trace_id) const {
    auto prompt_characters = PromptRequestCharacters(messages, tools);
    if (!prompt_characters.ok()) return prompt_characters.status();
    if (options_.max_prompt_characters == 0 ||
        prompt_characters.value() > options_.max_prompt_characters) {
        logger_.warn("[trace={}] [persona_runtime] prompt rejected session={} characters={} budget={}",
                     trace_id, session_id, prompt_characters.value(), options_.max_prompt_characters);
        return core::Status::Error(core::ErrorCode::ResourceExhausted,
                                   "prompt exceeds the configured UTF-8 character budget");
    }
    return core::Status::Ok();
}

core::Status PersonaRuntime::MaybeRecordEmotionCalibrationSample(
    const SessionState& session,
    const ChatRequest& request,
    const EmotionAnalysis& emotion,
    const std::vector<ConversationTurn>& recent_turns) const {
    if (!options_.emotion_calibration.enabled || !emotion_calibration_sink_) {
        return core::Status::Ok();
    }
    if (request.user_input.size() < options_.emotion_calibration.min_text_length) {
        return core::Status::Ok();
    }
    if (!std::isfinite(emotion.emotion.primary_prob)) {
        return core::Status::Ok();
    }
    auto reason = EmotionCalibrationReason(emotion, options_.emotion_calibration);
    if (!reason) {
        return core::Status::Ok();
    }

    EmotionCalibrationSample sample;
    sample.trace_id = request.trace_id;
    sample.session_id = session.session_id;
    sample.user_uuid = session.user_uuid;
    sample.persona_id = session.persona_id;
    sample.text = request.user_input;
    sample.bert_result = emotion;
    sample.state_snapshot = session.emotion_state.state();
    sample.recent_turns = recent_turns;
    sample.reason = std::move(*reason);
    return emotion_calibration_sink_->Record(sample);
}

core::Result<ChatResponse> PersonaRuntime::CompleteWithLlm(SessionState& session,
                                                            PreparedChat prepared) {
    if (!prepared.tool_plan.prefetch.tool_calls.empty()) {
        if (!skill_tool_coordinator_) return core::Status::Error(core::ErrorCode::FailedPrecondition,
            "prefetch requires a tool coordinator");
        auto results = skill_tool_coordinator_->Execute(prepared.tool_plan.prefetch, ToolCallContext(session, prepared));
        if (!results.ok()) return results.status();
        llm::ChatMessage assistant;
        assistant.role = llm::ChatRole::Assistant;
        assistant.tool_calls = prepared.tool_plan.prefetch.tool_calls;
        prepared.messages.push_back(std::move(assistant));
        if (auto status = AppendToolResults(prepared, prepared.tool_plan.prefetch, std::move(results).value()); !status.ok())
            return status;
    }
    const auto turn_user_input = prepared.request.user_input;
    const auto turn_context_id = prepared.request.context_id;
    const auto io_stage_start = std::chrono::steady_clock::now();
    std::optional<AnswerCacheLookupRequest> answer_cache_lookup;
    std::optional<AnswerCacheLookupResult> answer_cache_hit;
    if (answer_cache_provider_ && !prepared.request.stream && prepared.tool_plan.required_names.empty()) {
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
            answer_cache_hit = std::move(cache_result).value();
        } else {
            answer_cache_lookup = std::move(lookup);
        }
    }

    const auto llm_start = std::chrono::steady_clock::now();
    llm::ChatCompletionResponse completion;
    if (answer_cache_hit) {
        completion.content = answer_cache_hit->response;
        completion.model = prepared.request.model.empty() ? options_.default_model : prepared.request.model;
        prepared.latency.llm_total = std::chrono::milliseconds{0};
    } else {
        llm::ChatCompletionRequest llm_req;
        llm_req.model = prepared.request.model.empty() ? options_.default_model : prepared.request.model;
        llm_req.messages = prepared.messages;
        llm_req.temperature = static_cast<float>(prepared.generation.temperature);
        llm_req.max_tokens = prepared.generation.max_tokens;
        llm_req.top_p = static_cast<float>(prepared.generation.top_p);
        llm_req.tools = prepared.tools;
        if (!llm_req.tools.empty()) llm_req.tool_choice = "auto";

        ObservePrompt(prepared);
        auto llm_result = llm_client_->Complete(llm_req);
        ObserveLlmResult(prepared, llm_result.ok() ? &llm_result.value() : nullptr, llm_result.status());
        prepared.latency.llm_total = Since(llm_start);
        if (!llm_result.ok()) {
            return llm_result.status();
        }
        completion = std::move(llm_result).value();
        // 同步阶段最多执行一轮工具调用；异步 continuation 保持后续扩展点。
        if (!completion.tool_calls.empty() && skill_tool_coordinator_) {
            prepared.prompt_tokens += completion.prompt_tokens;
            prepared.completion_tokens += completion.completion_tokens;
            prepared.total_tokens += completion.total_tokens;
            llm::ChatMessage assistant;
            assistant.role = llm::ChatRole::Assistant;
            assistant.content = completion.content;
            assistant.reasoning_content = completion.reasoning_content;
            assistant.tool_calls = completion.tool_calls;
            prepared.messages.push_back(std::move(assistant));
            auto tool_messages = skill_tool_coordinator_->Execute(
                completion,
                ToolCallContext(session, prepared));
            if (!tool_messages.ok()) return tool_messages.status();
            if (auto status = AppendToolResults(prepared, completion, std::move(tool_messages).value()); !status.ok())
                return status;
            if (auto status = CheckToolRequirements(prepared); !status.ok()) return status;
            if (auto budget = ValidatePromptCharacterBudget(
                    prepared.messages, prepared.tools, session.session_id, prepared.request.trace_id);
                !budget.ok()) return budget;
            llm_req.messages = prepared.messages;
            prepared.tool_round = 1;
            ObservePrompt(prepared);
            auto follow_up = llm_client_->Complete(llm_req);
            ObserveLlmResult(prepared, follow_up.ok() ? &follow_up.value() : nullptr, follow_up.status());
            if (!follow_up.ok()) return follow_up.status();
            completion = std::move(follow_up).value();
        }
        if (!completion.tool_calls.empty()) {
            return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                       "tool loop budget exhausted or coordinator missing");
        }
    }
    if (auto status = CheckToolRequirements(prepared); !status.ok()) return status;
    auto finalized = FinalizeLlmCompletion(
        session,
        std::move(prepared),
        std::move(completion),
        std::move(answer_cache_lookup),
        io_stage_start);
    if (!finalized.ok()) {
        return finalized.status();
    }
    auto value = std::move(finalized).value();
    ConversationTurn turn;
    turn.user_input = turn_user_input;
    turn.emotion = value.response.user_emotion.emotion.primary;
    turn.intensity = value.response.user_emotion.emotion.intensity;
    turn.behavior = value.response.user_emotion.behavior;
    turn.tone = value.response.user_emotion.tone;
    turn.response = value.response.response;
    turn.context_id = turn_context_id;
    turn.turn_id = session.session_id + ":" +
        std::to_string(session.metrics.turn_count + 1);
    turn.persona_id = session.persona_id;
    turn.valence = value.emotion_state.state().valence;
    turn.arousal = value.emotion_state.state().arousal;
    auto admit_status = memory_provider_->AdmitTurn(
        value.response.session_id,
        session.tenant_id,
        session.user_uuid,
        turn,
        value.response.trace_id);
    if (!admit_status.ok()) {
        return admit_status;
    }
    session.emotion_state = value.emotion_state;
    return std::move(value.response);
}


core::Result<std::shared_ptr<PersonaRuntime::AsyncTurnOperation>>
PersonaRuntime::BeginAsyncTurnOperation(
    SessionState session,
    ChatRequest request,
    std::shared_ptr<std::optional<ChatResponse>> response_holder,
    SessionManager::SessionTurnAsyncFinish finish,
    std::chrono::steady_clock::time_point submitted_at) {
    auto operation = std::make_shared<AsyncTurnOperation>(
        this,
        std::move(session),
        std::move(request),
        std::move(response_holder),
        std::move(finish),
        submitted_at);
    std::lock_guard lock(async_operations_mutex_);
    if (async_stopping_) {
        return core::Status::Error(core::ErrorCode::Cancelled,
                                   "persona runtime is shutting down");
    }
    ++async_turn_operation_count_;
    async_turns_by_session_[operation->session.session_id] = operation;
    operation->armed.store(true, std::memory_order_release);
    return operation;
}

core::Status PersonaRuntime::StartAsyncTurn(
    const std::shared_ptr<AsyncTurnOperation>& operation) {
    if (!operation->Transition(
            AsyncTurnPhase::Admitted,
            AsyncTurnPhase::UserEmotionPending)) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "async turn entered an invalid emotion phase");
    }

    if (!async_emotion_analyzer_) {
        auto emotion = emotion_analyzer_->Analyze(
            operation->request.user_input,
            operation->request.trace_id,
            operation->session.personality);
        OnAsyncUserEmotionCompleted(operation, std::move(emotion));
        return core::Status::Ok();
    }
    return async_emotion_analyzer_->AnalyzeAsync(
        operation->request.user_input,
        operation->request.trace_id,
        operation->session.personality,
        [this, operation](core::Result<EmotionAnalysis> emotion) mutable {
            OnAsyncUserEmotionCompleted(operation, std::move(emotion));
        });
}

core::Status PersonaRuntime::StartAsyncMemoryLookup(
    const std::shared_ptr<AsyncTurnOperation>& operation) {
    if (!async_memory_provider_) {
        auto recent_copy = std::vector<ConversationTurn>(
            operation->session.recent_history.begin(),
            operation->session.recent_history.end());
        MemoryContextRequest request;
        request.session_id = operation->session.session_id;
        request.tenant_id = operation->session.tenant_id;
        request.user_uuid = operation->session.user_uuid;
        request.persona_id = operation->session.persona_id;
        request.query = operation->request.user_input;
        request.trace_id = operation->request.trace_id;
        request.current_session_recent = recent_copy;
        request.max_recent_turns = options_.recent_raw_turns;
        const auto memory_start = std::chrono::steady_clock::now();
        OnAsyncMemoryCompleted(
            operation,
            memory_start,
            memory_provider_->BuildContext(request));
        return core::Status::Ok();
    }

    AsyncMemoryContextRequest memory_request;
    memory_request.session_id = operation->session.session_id;
    memory_request.tenant_id = operation->session.tenant_id;
    memory_request.user_uuid = operation->session.user_uuid;
    memory_request.persona_id = operation->session.persona_id;
    memory_request.query = operation->request.user_input;
    memory_request.trace_id = operation->request.trace_id;
    memory_request.current_session_recent.assign(
        operation->session.recent_history.begin(),
        operation->session.recent_history.end());
    memory_request.max_recent_turns = options_.recent_raw_turns;
    const auto memory_start = std::chrono::steady_clock::now();
    logger_.info(
        "[trace={}] [persona_runtime] async memory context start session={} user={} recent={}",
        operation->request.trace_id,
        operation->session.session_id,
        operation->session.user_uuid,
        memory_request.current_session_recent.size());

    return async_memory_provider_->BuildContextAsync(
        std::move(memory_request),
        [this, operation, memory_start](core::Result<RecalledContext> memory) mutable {
            OnAsyncMemoryCompleted(operation, memory_start, std::move(memory));
        });
}

void PersonaRuntime::OnAsyncMemoryCompleted(
    const std::shared_ptr<AsyncTurnOperation>& operation,
    std::chrono::steady_clock::time_point memory_start,
    core::Result<RecalledContext> memory) noexcept {
    if (memory.ok() && tool_memory_provider_ && !memory.value().tool_lookup_done) {
        if (!operation->Transition(AsyncTurnPhase::MemoryLookupPending, AsyncTurnPhase::ToolLookupPending)) return;
        try {
        auto holder = std::make_shared<RecalledContext>(std::move(memory).value());
        holder->tool_lookup_done = true;
        ToolMemoryQuery query{operation->session.session_id, operation->session.user_uuid,
            operation->session.persona_id, operation->request.trace_id, operation->request.user_input};
        auto delivered = std::make_shared<std::atomic<bool>>(false);
        auto complete = [this, operation, memory_start, holder, delivered](core::Result<ToolMemoryContext> tools) mutable {
            if (delivered->exchange(true)) return;
            if (tools.ok()) holder->recalled_tools = std::move(tools).value();
            else {
                holder->recalled_tools.emplace();
                holder->recalled_tools->degraded_reasons.push_back("recall_unavailable");
                logger_.warn("[trace={}] L4 recall degraded reason={}", operation->request.trace_id, tools.status().message());
            }
            OnAsyncMemoryCompleted(operation, memory_start, std::move(*holder));
        };
        try {
            auto status = tool_memory_provider_->QueryAsync(std::move(query), complete);
            if (!status.ok()) complete(status);
        } catch (const std::exception& error) {
            complete(core::Status::Error(core::ErrorCode::InternalError, error.what()));
        }
        return;
        } catch (const std::exception& error) {
            operation->Finish(core::Status::Error(core::ErrorCode::InternalError, error.what()));
            return;
        } catch (...) {
            operation->Finish(core::Status::Error(core::ErrorCode::InternalError, "tool memory query failed"));
            return;
        }
    }
    if (!operation->Transition(
            memory.ok() && memory.value().tool_lookup_done ? AsyncTurnPhase::ToolLookupPending : AsyncTurnPhase::MemoryLookupPending,
            AsyncTurnPhase::Preparing)) {
        return;
    }
    try {
        if (!memory.ok()) {
            logger_.warn(
                "[trace={}] [persona_runtime] async memory context failed session={} code={} reason={}",
                operation->request.trace_id,
                operation->session.session_id,
                static_cast<int>(memory.status().code()),
                memory.status().message());
            operation->Finish(memory.status());
            return;
        }
        std::vector<ConversationTurn> recent_copy(
            operation->session.recent_history.begin(),
            operation->session.recent_history.end());
        if (!operation->user_emotion) {
            operation->Finish(core::Status::Error(
                core::ErrorCode::FailedPrecondition,
                "user emotion is missing before memory continuation"));
            return;
        }
        auto prepared = PrepareChatBeforeEmotion(
            operation->session,
            operation->request,
            std::move(memory).value(),
            memory_start);
        if (!prepared.ok()) {
            operation->Finish(prepared.status());
            return;
        }
        ContinueAsyncTurn(
            operation,
            PrepareChatAfterEmotion(
                operation->session,
                std::move(prepared).value(),
                std::move(recent_copy),
                std::move(*operation->user_emotion)));
    } catch (const std::exception& exception) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::InternalError, exception.what()));
    } catch (...) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::Unknown,
            "async memory continuation threw an unknown exception"));
    }
}

void PersonaRuntime::OnAsyncUserEmotionCompleted(
    const std::shared_ptr<AsyncTurnOperation>& operation,
    core::Result<EmotionAnalysis> emotion) noexcept {
    if (!operation->Transition(
            AsyncTurnPhase::UserEmotionPending,
            AsyncTurnPhase::MemoryLookupPending)) {
        return;
    }
    try {
        if (!emotion.ok()) {
            logger_.warn(
                "[trace={}] [persona_runtime] async user emotion failed session={} code={} reason={}",
                operation->request.trace_id,
                operation->session.session_id,
                static_cast<int>(emotion.status().code()),
                emotion.status().message());
            operation->Finish(emotion.status());
            return;
        }
        operation->user_emotion = std::move(emotion).value();
        logger_.info(
            "[trace={}] [persona_runtime] async user emotion done session={}",
            operation->request.trace_id,
            operation->session.session_id);
        auto status = StartAsyncMemoryLookup(operation);
        if (!status.ok()) {
            operation->Finish(status);
        }
    } catch (const std::exception& exception) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::InternalError, exception.what()));
    } catch (...) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::Unknown,
            "async emotion continuation threw an unknown exception"));
    }
}

void PersonaRuntime::ContinueAsyncTurn(
    const std::shared_ptr<AsyncTurnOperation>& operation,
    core::Result<PreparedChat> prepared) noexcept {
    try {
        if (!prepared.ok()) {
            operation->Finish(prepared.status());
            return;
        }
        if (!operation->Transition(
                AsyncTurnPhase::Preparing,
                AsyncTurnPhase::LlmPending)) {
            return;
        }
        prepared.value().started_at = operation->submitted_at;
        prepared.value().latency.compute_queue_wait = operation->compute_queue_wait;
        prepared.value().latency.compute_stage = Since(operation->compute_started_at);
        prepared.value().io_submitted_at = std::chrono::steady_clock::now();
        prepared.value().cancel_requested = operation->cancel_requested;
        prepared.value().cancellation = operation->cancellation.get_token();
        auto status = CompleteWithLlmAsync(
            operation->session,
            std::move(prepared).value(),
            [this, operation](core::Result<CompletedChat> completed) mutable {
                OnAsyncLlmCompleted(operation, std::move(completed));
            });
        if (!status.ok()) {
            operation->Finish(status);
        }
    } catch (const std::exception& exception) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::InternalError, exception.what()));
    } catch (...) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::Unknown,
            "async turn continuation threw an unknown exception"));
    }
}

void PersonaRuntime::OnAsyncLlmCompleted(
    const std::shared_ptr<AsyncTurnOperation>& operation,
    core::Result<CompletedChat> completed) noexcept {
    if (!operation->Transition(
            AsyncTurnPhase::LlmPending,
            AsyncTurnPhase::MemoryAdmissionPending)) {
        return;
    }
    try {
        if (!completed.ok()) {
            operation->Finish(completed.status());
            return;
        }
        auto value = std::move(completed).value();
        auto completed_holder = std::make_shared<CompletedChat>(std::move(value));
        auto async_admission =
            std::dynamic_pointer_cast<IAsyncMemoryAdmissionProvider>(memory_provider_);
        if (async_admission) {
            auto status = async_admission->AdmitTurnAsync(
                operation->session.session_id,
                operation->session.tenant_id,
                operation->session.user_uuid,
                ConversationTurn{
                    .user_input = operation->user_input,
                    .emotion = completed_holder->response.user_emotion.emotion.primary,
                    .intensity = completed_holder->response.user_emotion.emotion.intensity,
                    .behavior = completed_holder->response.user_emotion.behavior,
                    .tone = completed_holder->response.user_emotion.tone,
                    .response = completed_holder->response.response,
                    .context_id = operation->context_id,
                    .persona_id = operation->persona_id,
                    .valence = completed_holder->emotion_state.state().valence,
                    .arousal = completed_holder->emotion_state.state().arousal,
                    .turn_id = operation->turn_id},
                completed_holder->response.trace_id,
                [this, operation, completed_holder](core::Status status) mutable {
                    OnAsyncMemoryAdmissionCompleted(
                        operation, std::move(*completed_holder), std::move(status));
                });
            if (status.ok()) {
                return;
            }
            if (operation->finished.load(std::memory_order_acquire)) {
                return;
            }
            logger_.warn(
                "[trace={}] [persona_runtime] async memory admission unavailable: {}",
                operation->request.trace_id,
                status.message());
            if (status.code() != core::ErrorCode::Unimplemented) {
                // 队列拒绝不能绕过背压；仅未装配异步能力的旧宿主保留同步兼容路径。
                OnAsyncMemoryAdmissionCompleted(operation, std::move(*completed_holder), std::move(status));
                return;
            }
        }
        if (memory_provider_) {
            const ConversationTurn turn{
                .user_input = operation->user_input,
                .emotion = completed_holder->response.user_emotion.emotion.primary,
                .intensity = completed_holder->response.user_emotion.emotion.intensity,
                .behavior = completed_holder->response.user_emotion.behavior,
                .tone = completed_holder->response.user_emotion.tone,
                .response = completed_holder->response.response,
                .context_id = operation->context_id,
                .persona_id = operation->persona_id,
                .valence = completed_holder->emotion_state.state().valence,
                .arousal = completed_holder->emotion_state.state().arousal,
                .turn_id = operation->turn_id};
            auto status = memory_provider_->AdmitTurn(
                operation->session.session_id,
                operation->session.tenant_id,
                operation->session.user_uuid,
                turn,
                completed_holder->response.trace_id);
            if (!status.ok()) {
                logger_.warn(
                    "[trace={}] [persona_runtime] memory admission failed after response: {}",
                    operation->request.trace_id,
                    status.message());
            }
        }
        operation->phase.store(
            AsyncTurnPhase::SessionCommitPending, std::memory_order_release);
        FinishAsyncCompletedChat(operation, std::move(*completed_holder));
    } catch (const std::exception& exception) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::InternalError, exception.what()));
    } catch (...) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::Unknown,
            "async LLM continuation threw an unknown exception"));
    }
}

void PersonaRuntime::OnAsyncMemoryAdmissionCompleted(
    const std::shared_ptr<AsyncTurnOperation>& operation,
    CompletedChat completed,
    core::Status status) noexcept {
    if (!operation->Transition(
            AsyncTurnPhase::MemoryAdmissionPending,
            AsyncTurnPhase::SessionCommitPending)) {
        return;
    }
    if (!status.ok()) {
        logger_.warn(
            "[trace={}] [persona_runtime] memory admission failed after response: {}",
            operation->request.trace_id,
            status.message());
    }
    FinishAsyncCompletedChat(operation, std::move(completed));
}

void PersonaRuntime::FinishAsyncCompletedChat(
    const std::shared_ptr<AsyncTurnOperation>& operation,
    CompletedChat value) noexcept {
    try {
        SessionTurnCommit commit;
        commit.trace_id = value.response.trace_id;
        commit.turn.user_input = operation->user_input;
        commit.turn.emotion = value.response.user_emotion.emotion.primary;
        commit.turn.intensity = value.response.user_emotion.emotion.intensity;
        commit.turn.behavior = value.response.user_emotion.behavior;
        commit.turn.tone = value.response.user_emotion.tone;
        commit.turn.response = value.response.response;
        commit.turn.context_id = operation->context_id;
        commit.turn.turn_id = operation->turn_id;
        commit.turn.persona_id = operation->persona_id;
        commit.turn.valence = value.emotion_state.state().valence;
        commit.turn.arousal = value.emotion_state.state().arousal;
        commit.emotion_state = std::move(value.emotion_state);
        commit.latency = value.response.latency.total;
        *operation->response_holder = std::move(value.response);
        operation->Finish(std::move(commit));
    } catch (const std::exception& exception) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::InternalError, exception.what()));
    } catch (...) {
        operation->Finish(core::Status::Error(
            core::ErrorCode::Unknown,
            "async LLM continuation threw an unknown exception"));
    }
}

void PersonaRuntime::FinishAsyncTurnOperation(const AsyncTurnOperation& operation) noexcept {
    {
        std::lock_guard lock(async_operations_mutex_);
        if (async_turn_operation_count_ > 0) {
            --async_turn_operation_count_;
        }
        auto turn = async_turns_by_session_.find(operation.session.session_id);
        if (turn != async_turns_by_session_.end()) {
            auto current = turn->second.lock();
            if (!current || current.get() == &operation) async_turns_by_session_.erase(turn);
        }
    }
    async_operations_drained_.notify_all();
}

void PersonaRuntime::Shutdown() noexcept {
    std::lock_guard shutdown_lock(shutdown_mutex_);
    callback_lifetime_.CloseAdmission();
    std::vector<std::shared_ptr<llm::IAsyncLlmOperation>> operations;
    std::vector<std::shared_ptr<AsyncTurnOperation>> turns;
    {
        std::lock_guard lock(async_operations_mutex_);
        async_stopping_ = true;
        for (const auto& [_, weak] : async_turns_by_session_) {
            if (auto turn = weak.lock()) turns.push_back(std::move(turn));
        }
        operations.reserve(async_operations_.size());
        for (const auto& [_, record] : async_operations_) {
            if (record.llm_operation) {
                operations.push_back(record.llm_operation);
            }
        }
    }
    for (const auto& turn : turns) turn->cancellation.request_stop();
    for (const auto& operation : operations) {
        operation->Cancel();
    }
    std::unique_lock lock(async_operations_mutex_);
    async_operations_drained_.wait(lock, [this] {
        return async_operations_.empty() && async_turn_operation_count_ == 0;
    });
    lock.unlock();
    // 不把 callback 生命周期和 scheduler quota 绑定；callback 内只有原子租约归还。
    callback_lifetime_.Wait();
}

core::Result<PersonaRuntime::CompletedChat> PersonaRuntime::FinalizeLlmCompletion(
    SessionState& session,
    PreparedChat prepared,
    llm::ChatCompletionResponse completion,
    std::optional<AnswerCacheLookupRequest> answer_cache_lookup,
    std::chrono::steady_clock::time_point io_stage_start) {
    if (answer_cache_provider_ && answer_cache_lookup) {
        AnswerCacheStoreRequest store;
        store.lookup = std::move(*answer_cache_lookup);
        store.response = completion.content;
        auto store_status = answer_cache_provider_->Store(store);
        if (!store_status.ok()) {
            return store_status;
        }
    }
    prepared.latency.total = Since(prepared.started_at);
    prepared.latency.io_stage = Since(io_stage_start);

    auto ai_emotion = emotion_analyzer_->Analyze(completion.content,
                                                 prepared.request.trace_id,
                                                 session.personality);
    if (!ai_emotion.ok()) {
        return ai_emotion.status();
    }

    return FinalizeLlmCompletionAfterEmotion(
        session,
        std::move(prepared),
        std::move(completion),
        std::move(ai_emotion).value(),
        io_stage_start);
}

core::Status PersonaRuntime::FinalizeLlmCompletionAsync(
    SessionState session,
    PreparedChat prepared,
    llm::ChatCompletionResponse llm_completion,
    std::optional<AnswerCacheLookupRequest> answer_cache_lookup,
    std::chrono::steady_clock::time_point io_stage_start,
    std::function<void(core::Result<CompletedChat>)> completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "LLM finalization completion is required");
    }
    if (prepared.cancel_requested && prepared.cancel_requested->load(std::memory_order_acquire)) {
        return core::Status::Error(core::ErrorCode::Cancelled, "persona turn cancelled by caller");
    }
    if (answer_cache_provider_ && answer_cache_lookup) {
        AnswerCacheStoreRequest store;
        store.lookup = std::move(*answer_cache_lookup);
        store.response = llm_completion.content;
        auto store_status = answer_cache_provider_->Store(store);
        if (!store_status.ok()) {
            return store_status;
        }
    }
    prepared.latency.total = Since(prepared.started_at);
    prepared.latency.io_stage = Since(io_stage_start);

    if (!async_emotion_analyzer_) {
        auto emotion = emotion_analyzer_->Analyze(
            llm_completion.content,
            prepared.request.trace_id,
            session.personality);
        if (!emotion.ok()) {
            completion(emotion.status());
        } else {
            completion(FinalizeLlmCompletionAfterEmotion(
                session,
                std::move(prepared),
                std::move(llm_completion),
                std::move(emotion).value(),
                io_stage_start));
        }
        return core::Status::Ok();
    }

    auto state = std::make_shared<std::tuple<
        SessionState,
        PreparedChat,
        llm::ChatCompletionResponse,
        std::function<void(core::Result<CompletedChat>)>>>(
            std::move(session),
            std::move(prepared),
            std::move(llm_completion),
            std::move(completion));
    const auto text = std::get<2>(*state).content;
    const auto trace_id = std::get<1>(*state).request.trace_id;
    const auto personality = std::get<0>(*state).personality;
    return async_emotion_analyzer_->AnalyzeAsync(
        text,
        trace_id,
        personality,
        [this, state, io_stage_start](core::Result<EmotionAnalysis> emotion) mutable {
            auto finish = [this, state, io_stage_start,
                           emotion_holder = std::make_shared<core::Result<EmotionAnalysis>>(
                               std::move(emotion))]() mutable {
                auto completion = std::move(std::get<3>(*state));
                const auto& cancellation = std::get<1>(*state).cancel_requested;
                if (cancellation && cancellation->load(std::memory_order_acquire)) {
                    completion(core::Status::Error(core::ErrorCode::Cancelled,
                                                  "persona turn cancelled by caller"));
                    return;
                }
                if (!emotion_holder->ok()) {
                    completion(emotion_holder->status());
                    return;
                }
                completion(FinalizeLlmCompletionAfterEmotion(
                    std::get<0>(*state),
                    std::move(std::get<1>(*state)),
                    std::move(std::get<2>(*state)),
                    std::move(*emotion_holder).value(),
                    io_stage_start));
            };
            if (!continuation_pool_) {
                finish();
                return;
            }
            auto holder = std::make_shared<std::function<void()>>(std::move(finish));
            auto status = continuation_pool_->Submit(
                [holder](core::ThreadPoolContext&) mutable {
                    (*holder)();
                    return core::Status::Ok();
                },
                {},
                "persona-ai-emotion-continuation");
            if (!status.ok()) {
                (*holder)();
            }
        });
}

core::Result<PersonaRuntime::CompletedChat>
PersonaRuntime::FinalizeLlmCompletionAfterEmotion(
    SessionState& session,
    PreparedChat prepared,
    llm::ChatCompletionResponse completion,
    EmotionAnalysis ai_emotion,
    std::chrono::steady_clock::time_point) {

    auto state_update = session.emotion_state.Update(
        prepared.user_emotion.emotion.primary,
        prepared.user_emotion.emotion.intensity,
        ai_emotion.emotion.primary,
        ai_emotion.emotion.intensity);
    if (!state_update.ok()) {
        return state_update.status();
    }
    ChatResponse response;
    response.session_id = prepared.request.session_id;
    response.trace_id = prepared.request.trace_id;
    response.response = completion.content;
    response.user_emotion = prepared.user_emotion;
    response.ai_emotion = std::move(ai_emotion);
    response.l0_hit = prepared.memory.l0_hit;
    response.l3_hit = prepared.memory.l3_hit;
    response.l4_hit = prepared.memory.l4_hit;
    response.turn_index = 0;
    response.answer_cache = std::move(prepared.answer_cache);
    response.latency = prepared.latency;
    response.messages = std::move(prepared.messages);
    response.prompt_tokens = prepared.prompt_tokens + completion.prompt_tokens;
    response.completion_tokens = prepared.completion_tokens + completion.completion_tokens;
    response.total_tokens = prepared.total_tokens + completion.total_tokens;
    response.context_budget = prepared.memory.budget_report;
    response.required_tools = prepared.tool_plan.required_names;
    response.satisfied_tools = prepared.tool_plan.satisfied_names;
    response.degraded_reasons = prepared.tool_plan.degraded_reasons;
    response.tools_recalled = prepared.tool_plan.recalled_ids;
    for (const auto& call : prepared.llm_calls)
        response.tool_calls.insert(response.tool_calls.end(), call.tool_calls.begin(), call.tool_calls.end());
    ObserveTurn(prepared, core::Status::Ok());
    response.llm_calls = std::move(prepared.llm_calls);
    for (const auto& tool : prepared.tools) response.tools_declared.push_back(tool.name);
    response.latency.callback_to_response = Since(prepared.started_at) - response.latency.total;
    CompletedChat completed;
    completed.response = std::move(response);
    completed.emotion_state = session.emotion_state;
    return completed;
}

} // namespace agent::service::persona
