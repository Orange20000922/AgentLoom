#pragma once

#include "isemantic_cache.h"
#include "long_term_memory_compressor.h"
#include "openai_llm_client.h"
#include "session_manager.h"
#include "skill_session_manager.h"
#include "stateful_skill_registry.h"
#include "../../skill/skill_executor.h"
#include "tool_memory_provider.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

namespace agent::service::persona {

struct RecalledContext {
    std::string system_context;
    std::vector<ConversationTurn> recent_turns;
    bool l0_hit = false;
    bool l3_hit = false;
    bool l4_hit = false;
};

struct MemoryContextRequest {
    std::string session_id;
    std::string tenant_id = "default";
    std::string user_uuid;
    std::string persona_id;
    std::string query;
    std::string trace_id;
    std::span<const ConversationTurn> current_session_recent;
    std::size_t max_recent_turns = 10;
};

struct AsyncMemoryContextRequest {
    std::string session_id;
    std::string tenant_id = "default";
    std::string user_uuid;
    std::string persona_id;
    std::string query;
    std::string trace_id;
    std::vector<ConversationTurn> current_session_recent;
    std::size_t max_recent_turns = 10;
};

class IMemoryContextProvider {
public:
    virtual ~IMemoryContextProvider() = default;

    /// 构造本轮 L0/L3/L4 上下文。
    /// @param request session、用户、查询和当前会话最近回合；span 仅在调用期间有效。
    virtual core::Result<RecalledContext> BuildContext(const MemoryContextRequest& request) = 0;
    /// 在回复成功后接纳一个完整回合。
    /// @param turn 已完成的用户输入、回复和情绪元数据。
    virtual core::Status AdmitTurn(std::string_view session_id,
                                   std::string_view tenant_id,
                                   std::string_view user_uuid,
                                   const ConversationTurn& turn,
                                   std::string_view trace_id) = 0;
};

class IAsyncMemoryContextProvider {
public:
    using BuildCompletion = std::function<void(core::Result<RecalledContext>)>;

    virtual ~IAsyncMemoryContextProvider() = default;
    /// 返回非 OK 表示请求未被接纳，之后不得调用 completion；返回 OK 后必须恰好完成一次。
    virtual core::Status BuildContextAsync(AsyncMemoryContextRequest request,
                                           BuildCompletion completion) = 0;
};

class IAsyncMemoryAdmissionProvider {
public:
    using AdmissionCompletion = std::function<void(core::Status)>;

    virtual ~IAsyncMemoryAdmissionProvider() = default;
    /// 返回 OK 后必须恰好调用一次 completion；失败表示未接纳请求。
    virtual core::Status AdmitTurnAsync(
        std::string session_id,
        std::string tenant_id,
        std::string user_uuid,
        ConversationTurn turn,
        std::string trace_id,
        AdmissionCompletion completion) = 0;
};

struct SemanticMemoryContextProviderOptions {
    std::size_t max_recent_turns = 10;
    int l3_top_k = 5;
};

class SemanticMemoryContextProvider final : public IMemoryContextProvider,
                                            public IAsyncMemoryContextProvider,
                                            public IAsyncMemoryAdmissionProvider {
public:
    SemanticMemoryContextProvider(
        std::shared_ptr<semantic_cache::ISemanticCache> l0_cache,
        std::shared_ptr<memory::LongTermMemoryCompressor> l3_memory = nullptr,
        SemanticMemoryContextProviderOptions options = {});

    core::Result<RecalledContext> BuildContext(const MemoryContextRequest& request) override;
    core::Status BuildContextAsync(AsyncMemoryContextRequest request,
                                   BuildCompletion completion) override;
    core::Status AdmitTurnAsync(
        std::string session_id,
        std::string tenant_id,
        std::string user_uuid,
        ConversationTurn turn,
        std::string trace_id,
        AdmissionCompletion completion) override;
    core::Status AdmitTurn(std::string_view session_id,
                           std::string_view tenant_id,
                           std::string_view user_uuid,
                           const ConversationTurn& turn,
                           std::string_view trace_id) override;

private:
    std::shared_ptr<semantic_cache::ISemanticCache> l0_cache_;
    std::shared_ptr<memory::LongTermMemoryCompressor> l3_memory_;
    SemanticMemoryContextProviderOptions options_;
};

class IEmotionAnalyzer {
public:
    virtual ~IEmotionAnalyzer() = default;
    /// @param text 待分析 UTF-8 文本。
    /// @param trace_id 用于跨服务日志关联。
    /// @param personality 可选的人格配置，只读共享所有权。
    virtual core::Result<EmotionAnalysis> Analyze(std::string_view text,
                                                  std::string_view trace_id,
                                                  std::shared_ptr<const PersonalityConfig> personality = nullptr) = 0;
};

class IAsyncEmotionAnalyzer {
public:
    using AnalyzeCompletion = std::function<void(core::Result<EmotionAnalysis>)>;

