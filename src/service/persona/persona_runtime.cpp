#include "persona_runtime.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <map>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
namespace agent::service::persona {
namespace {
std::string FormatRecentTurn(const ConversationTurn& turn) {
    return "用户: " + turn.user_input + "\n助手: " + turn.response;
}
std::string FormatL3Facts(const std::vector<vector_storage::EntryRecord>& facts) {
    if (facts.empty()) {
        return {};
    }
    std::string out = "<memory_l3>\n";
    for (const auto& fact : facts) {
        if (!fact.payload.empty()) {
            out += "- " + fact.payload + "\n";
        }
    }
    out += "</memory_l3>";
    return out;
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
std::vector<ConversationTurn> TakeRecent(std::span<const ConversationTurn> turns, std::size_t limit) {
    std::vector<ConversationTurn> out;
    const auto count = std::min<std::size_t>(turns.size(), limit);
    out.reserve(count);
    const auto begin = turns.size() - count;
    for (std::size_t i = begin; i < turns.size(); ++i) {
        out.push_back(turns[i]);
    }
    return out;
}
/// 将 provider 的同步完成、异步完成和异常实现统一收敛为一次外部结果。
class MemoryBuildCompletionState final {
public:
    explicit MemoryBuildCompletionState(
        IAsyncMemoryContextProvider::BuildCompletion completion)
        : completion_(std::move(completion)) {}
    bool TryComplete(core::Result<RecalledContext> result) {
        bool expected = false;
        if (!completed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        // 状态可能被下游缓存持有；先移出回调，确保完成后及时释放 Runtime/session 捕获。
        auto completion = std::move(completion_);
        completion(std::move(result));
        return true;
    }
    bool CancelWithoutCompletion() noexcept {
        bool expected = false;
        return completed_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel);
    }
private:
    std::atomic<bool> completed_{false};
    IAsyncMemoryContextProvider::BuildCompletion completion_;
};
bool IsProactiveInput(std::string_view input) {
    return input.rfind("[proactive]", 0) == 0 ||
           input.rfind("[proactive_decision]", 0) == 0 ||
           input.rfind("conversation idle for ", 0) == 0 ||
           input.rfind("[system_event]", 0) == 0;
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
SemanticMemoryContextProvider::SemanticMemoryContextProvider(
    std::shared_ptr<semantic_cache::ISemanticCache> l0_cache,
    std::shared_ptr<memory::LongTermMemoryCompressor> l3_memory,
    SemanticMemoryContextProviderOptions options)
    : l0_cache_(std::move(l0_cache)),
      l3_memory_(std::move(l3_memory)),
      options_(options) {}
core::Result<RecalledContext> SemanticMemoryContextProvider::BuildContext(
    const MemoryContextRequest& request) {
    RecalledContext context;
    const auto recent_limit = request.max_recent_turns == 0
        ? options_.max_recent_turns
        : request.max_recent_turns;
    context.recent_turns = TakeRecent(request.current_session_recent, recent_limit);
    std::vector<std::string> sections;
    if (l0_cache_) {
        semantic_cache::CacheLookupRequest lookup;
        lookup.text = request.query;
        lookup.scope = semantic_cache::CacheScope::User;
        lookup.answer_type = semantic_cache::AnswerType::Personalized;
        lookup.tenant_id = request.tenant_id;
        lookup.user_id = request.user_uuid;
        lookup.session_id = request.session_id;
        lookup.persona_id = request.persona_id;
        lookup.extra["trace_id"] = request.trace_id;
        lookup.recent_turns.reserve(context.recent_turns.size());
        for (const auto& turn : context.recent_turns) {
            lookup.recent_turns.push_back(FormatRecentTurn(turn));
        }
        auto l0 = l0_cache_->Lookup(lookup);
        if (!l0.ok()) {
            return l0.status();
        }
        if (l0.value().hit && !l0.value().payload.empty()) {
            context.l0_hit = true;
            sections.push_back("<memory_l0>\n" + l0.value().payload + "\n</memory_l0>");
        }
    }
    if (l3_memory_) {
        auto facts = l3_memory_->SearchFacts(
            {request.tenant_id, request.user_uuid}, request.query, options_.l3_top_k);
        if (!facts.ok()) {
            return facts.status();
        }
        auto formatted = FormatL3Facts(facts.value());
        if (!formatted.empty()) {
            context.l3_hit = true;
            sections.push_back(std::move(formatted));
        }
    }
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i != 0) {
            context.system_context += "\n";
        }
        context.system_context += sections[i];
    }
    return context;
}
core::Status SemanticMemoryContextProvider::BuildContextAsync(
    AsyncMemoryContextRequest request,
    BuildCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "memory context completion is required");
    }
    RecalledContext context;
    const auto recent_limit = request.max_recent_turns == 0
        ? options_.max_recent_turns
        : request.max_recent_turns;
    context.recent_turns = TakeRecent(request.current_session_recent, recent_limit);
    auto completion_state = std::make_shared<MemoryBuildCompletionState>(
        std::move(completion));
    const auto l3_memory = l3_memory_;
    const auto l3_top_k = options_.l3_top_k;
    const auto user_uuid = request.user_uuid;
    const auto query = request.query;
    auto complete_remaining =
        [l3_memory, l3_top_k, tenant_id = request.tenant_id, user_uuid, query, completion_state](
            RecalledContext value) mutable {
            std::vector<std::string> sections;
            if (!value.system_context.empty()) {
                sections.push_back(std::move(value.system_context));
            }
            if (l3_memory) {
                auto facts = l3_memory->SearchFacts(
                    {tenant_id, user_uuid}, query, l3_top_k);
                if (!facts.ok()) {
                    completion_state->TryComplete(facts.status());
                    return;
                }
                auto formatted = FormatL3Facts(facts.value());
                if (!formatted.empty()) {
                    value.l3_hit = true;
                    sections.push_back(std::move(formatted));
                }
            }
            value.system_context.clear();
            for (std::size_t index = 0; index < sections.size(); ++index) {
                if (index != 0) {
                    value.system_context += "\n";
                }
                value.system_context += sections[index];
            }
            completion_state->TryComplete(std::move(value));
        };
    if (!l0_cache_) {
        complete_remaining(std::move(context));
        return core::Status::Ok();
    }
    auto async_l0 = std::dynamic_pointer_cast<semantic_cache::IAsyncSemanticCache>(l0_cache_);
    if (!async_l0) {
        MemoryContextRequest sync_request;
        sync_request.session_id = request.session_id;
        sync_request.tenant_id = request.tenant_id;
        sync_request.user_uuid = request.user_uuid;
        sync_request.persona_id = request.persona_id;
        sync_request.query = request.query;
        sync_request.trace_id = request.trace_id;
        sync_request.current_session_recent = request.current_session_recent;
        sync_request.max_recent_turns = request.max_recent_turns;
        completion_state->TryComplete(BuildContext(sync_request));
        return core::Status::Ok();
    }
    semantic_cache::CacheLookupRequest lookup;
    lookup.text = request.query;
    lookup.scope = semantic_cache::CacheScope::User;
    lookup.answer_type = semantic_cache::AnswerType::Personalized;
    lookup.tenant_id = request.tenant_id;
    lookup.user_id = request.user_uuid;
    lookup.session_id = request.session_id;
    lookup.persona_id = request.persona_id;
    lookup.extra["trace_id"] = request.trace_id;
    lookup.recent_turns.reserve(context.recent_turns.size());
    for (const auto& turn : context.recent_turns) {
        lookup.recent_turns.push_back(FormatRecentTurn(turn));
    }
    core::Status start_status;
    try {
        start_status = async_l0->LookupAsync(
            std::move(lookup),
            [context = std::move(context),
             complete_remaining = std::move(complete_remaining),
             completion_state](
                core::Result<semantic_cache::CacheLookupResult> l0) mutable {
                if (!l0.ok()) {
                    completion_state->TryComplete(l0.status());
                    return;
                }
                if (l0.value().hit && !l0.value().payload.empty()) {
                    context.l0_hit = true;
                    context.system_context =
                        "<memory_l0>\n" + l0.value().payload + "\n</memory_l0>";
                }
                complete_remaining(std::move(context));
            });
    } catch (const std::exception& exception) {
        start_status = core::Status::Error(core::ErrorCode::InternalError, exception.what());
    } catch (...) {
        start_status = core::Status::Error(
            core::ErrorCode::Unknown,
            "async semantic cache threw an unknown exception");
    }
    if (!start_status.ok()) {
        // 若下游违反契约并在返回失败前同步回调，以已完成结果为准，避免上层二次 finish。
        if (!completion_state->CancelWithoutCompletion()) {
            return core::Status::Ok();
        }
    }
    return start_status;
}
core::Status SemanticMemoryContextProvider::AdmitTurnAsync(
    std::string session_id,
    std::string tenant_id,
    std::string user_uuid,
    ConversationTurn turn,
    std::string trace_id,
    AdmissionCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "memory admission completion is required");
    }
    if (!l0_cache_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "L0 memory cache is not initialized");
    }
    const memory::MemoryOwner owner{tenant_id, user_uuid};
    auto async_l0 = std::dynamic_pointer_cast<semantic_cache::IAsyncSemanticCache>(l0_cache_);
    if (!async_l0) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "asynchronous L0 memory admission is not supported");
    }
    semantic_cache::CacheStoreRequest store;
    store.origin.text = turn.user_input;
    store.origin.scope = semantic_cache::CacheScope::User;
    store.origin.answer_type = semantic_cache::AnswerType::Personalized;
    store.origin.tenant_id = std::move(tenant_id);
    store.origin.user_id = std::move(user_uuid);
    store.origin.session_id = std::move(session_id);
    store.origin.persona_id = turn.persona_id;
    store.origin.extra["trace_id"] = std::move(trace_id);
    store.response_payload = turn.response;
    store.answer_type = semantic_cache::AnswerType::Personalized;
    auto completion_state = std::make_shared<std::atomic<bool>>(false);
    auto guarded_completion = [completion = std::move(completion), completion_state,
                               l3_memory = l3_memory_, owner](
                                  core::Status status) mutable {
        bool expected = false;
        if (!completion_state->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (status.ok() && l3_memory) {
            status = l3_memory->RegisterOwner(owner);
        }
        try {
            completion(std::move(status));
        } catch (...) {
            // Provider 回调不应将业务异常传播回存储 executor。
        }
    };
    core::Status status;
    try {
        status = async_l0->StoreAsync(std::move(store), std::move(guarded_completion));
    } catch (const std::exception& exception) {
        status = core::Status::Error(core::ErrorCode::InternalError, exception.what());
    } catch (...) {
        status = core::Status::Error(
            core::ErrorCode::Unknown,
            "asynchronous semantic cache store threw an unknown exception");
    }
    if (!status.ok()) {
        bool expected = false;
        // 违反“失败不回调”契约时，以已交付完成为准，避免二次 admission 收口。
        if (!completion_state->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return core::Status::Ok();
        }
    }
    return status;
}