    virtual ~IAsyncEmotionAnalyzer() = default;
    /// 返回 OK 后必须恰好完成一次；实现不得在等待远端推理时占用业务 worker。
    virtual core::Status AnalyzeAsync(
        std::string text,
        std::string trace_id,
        std::shared_ptr<const PersonalityConfig> personality,
        AnalyzeCompletion completion) = 0;
};

class NeutralEmotionAnalyzer final : public IEmotionAnalyzer {
public:
    core::Result<EmotionAnalysis> Analyze(std::string_view text,
                                          std::string_view trace_id,
                                          std::shared_ptr<const PersonalityConfig> personality = nullptr) override;
};

struct EmotionCalibrationOptions {
    bool enabled = false;
    double low_confidence_threshold = 0.45;
    double top_margin_threshold = 0.15;
    std::size_t min_text_length = 8;
};

struct EmotionCalibrationSample {
    std::string trace_id;
    std::string session_id;
    std::string user_uuid;
    std::string persona_id;
    std::string text;
    EmotionAnalysis bert_result;
    EmotionState state_snapshot;
    std::vector<ConversationTurn> recent_turns;
    std::string reason;
};

class IEmotionCalibrationSampleSink {
public:
    virtual ~IEmotionCalibrationSampleSink() = default;
    virtual core::Status Record(const EmotionCalibrationSample& sample) = 0;
};

struct ChatRequest {
    std::string session_id;
    std::string user_input;
    std::string trace_id;
    std::string context_id;
    std::optional<GenerationParams> generation_override;
    std::string model;
};

struct ChatLatencyBreakdown {
    std::chrono::milliseconds compute_queue_wait{0};
    std::chrono::milliseconds compute_stage{0};
    std::chrono::milliseconds io_queue_wait{0};
    std::chrono::milliseconds io_stage{0};
    std::chrono::milliseconds memory_context{0};
    std::chrono::milliseconds answer_cache{0};
    std::chrono::milliseconds prompt_build{0};
    std::chrono::milliseconds llm_total{0};
    std::chrono::milliseconds callback_to_response{0};
    std::chrono::milliseconds total{0};
};

struct AnswerCacheInfo {
    bool enabled = false;
    bool hit = false;
    bool bypassed = false;
    std::string source = "llm";
    std::string cache_key;
    float similarity_score = 0.0f;
};

struct AnswerCacheLookupRequest {
    std::string session_id;
    std::string user_uuid;
    std::string persona_id;
    std::string trace_id;
    std::string query;
    std::string model;
    GenerationParams generation;
    std::vector<llm::ChatMessage> messages;
};

struct AnswerCacheLookupResult {
    bool hit = false;
    std::string response;
    std::string cache_key;
    std::string source = "semantic_cache";
    float similarity_score = 0.0f;
};

struct AnswerCacheStoreRequest {
    AnswerCacheLookupRequest lookup;
    std::string response;
};

class IAnswerCacheProvider {
public:
    virtual ~IAnswerCacheProvider() = default;

    /// 查询是否可以绕过本轮 LLM；provider 必须自行执行 scope 和风险校验。
    virtual core::Result<AnswerCacheLookupResult> Lookup(const AnswerCacheLookupRequest& request) = 0;
    /// 保存一次成功生成结果；失败应记录日志但不改变已经返回的对话结果。
    virtual core::Status Store(const AnswerCacheStoreRequest& request) = 0;
};

struct ChatResponse {
    std::string session_id;
    std::string trace_id;
    std::string response;
    EmotionAnalysis user_emotion;
    EmotionAnalysis ai_emotion;
    bool l0_hit = false;
    bool l3_hit = false;
    bool l4_hit = false;
    std::uint64_t turn_index = 0;
    AnswerCacheInfo answer_cache;
    ChatLatencyBreakdown latency;
    std::vector<llm::ChatMessage> messages;
    // 本轮成功对话 LLM 调用的协议用量，含工具 follow-up；缓存直返为零。
    // Provider 未返回 usage 时沿用客户端的零值，不估算，也不包含情绪分析调用。
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t total_tokens = 0;
};

using ChatCallback = std::function<void(core::Result<ChatResponse>)>;

struct PersonaRuntimeOptions {
    std::size_t recent_raw_turns = 10;
    std::string default_model;
    EmotionCalibrationOptions emotion_calibration;
    EmotionGenerationOptions emotion_generation;
};

class PersonaRuntime {
public:
    /// 组装 Persona 对话运行时。
    /// @param sessions 借用的会话管理器，生命周期必须长于 PersonaRuntime。
    /// @param memory_provider 记忆上下文 provider，SubmitChat 前必须非空。
    /// @param emotion_analyzer 情绪分析 provider；无需情绪能力时可传 NeutralEmotionAnalyzer。
    /// @param llm_client 主对话 LLM provider，SubmitChat 前必须非空。
    /// @param answer_cache_provider 可选的答案直返缓存，不同于 L0 reference 注入。
    /// @param tool_memory_provider 可选的工具记忆和 Skill 触发 provider。
    /// @param skill_session_manager 可选的有限 Skill 会话管理器。
    PersonaRuntime(SessionManager& sessions,
                   std::shared_ptr<IMemoryContextProvider> memory_provider,
                   std::shared_ptr<IEmotionAnalyzer> emotion_analyzer,
                   std::shared_ptr<llm::ILlmClient> llm_client,
                   PersonaRuntimeOptions options = {},
                   std::shared_ptr<IAnswerCacheProvider> answer_cache_provider = nullptr,
                   std::shared_ptr<IToolMemoryProvider> tool_memory_provider = nullptr,
                   std::shared_ptr<ISkillSessionManager> skill_session_manager = nullptr,
                   core::LoggerAdapter logger = core::LoggerAdapter::ForModule("service"),
                   std::shared_ptr<IEmotionCalibrationSampleSink> emotion_calibration_sink = nullptr,
                   std::shared_ptr<llm::IAsyncLlmClient> async_llm_client = nullptr,
                   core::ThreadPool* continuation_pool = nullptr,
                   std::shared_ptr<agent::skill::ISkillToolCallCoordinator> skill_tool_coordinator = nullptr,
                   std::shared_ptr<IStatefulSkillExecutionRouter> stateful_skill_router = nullptr);
    ~PersonaRuntime();

    /// 异步提交对话；callback 恰好调用一次并携带最终 Result。
    /// @param request 本轮 session、输入、trace 和生成参数。
    /// @param callback 完成回调，不得为空；可能在线程池工作线程执行。
    core::Status SubmitChat(ChatRequest request, ChatCallback callback);
    /// 停止异步 admission，等待 memory continuation，并取消在途 LLM 后收口 Session commit。
    void Shutdown() noexcept;

private:
    enum class AsyncTurnPhase : std::uint8_t {
        Admitted,
        MemoryLookupPending,
        Preparing,
        UserEmotionPending,
        LlmPending,
        MemoryAdmissionPending,
        SessionCommitPending,
        Completed,
        Failed,
    };

    struct AsyncTurnOperation;

    struct PreparedChat {
        ChatRequest request;
        RecalledContext memory;
        EmotionAnalysis user_emotion;
        GenerationParams generation;
        std::vector<llm::ChatMessage> messages;
        std::vector<llm::ChatCompletionRequest::Tool> tools;
        std::size_t tool_round = 0;
        AnswerCacheInfo answer_cache;
        std::chrono::steady_clock::time_point started_at;
        std::chrono::steady_clock::time_point io_submitted_at;
        ChatLatencyBreakdown latency;
        // 保留工具调用首轮用量，最终结果再加最后一次生成，避免覆盖首轮计量。
        std::int64_t prompt_tokens = 0;
        std::int64_t completion_tokens = 0;
        std::int64_t total_tokens = 0;
    };

    struct CompletedChat {
        ChatResponse response;
        EmotionStateTracker emotion_state;
    };