core::Status SemanticMemoryContextProvider::AdmitTurn(std::string_view session_id,
                                                      std::string_view tenant_id,
                                                      std::string_view user_uuid,
                                                      const ConversationTurn& turn,
                                                      std::string_view) {
    if (!l0_cache_) {
        return core::Status::Ok();
    }

    const memory::MemoryOwner owner{std::string(tenant_id), std::string(user_uuid)};
    semantic_cache::CacheStoreRequest store;
    store.origin.text = turn.user_input;
    store.origin.scope = semantic_cache::CacheScope::User;
    store.origin.answer_type = semantic_cache::AnswerType::Personalized;
    store.origin.tenant_id = std::string(tenant_id);
    store.origin.user_id = std::string(user_uuid);
    store.origin.session_id = std::string(session_id);
    store.origin.persona_id = turn.persona_id;
    store.origin.extra["emotion"] = turn.emotion;
    store.origin.extra["intensity"] = std::to_string(turn.intensity);
    store.origin.extra["behavior"] = turn.behavior;
    store.origin.extra["tone"] = turn.tone;
    store.origin.extra["context_id"] = turn.context_id;
    if (turn.valence) {
        store.origin.extra["valence"] = std::to_string(*turn.valence);
    }
    if (turn.arousal) {
        store.origin.extra["arousal"] = std::to_string(*turn.arousal);
    }
    store.origin.extra["payload_type"] = "conversation_turn";
    store.response_payload = turn.response;
    store.answer_type = semantic_cache::AnswerType::Personalized;
    auto status = l0_cache_->Store(store);
    if (status.ok() && l3_memory_) {
        status = l3_memory_->RegisterOwner(owner);
    }
    return status;
}

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
    if (request.session_id.empty() || request.user_input.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "session_id and user_input are required");
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
    std::vector<std::string> triggered_skills;
    if (tool_memory_provider_) {
        ToolMemoryQuery tool_query;
        tool_query.session_id = session.session_id;
        tool_query.user_uuid = session.user_uuid;
        tool_query.persona_id = session.persona_id;
        tool_query.trace_id = prepared.request.trace_id;
        tool_query.query = prepared.request.user_input;
        auto tool_context = tool_memory_provider_->Query(tool_query);
        if (!tool_context.ok()) {
            logger_.warn("[trace={}] [persona_runtime] L4 tool memory skipped session={} reason={}",
                         prepared.request.trace_id,
                         session.session_id,
                         tool_context.status().message());
        } else if (tool_context.value().hit && !tool_context.value().prompt_block.empty()) {
            prepared.tools = tool_context.value().tools;
            for (const auto& hit : tool_context.value().hits) {
                if (!hit.tool_id.empty() &&
                    StatefulSkillRegistry::Instance().Contains(hit.tool_id)) {
                    triggered_skills.push_back(hit.tool_id);
                }
            }
            if (!prepared.memory.system_context.empty()) {
                prepared.memory.system_context += "\n";
            }
            prepared.memory.system_context += tool_context.value().prompt_block;
            prepared.memory.l4_hit = true;
        }
    }
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
                prepared.memory.system_context += FormatSkillPromptBlock(skill);
            }
        }
    }
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
    const bool proactive = IsProactiveInput(user_input);
    if (proactive) {
        system_prompt += "\n<proactive_trigger>";
        system_prompt.append(user_input);
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
    messages.push_back(llm::ChatMessage{llm::ChatRole::User, proactive ? std::string("...") : std::string(user_input)});
    return messages;
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
    const auto turn_user_input = prepared.request.user_input;
    const auto turn_context_id = prepared.request.context_id;
    const auto io_stage_start = std::chrono::steady_clock::now();
    std::optional<AnswerCacheLookupRequest> answer_cache_lookup;
    std::optional<AnswerCacheLookupResult> answer_cache_hit;
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

        auto llm_result = llm_client_->Complete(llm_req);
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
            assistant.tool_calls = completion.tool_calls;
            prepared.messages.push_back(std::move(assistant));
            auto tool_messages = skill_tool_coordinator_->Execute(
                completion,
                agent::skill::SkillToolCallContext{
                    session.session_id,
                    session.user_uuid,
                    session.persona_id,
                    prepared.request.trace_id,
                    std::chrono::seconds(30)});
            if (!tool_messages.ok()) return tool_messages.status();
            prepared.messages.insert(prepared.messages.end(),
                                     std::make_move_iterator(tool_messages.value().begin()),
                                     std::make_move_iterator(tool_messages.value().end()));
            llm_req.messages = prepared.messages;
            auto follow_up = llm_client_->Complete(llm_req);
            if (!follow_up.ok()) return follow_up.status();
            completion = std::move(follow_up).value();
        }
    }
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
    if (!operation->Transition(
            AsyncTurnPhase::MemoryLookupPending,
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
                    .arousal = completed_holder->emotion_state.state().arousal},
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
                .arousal = completed_holder->emotion_state.state().arousal};
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
    {
        std::lock_guard lock(async_operations_mutex_);
        async_stopping_ = true;
        operations.reserve(async_operations_.size());
        for (const auto& [_, record] : async_operations_) {
            if (record.llm_operation) {
                operations.push_back(record.llm_operation);
            }
        }
    }
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
    response.latency.callback_to_response = Since(prepared.started_at) - response.latency.total;
    CompletedChat completed;
    completed.response = std::move(response);
    completed.emotion_state = session.emotion_state;
    return completed;
}

} // namespace agent::service::persona