    core::Result<PreparedChat> PrepareChat(SessionState& session, ChatRequest request);
    core::Result<PreparedChat> PrepareChatBeforeEmotion(
        SessionState& session,
        ChatRequest request,
        RecalledContext memory,
        std::chrono::steady_clock::time_point memory_start);
    core::Result<PreparedChat> PrepareChatAfterEmotion(
        SessionState& session,
        PreparedChat prepared,
        std::vector<ConversationTurn> recent_copy,
        EmotionAnalysis emotion);
    core::Result<std::vector<llm::ChatMessage>> BuildMessages(
        SessionState& session,
        const RecalledContext& memory,
        const EmotionAnalysis& emotion,
        std::string_view user_input,
        const std::optional<std::string>& emotion_hint) const;
    core::Status MaybeRecordEmotionCalibrationSample(const SessionState& session,
                                                     const ChatRequest& request,
                                                     const EmotionAnalysis& emotion,
                                                     const std::vector<ConversationTurn>& recent_turns) const;
    core::Result<ChatResponse> CompleteWithLlm(SessionState& session, PreparedChat prepared);
    core::Status CompleteWithLlmAsync(
        SessionState session,
        PreparedChat prepared,
        std::function<void(core::Result<CompletedChat>)> completion);
    core::Result<CompletedChat> FinalizeLlmCompletion(
        SessionState& session,
        PreparedChat prepared,
        llm::ChatCompletionResponse llm_completion,
        std::optional<AnswerCacheLookupRequest> answer_cache_lookup,
        std::chrono::steady_clock::time_point io_stage_start);
    core::Status FinalizeLlmCompletionAsync(
        SessionState session,
        PreparedChat prepared,
        llm::ChatCompletionResponse llm_completion,
        std::optional<AnswerCacheLookupRequest> answer_cache_lookup,
        std::chrono::steady_clock::time_point io_stage_start,
        std::function<void(core::Result<CompletedChat>)> completion);
    core::Result<CompletedChat> FinalizeLlmCompletionAfterEmotion(
        SessionState& session,
        PreparedChat prepared,
        llm::ChatCompletionResponse llm_completion,
        EmotionAnalysis ai_emotion,
        std::chrono::steady_clock::time_point io_stage_start);
    core::Result<std::shared_ptr<AsyncTurnOperation>> BeginAsyncTurnOperation(
        SessionState session,
        ChatRequest request,
        std::shared_ptr<std::optional<ChatResponse>> response_holder,
        SessionManager::SessionTurnAsyncFinish finish,
        std::chrono::steady_clock::time_point submitted_at);
    core::Status StartAsyncTurn(const std::shared_ptr<AsyncTurnOperation>& operation);
    core::Status StartAsyncMemoryLookup(
        const std::shared_ptr<AsyncTurnOperation>& operation);
    void OnAsyncMemoryCompleted(
        const std::shared_ptr<AsyncTurnOperation>& operation,
        std::chrono::steady_clock::time_point memory_start,
        core::Result<RecalledContext> memory) noexcept;
    void OnAsyncUserEmotionCompleted(
        const std::shared_ptr<AsyncTurnOperation>& operation,
        core::Result<EmotionAnalysis> emotion) noexcept;
    void ContinueAsyncTurn(
        const std::shared_ptr<AsyncTurnOperation>& operation,
        core::Result<PreparedChat> prepared) noexcept;
    void OnAsyncLlmCompleted(
        const std::shared_ptr<AsyncTurnOperation>& operation,
        core::Result<CompletedChat> completed) noexcept;
    void OnAsyncMemoryAdmissionCompleted(
        const std::shared_ptr<AsyncTurnOperation>& operation,
        CompletedChat completed,
        core::Status status) noexcept;
    void FinishAsyncCompletedChat(
        const std::shared_ptr<AsyncTurnOperation>& operation,
        CompletedChat completed) noexcept;
    void FinishAsyncTurnOperation() noexcept;

    SessionManager& sessions_;
    std::shared_ptr<IMemoryContextProvider> memory_provider_;
    std::shared_ptr<IAsyncMemoryContextProvider> async_memory_provider_;
    std::shared_ptr<IEmotionAnalyzer> emotion_analyzer_;
    std::shared_ptr<IAsyncEmotionAnalyzer> async_emotion_analyzer_;
    std::shared_ptr<llm::ILlmClient> llm_client_;
    std::shared_ptr<llm::IAsyncLlmClient> async_llm_client_;
    core::ThreadPool* continuation_pool_ = nullptr;
    std::shared_ptr<IAnswerCacheProvider> answer_cache_provider_;
    std::shared_ptr<IToolMemoryProvider> tool_memory_provider_;
    std::shared_ptr<ISkillSessionManager> skill_session_manager_;
    std::shared_ptr<agent::skill::ISkillToolCallCoordinator> skill_tool_coordinator_;
    std::shared_ptr<IStatefulSkillExecutionRouter> stateful_skill_router_;
    std::shared_ptr<IEmotionCalibrationSampleSink> emotion_calibration_sink_;
    PersonaRuntimeOptions options_;
    core::LoggerAdapter logger_;
    mutable std::mutex async_operations_mutex_;
    std::condition_variable async_operations_drained_;
    std::unordered_map<std::uint64_t, std::shared_ptr<llm::IAsyncLlmOperation>> async_operations_;
    std::size_t async_turn_operation_count_ = 0;
    std::uint64_t next_async_operation_id_ = 1;
    bool async_stopping_ = false;
};

} // namespace agent::service::persona
