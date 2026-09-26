#include "persona_runtime.h"
#include "gateway_session_affinity_scheduler.h"
#include "runtime_maintenance_service.h"
#include "skill_session_manager.h"

#include <gtest/gtest.h>

#include <future>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

class RuntimeTriggeredStatefulSkill final
    : public agent::service::persona::IStatefulSkillExecution {
public:
    explicit RuntimeTriggeredStatefulSkill(
        agent::service::persona::StatefulSkillExecutionContext context)
        : context_(std::move(context)) {}

    core::Status Start() override { return core::Status::Ok(); }

    core::Status Stop(
        const agent::service::persona::SkillSessionStopRequest& request) override {
        return context_.sessions->Stop(request).status();
    }

private:
    agent::service::persona::StatefulSkillExecutionContext context_;
};

REGISTER_STATEFUL_SKILL(RuntimeTriggeredStatefulSkill, "test.runtime.triggered", "1.0.0");

using agent::llm::ChatCompletionRequest;
using agent::llm::ChatCompletionResponse;
using agent::llm::ChatMessage;
using agent::semantic_cache::CacheLookupRequest;
using agent::semantic_cache::CacheLookupResult;
using agent::semantic_cache::CacheStoreRequest;
using agent::service::persona::ChatRequest;
using agent::service::persona::ChatResponse;
using agent::service::persona::CreateSessionRequest;
using agent::service::persona::EmotionAnalysis;
using agent::service::persona::EmotionCalibrationSample;
using agent::service::persona::GatewaySessionAffinityScheduler;
using agent::service::persona::GatewaySessionAffinitySchedulerOptions;
using agent::service::persona::GenerationParams;
using agent::service::persona::NeutralEmotionAnalyzer;
using agent::service::persona::PersonaRuntime;
using agent::service::persona::PersonaRuntimeOptions;
using agent::service::persona::PersonalityConfig;
using agent::service::persona::SemanticMemoryContextProvider;
using agent::service::persona::SessionManager;
using agent::service::persona::SkillObservation;
using agent::service::persona::SkillSessionManager;
using agent::service::persona::SkillSessionOptions;
using agent::service::persona::SkillSessionStartRequest;
using agent::service::persona::SkillSessionState;
using agent::service::persona::SkillSessionStopRequest;
using agent::service::persona::ToolMemoryContext;
using agent::service::persona::ToolMemoryHit;
using agent::service::persona::ToolMemoryQuery;

class ManualMemoryContextProvider final
    : public agent::service::persona::IMemoryContextProvider,
      public agent::service::persona::IAsyncMemoryContextProvider {
public:
    core::Result<agent::service::persona::RecalledContext> BuildContext(
        const agent::service::persona::MemoryContextRequest&) override {
        return core::Status::Error(core::ErrorCode::InternalError,
                                   "unexpected synchronous memory lookup");
    }

    core::Status BuildContextAsync(
        agent::service::persona::AsyncMemoryContextRequest request,
        BuildCompletion completion) override {
        {
            std::lock_guard lock(mutex_);
            request_ = std::move(request);
            completion_ = std::move(completion);
        }
        condition_.notify_all();
        return core::Status::Ok();
    }

    core::Status AdmitTurn(std::string_view,
                           std::string_view,
                           std::string_view,
                           const agent::service::persona::ConversationTurn&,
                           std::string_view) override {
        return core::Status::Ok();
    }

    bool WaitUntilPending() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return static_cast<bool>(completion_);
        });
    }

    void Complete(core::Result<agent::service::persona::RecalledContext> result) {
        BuildCompletion completion;
        {
            std::lock_guard lock(mutex_);
            completion = std::move(completion_);
        }
        completion(std::move(result));
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    agent::service::persona::AsyncMemoryContextRequest request_;
    BuildCompletion completion_;
};

class DuplicateCompletionMemoryContextProvider final
    : public agent::service::persona::IMemoryContextProvider,
      public agent::service::persona::IAsyncMemoryContextProvider {
public:
    core::Result<agent::service::persona::RecalledContext> BuildContext(
        const agent::service::persona::MemoryContextRequest&) override {
        return core::Status::Error(core::ErrorCode::InternalError,
                                   "unexpected synchronous memory lookup");
    }

    core::Status BuildContextAsync(
        agent::service::persona::AsyncMemoryContextRequest,
        BuildCompletion completion) override {
        completion(agent::service::persona::RecalledContext{});
        completion(agent::service::persona::RecalledContext{});
        return core::Status::Ok();
    }

    core::Status AdmitTurn(std::string_view,
                           std::string_view,
                           std::string_view,
                           const agent::service::persona::ConversationTurn&,
                           std::string_view) override {
        return core::Status::Ok();
    }
};

class FakeSemanticCache final : public agent::semantic_cache::ISemanticCache {
public:
    core::Result<CacheLookupResult> Lookup(const CacheLookupRequest& req) override {
        std::lock_guard lock(mutex_);
        last_lookup = req;
        CacheLookupResult result;
        result.hit = lookup_hit;
        result.payload = lookup_payload;
        result.similarity_score = 0.97f;
        return result;
    }

    core::Status Store(const CacheStoreRequest& req) override {
        std::lock_guard lock(mutex_);
        stores.push_back(req);
        return core::Status::Ok();
    }

    bool lookup_hit = true;
    std::string lookup_payload = "历史筛选上下文";
    CacheLookupRequest last_lookup;
    std::vector<CacheStoreRequest> stores;
    std::mutex mutex_;
};

class FakeLlmClient final : public agent::llm::ILlmClient {
public:
    core::Result<ChatCompletionResponse> Complete(const ChatCompletionRequest& req) override {
        std::lock_guard lock(mutex_);
        last_request = req;
        ChatCompletionResponse response;
        if (tool_round_trip && call_count++ == 0) {
            response.tool_calls.push_back({"persona-call-1", "vision.observe", R"({"reason":"look"})"});
        } else {
            response.content = tool_round_trip ? "工具结果后的最终回复" : "这是回复";
        }
        response.total_tokens = 42;
        return response;
    }

    bool tool_round_trip = false;
    int call_count = 0;
    ChatCompletionRequest last_request;
    std::mutex mutex_;
};

class RecordingToolCallCoordinator final
    : public agent::skill::ISkillToolCallCoordinator {
public:
    core::Result<std::vector<agent::llm::ChatMessage>> Execute(
        const agent::llm::ChatCompletionResponse& response,
        const agent::skill::SkillToolCallContext&) override {
        ++execute_count;
        if (!status.ok()) return status;
        EXPECT_EQ(response.tool_calls.size(), 1u);
        agent::skill::SkillResult result;
        result.call_id = response.tool_calls.front().id;
        result.skill_id = response.tool_calls.front().name;
        result.result_json = R"({"observed":true})";
        return std::vector<agent::llm::ChatMessage>{agent::skill::MakeToolResultMessage(result)};
    }

    core::Status ExecuteAsync(
        const agent::llm::ChatCompletionResponse& response,
        agent::skill::SkillToolCallContext context,
        Completion completion) override {
        auto result = Execute(response, context);
        completion(std::move(result));
        return core::Status::Ok();
    }

    int execute_count = 0;
    core::Status status = core::Status::Ok();
};

class DeferredToolCallCoordinator final
    : public agent::skill::ISkillToolCallCoordinator {
public:
    core::Result<std::vector<agent::llm::ChatMessage>> Execute(
        const agent::llm::ChatCompletionResponse& response,
        const agent::skill::SkillToolCallContext& context) override {
        return ExecuteResult(response, context);
    }

    core::Status ExecuteAsync(
        const agent::llm::ChatCompletionResponse& response,
        agent::skill::SkillToolCallContext context,
        Completion completion) override {
        {
            std::lock_guard lock(mutex_);
            pending_response_ = response;
            pending_context_ = std::move(context);
            pending_completion_ = std::move(completion);
        }
        condition_.notify_all();
        return core::Status::Ok();
    }

    bool WaitUntilPending(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return static_cast<bool>(pending_completion_); });
    }

    void CompletePending() {
        Completion completion;
        agent::llm::ChatCompletionResponse response;
        agent::skill::SkillToolCallContext context;
        {
            std::lock_guard lock(mutex_);
            completion = std::move(pending_completion_);
            response = std::move(pending_response_);
            context = std::move(pending_context_);
        }
        ASSERT_TRUE(completion);
        completion(ExecuteResult(response, context));
    }

    int execution_count() const noexcept { return execution_count_; }

private:
    std::vector<agent::llm::ChatMessage> ExecuteResult(
        const agent::llm::ChatCompletionResponse& response,
        const agent::skill::SkillToolCallContext&) {
        ++execution_count_;
        agent::skill::SkillResult result;
        result.call_id = response.tool_calls.front().id;
        result.skill_id = response.tool_calls.front().name;
        result.result_json = R"({"observed":true})";
        return {agent::skill::MakeToolResultMessage(result)};
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    agent::llm::ChatCompletionResponse pending_response_;
    agent::skill::SkillToolCallContext pending_context_;
    Completion pending_completion_;
    int execution_count_ = 0;
};

class ManualAsyncLlmClient final : public agent::llm::IAsyncLlmClient {
private:
    struct Pending;

    class Operation final : public agent::llm::IAsyncLlmOperation {
    public:
        explicit Operation(std::weak_ptr<Pending> pending)
            : pending_(std::move(pending)) {}

        void Cancel() noexcept override;

    private:
        std::weak_ptr<Pending> pending_;
    };

    struct Pending {
        ChatCompletionRequest request;
        Callback callback;
        std::atomic<bool> completed{false};

        void Finish(core::Result<ChatCompletionResponse> result) noexcept {
            if (completed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            callback(std::move(result));
        }
    };

public:
    core::Result<std::shared_ptr<agent::llm::IAsyncLlmOperation>> CompleteAsync(
        ChatCompletionRequest request,
        Callback callback) override {
        auto pending = std::make_shared<Pending>();
        pending->request = std::move(request);
        pending->callback = std::move(callback);
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(pending);
        }
        condition_.notify_all();
        return std::shared_ptr<agent::llm::IAsyncLlmOperation>(
            std::make_shared<Operation>(pending));
    }

    bool WaitForCount(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return pending_.size() >= count; });
    }

    void Complete(std::size_t index, std::string content) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(mutex_);
            ASSERT_LT(index, pending_.size());
            pending = pending_[index];
        }
        ChatCompletionResponse response;
        response.content = std::move(content);
        response.model = "async-test-model";
        pending->Finish(std::move(response));
    }

    void CompleteWithToolCall(std::size_t index,
                              std::string id,
                              std::string name,
                              std::string arguments) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(mutex_);
            ASSERT_LT(index, pending_.size());
            pending = pending_[index];
        }
        ChatCompletionResponse response;
        response.model = "async-test-model";
        response.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        pending->Finish(std::move(response));
    }

    ChatCompletionRequest RequestAt(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return pending_.at(index)->request;
    }

    std::size_t Count() const {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::shared_ptr<Pending>> pending_;
};

void ManualAsyncLlmClient::Operation::Cancel() noexcept {
    if (auto pending = pending_.lock()) {
        pending->Finish(core::Status::Error(core::ErrorCode::Cancelled,
                                            "manual async LLM operation cancelled"));
    }
}

class FixedEmotionAnalyzer final : public agent::service::persona::IEmotionAnalyzer {
public:
    explicit FixedEmotionAnalyzer(EmotionAnalysis analysis)
        : analysis_(std::move(analysis)) {}

    core::Result<EmotionAnalysis> Analyze(std::string_view,
                                          std::string_view,
                                          std::shared_ptr<const PersonalityConfig> = nullptr) override {
        return analysis_;
    }

private:
    EmotionAnalysis analysis_;
};

class ManualAsyncEmotionAnalyzer final
    : public agent::service::persona::IEmotionAnalyzer,
      public agent::service::persona::IAsyncEmotionAnalyzer {
public:
    core::Result<EmotionAnalysis> Analyze(
        std::string_view,
        std::string_view,
        std::shared_ptr<const PersonalityConfig> = nullptr) override {
        return core::Status::Error(core::ErrorCode::InternalError,
                                   "unexpected synchronous emotion analysis");
    }

    core::Status AnalyzeAsync(
        std::string text,
        std::string trace_id,
        std::shared_ptr<const PersonalityConfig>,
        AnalyzeCompletion completion) override {
        {
            std::lock_guard lock(mutex_);
            texts_.push_back(std::move(text));
            trace_ids_.push_back(std::move(trace_id));
            completions_.push_back(std::move(completion));
        }
        condition_.notify_all();
        return core::Status::Ok();
    }

    bool WaitForCount(std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return completions_.size() >= count;
        });
    }

    void Complete(std::size_t index, EmotionAnalysis emotion) {
        AnalyzeCompletion completion;
        {
            std::lock_guard lock(mutex_);
            completion = std::move(completions_.at(index));
        }
        completion(std::move(emotion));
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::string> texts_;
    std::vector<std::string> trace_ids_;
    std::vector<AnalyzeCompletion> completions_;
};

class RecordingEmotionCalibrationSink final : public agent::service::persona::IEmotionCalibrationSampleSink {
public:
    core::Status Record(const EmotionCalibrationSample& sample) override {
        std::lock_guard lock(mutex_);
        samples.push_back(sample);
        return core::Status::Ok();
    }

    std::vector<EmotionCalibrationSample> samples;
    std::mutex mutex_;
};

class FakeToolMemoryProvider final : public agent::service::persona::IToolMemoryProvider {
public:
    core::Result<ToolMemoryContext> Query(const ToolMemoryQuery& request) override {
        std::lock_guard lock(mutex_);
        last_query = request;
        ToolMemoryContext context;
        context.hit = hit;
        context.prompt_block = prompt_block;
        context.hits = hits;
        context.tools = tools;
        return context;
    }

    bool hit = true;
    std::string prompt_block =
        "<tool_memory_l4>\n"
        "- tool: vision.observe\n"
        "  instruction: use structured visual tool call only when needed\n"
        "</tool_memory_l4>";
    ToolMemoryQuery last_query;
    std::vector<ToolMemoryHit> hits;
    std::vector<agent::llm::ChatCompletionRequest::Tool> tools;
    std::mutex mutex_;
};

CreateSessionRequest MakeSessionRequest() {
    PersonalityConfig personality;
    personality.name = "小橘";
    personality.description = "教育陪伴人格";

    CreateSessionRequest req;
    req.user_uuid = "user-runtime";
    req.persona_id = "persona-main";
    req.session_id = "session-runtime";
    req.trace_id = "trace-create";
    req.personality = std::move(personality);
    req.time_awareness = false;
    req.emotion_state_config.noise_sigma = 0.0;
    return req;
}

EmotionAnalysis MakeEmotion(std::string primary, double intensity) {
    EmotionAnalysis analysis;
    analysis.emotion.primary = std::move(primary);
    analysis.emotion.intensity = intensity;
    analysis.emotion.primary_prob = 1.0;
    analysis.emotion.probabilities = {{analysis.emotion.primary, 1.0}};
    analysis.behavior = "unknown";
    analysis.tone = "neutral";
    return analysis;
}

EmotionAnalysis MakeUncertainEmotion() {
    EmotionAnalysis analysis;
    analysis.emotion.primary = "neutral";
    analysis.emotion.intensity = 0.2;
    analysis.emotion.primary_prob = 0.42;
    analysis.emotion.probabilities = {{"neutral", 0.42}, {"sadness", 0.37}, {"fear", 0.21}};
    analysis.behavior = "unknown";
    analysis.tone = "neutral";
    return analysis;
}

TEST(PersonaRuntimeTest, AsyncLlmReleasesWorkerAndPreservesPerSessionOrder) {
    core::ThreadPool compute({1, 32, "runtime-async-compute"});
    core::ThreadPool io({1, 32, "runtime-async-io"});
    core::ThreadPool llm_pool({1, 32, "runtime-async-llm",
                               std::make_shared<GatewaySessionAffinityScheduler>(
                                   GatewaySessionAffinitySchedulerOptions{
                                       .max_active_keys = 8,
                                       .max_outstanding_per_key = 4,
                                       .max_outstanding_per_fairness_key = 8,
                                       .max_outstanding_per_tenant = 16})});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(llm_pool.Start().ok());

    SessionManager sessions(
        compute, io, {}, core::LoggerAdapter::ForModule("service"), &llm_pool);
    auto first_session = MakeSessionRequest();
    first_session.session_id = "session-async-runtime-a";
    ASSERT_TRUE(sessions.CreateSession(std::move(first_session)).ok());
    auto second_session = MakeSessionRequest();
    second_session.session_id = "session-async-runtime-b";
    second_session.user_uuid = "user-runtime-b";
    ASSERT_TRUE(sessions.CreateSession(std::move(second_session)).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto sync_llm = std::make_shared<FakeLlmClient>();
    auto async_llm = std::make_shared<ManualAsyncLlmClient>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        sync_llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr,
        nullptr,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        nullptr,
        async_llm);

    auto submit = [&runtime](std::string session_id,
                             std::string input,
                             std::string trace_id,
                             std::promise<core::Result<ChatResponse>>& promise) {
        ChatRequest request;
        request.session_id = std::move(session_id);
        request.user_input = std::move(input);
        request.trace_id = std::move(trace_id);
        return runtime.SubmitChat(
            std::move(request),
            [&promise](auto result) { promise.set_value(std::move(result)); });
    };

    std::promise<core::Result<ChatResponse>> first_done;
    auto first_done_future = first_done.get_future();
    ASSERT_TRUE(submit("session-async-runtime-a", "a-first", "trace-a1", first_done).ok());
    ASSERT_TRUE(async_llm->WaitForCount(1, std::chrono::seconds(1)));

    std::promise<core::Result<ChatResponse>> same_session_done;
    auto same_session_done_future = same_session_done.get_future();
    ASSERT_TRUE(submit("session-async-runtime-a", "a-second", "trace-a2", same_session_done).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(async_llm->Count(), 1u);

    std::promise<core::Result<ChatResponse>> other_session_done;
    auto other_session_done_future = other_session_done.get_future();
    ASSERT_TRUE(submit("session-async-runtime-b", "b-first", "trace-b1", other_session_done).ok());
    ASSERT_TRUE(async_llm->WaitForCount(2, std::chrono::seconds(1)));

    async_llm->Complete(1, "b-response");
    ASSERT_EQ(other_session_done_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    auto other_result = other_session_done_future.get();
    ASSERT_TRUE(other_result.ok()) << other_result.status().message();
    EXPECT_EQ(other_result.value().response, "b-response");
    EXPECT_EQ(other_result.value().turn_index, 1u);

    async_llm->Complete(0, "a-first-response");
    ASSERT_EQ(first_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto first_result = first_done_future.get();
    ASSERT_TRUE(first_result.ok()) << first_result.status().message();
    EXPECT_EQ(first_result.value().turn_index, 1u);
    ASSERT_TRUE(async_llm->WaitForCount(3, std::chrono::seconds(1)));

    async_llm->Complete(2, "a-second-response");
    ASSERT_EQ(same_session_done_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    auto second_result = same_session_done_future.get();
    ASSERT_TRUE(second_result.ok()) << second_result.status().message();
    EXPECT_EQ(second_result.value().response, "a-second-response");
    EXPECT_EQ(second_result.value().turn_index, 2u);

    auto snapshot = sessions.GetSessionSnapshot("session-async-runtime-a");
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.value().metrics.turn_count, 2u);
    EXPECT_EQ(snapshot.value().recent_turn_count, 2u);

    sessions.CloseSession("session-async-runtime-a");
    sessions.CloseSession("session-async-runtime-b");
    llm_pool.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(PersonaRuntimeTest, AsyncEmotionPrecedesMemoryAndReleasesTurnWorker) {
    core::ThreadPool compute({1, 32, "runtime-order-compute"});
    core::ThreadPool io({1, 32, "runtime-order-io"});
    core::ThreadPool turn_pool({1, 32, "runtime-order-turn",
                                std::make_shared<GatewaySessionAffinityScheduler>()});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(turn_pool.Start().ok());

    SessionManager sessions(
        compute, io, {}, core::LoggerAdapter::ForModule("service"), &turn_pool);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto memory = std::make_shared<ManualMemoryContextProvider>();
    auto emotion = std::make_shared<ManualAsyncEmotionAnalyzer>();
    auto async_llm = std::make_shared<ManualAsyncLlmClient>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        std::make_shared<FakeLlmClient>(),
        PersonaRuntimeOptions{.default_model = "test-model"},
        nullptr,
        nullptr,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        nullptr,
        async_llm,
        &turn_pool);

    std::promise<core::Result<ChatResponse>> completed;
    auto completed_future = completed.get_future();
    ChatRequest request;
    request.session_id = "session-runtime";
    request.user_input = "先分析情绪，再查询记忆";
    request.trace_id = "trace-runtime-order";
    ASSERT_TRUE(runtime.SubmitChat(
        std::move(request),
        [&completed](core::Result<ChatResponse> result) mutable {
            completed.set_value(std::move(result));
        }).ok());
    ASSERT_TRUE(emotion->WaitForCount(1));
    EXPECT_EQ(async_llm->Count(), 0u);

    // 当前 Session 在等待 Emotion，但唯一 turn worker 应可处理其他 key。
    std::promise<void> worker_reused;
    auto worker_future = worker_reused.get_future();
    core::ThreadPoolTaskMetadata metadata;
    metadata.concurrency_key = "other-session";
    ASSERT_TRUE(turn_pool.Submit(
        [&worker_reused](core::ThreadPoolContext&) {
            worker_reused.set_value();
            return core::Status::Ok();
        },
        {},
        "emotion-wait-worker-probe",
        std::move(metadata)).ok());
    EXPECT_EQ(worker_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);

    emotion->Complete(0, MakeEmotion("curiosity", 0.8));
    ASSERT_TRUE(memory->WaitUntilPending());
    EXPECT_EQ(async_llm->Count(), 0u);

    memory->Complete(agent::service::persona::RecalledContext{});
    ASSERT_TRUE(async_llm->WaitForCount(1, std::chrono::seconds(1)));
    async_llm->Complete(0, "按严格阶段顺序生成的回复");

    // 回复侧情绪分析同样异步，完成前不得提交 Turn。
    ASSERT_TRUE(emotion->WaitForCount(2));
    EXPECT_EQ(completed_future.wait_for(std::chrono::milliseconds(50)),
              std::future_status::timeout);
    emotion->Complete(1, MakeEmotion("neutral", 0.5));
    ASSERT_EQ(completed_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    auto result = completed_future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().response, "按严格阶段顺序生成的回复");

    runtime.Shutdown();
    sessions.Shutdown();
    turn_pool.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(PersonaRuntimeTest, AsyncSkillExecutionReleasesWorkerAndResubmitsFollowUp) {
    core::ThreadPool compute({1, 32, "runtime-async-skill-compute"});
    core::ThreadPool io({1, 32, "runtime-async-skill-io"});
    core::ThreadPool turn_pool({1, 32, "runtime-async-skill-turn",
                                std::make_shared<GatewaySessionAffinityScheduler>()});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(turn_pool.Start().ok());

    SessionManager sessions(compute, io, {}, core::LoggerAdapter::ForModule("service"), &turn_pool);
    auto first = MakeSessionRequest();
    first.session_id = "session-async-skill-a";
    ASSERT_TRUE(sessions.CreateSession(std::move(first)).ok());
    auto second = MakeSessionRequest();
    second.session_id = "session-async-skill-b";
    second.user_uuid = "user-async-skill-b";
    ASSERT_TRUE(sessions.CreateSession(std::move(second)).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto async_llm = std::make_shared<ManualAsyncLlmClient>();
    auto tool_memory = std::make_shared<FakeToolMemoryProvider>();
    tool_memory->tools.push_back({"vision_observe", "observe", R"({"type":"object"})"});
    auto coordinator = std::make_shared<DeferredToolCallCoordinator>();
    PersonaRuntime runtime(
        sessions,
        memory,
        std::make_shared<NeutralEmotionAnalyzer>(),
        std::make_shared<FakeLlmClient>(),
        PersonaRuntimeOptions{.default_model = "test-model"},
        nullptr,
        tool_memory,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        nullptr,
        async_llm,
        &turn_pool,
        coordinator);

    std::promise<core::Result<ChatResponse>> first_done;
    auto first_future = first_done.get_future();
    ChatRequest first_request;
    first_request.session_id = "session-async-skill-a";
    first_request.user_input = "inspect the current screen";
    first_request.trace_id = "trace-async-skill-a";
    ASSERT_TRUE(runtime.SubmitChat(std::move(first_request),
        [&first_done](auto result) { first_done.set_value(std::move(result)); }).ok());
    ASSERT_TRUE(async_llm->WaitForCount(1, std::chrono::seconds(1)));
    async_llm->CompleteWithToolCall(0, "async-call-1", "vision_observe", R"({"reason":"inspect"})");
    ASSERT_TRUE(coordinator->WaitUntilPending(std::chrono::seconds(1)));

    // Skill execution is pending, but the single turn worker must accept another Session key.
    std::promise<core::Result<ChatResponse>> second_done;
    auto second_future = second_done.get_future();
    ChatRequest second_request;
    second_request.session_id = "session-async-skill-b";
    second_request.user_input = "ordinary response";
    second_request.trace_id = "trace-async-skill-b";
    ASSERT_TRUE(runtime.SubmitChat(std::move(second_request),
        [&second_done](auto result) { second_done.set_value(std::move(result)); }).ok());
    ASSERT_TRUE(async_llm->WaitForCount(2, std::chrono::seconds(1)));
    async_llm->Complete(1, "second session response");
    ASSERT_EQ(second_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_TRUE(second_future.get().ok());

    coordinator->CompletePending();
    ASSERT_TRUE(async_llm->WaitForCount(3, std::chrono::seconds(1)));
    auto follow_up_request = async_llm->RequestAt(2);
    ASSERT_GE(follow_up_request.messages.size(), 4u);
    EXPECT_EQ(follow_up_request.messages[follow_up_request.messages.size() - 2].role,
              agent::llm::ChatRole::Assistant);
    EXPECT_EQ(follow_up_request.messages.back().role, agent::llm::ChatRole::Tool);
    EXPECT_EQ(follow_up_request.messages.back().tool_call_id, "async-call-1");
    async_llm->Complete(2, "async skill follow-up response");
    ASSERT_EQ(first_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto first_result = first_future.get();
    ASSERT_TRUE(first_result.ok()) << first_result.status().message();
    EXPECT_EQ(first_result.value().response, "async skill follow-up response");
    EXPECT_EQ(coordinator->execution_count(), 1);

    runtime.Shutdown();
    sessions.Shutdown();
    turn_pool.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(PersonaRuntimeTest, AsyncTurnIgnoresDuplicateMemoryCompletion) {
    core::ThreadPool compute({1, 32, "runtime-duplicate-memory-compute"});
    core::ThreadPool io({1, 32, "runtime-duplicate-memory-io"});
    core::ThreadPool llm_pool({1, 32, "runtime-duplicate-memory-llm",
                               std::make_shared<GatewaySessionAffinityScheduler>(
                                   GatewaySessionAffinitySchedulerOptions{
                                       .max_active_keys = 8,
                                       .max_outstanding_per_key = 4,
                                       .max_outstanding_per_fairness_key = 8,
                                       .max_outstanding_per_tenant = 16})});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(llm_pool.Start().ok());

    SessionManager sessions(
        compute, io, {}, core::LoggerAdapter::ForModule("service"), &llm_pool);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto async_llm = std::make_shared<ManualAsyncLlmClient>();
    PersonaRuntime runtime(
        sessions,
        std::make_shared<DuplicateCompletionMemoryContextProvider>(),
        std::make_shared<NeutralEmotionAnalyzer>(),
        std::make_shared<FakeLlmClient>(),
        PersonaRuntimeOptions{.default_model = "test-model"},
        nullptr,
        nullptr,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        nullptr,
        async_llm);

    std::promise<core::Result<ChatResponse>> completed;
    auto future = completed.get_future();
    ChatRequest request;
    request.session_id = "session-runtime";
    request.user_input = "duplicate memory callback";
    request.trace_id = "trace-duplicate-memory";
    ASSERT_TRUE(runtime.SubmitChat(
        std::move(request),
        [&completed](core::Result<ChatResponse> result) mutable {
            completed.set_value(std::move(result));
        }).ok());
    ASSERT_TRUE(async_llm->WaitForCount(1, std::chrono::seconds(1)));
    EXPECT_EQ(async_llm->Count(), 1u);

    async_llm->Complete(0, "single response");
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().response, "single response");
    EXPECT_EQ(result.value().turn_index, 1u);

    runtime.Shutdown();
    sessions.Shutdown();
    llm_pool.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(PersonaRuntimeTest, ShutdownCancelsInflightAsyncLlmAndReleasesTurnLane) {
    core::ThreadPool compute({1, 16, "runtime-shutdown-compute"});
    core::ThreadPool io({1, 16, "runtime-shutdown-io"});
    core::ThreadPool llm_pool({1, 16, "runtime-shutdown-llm",
                               std::make_shared<GatewaySessionAffinityScheduler>()});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(llm_pool.Start().ok());
    SessionManager sessions(
        compute, io, {}, core::LoggerAdapter::ForModule("service"), &llm_pool);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto async_llm = std::make_shared<ManualAsyncLlmClient>();
    PersonaRuntime runtime(
        sessions,
        std::make_shared<SemanticMemoryContextProvider>(cache),
        std::make_shared<NeutralEmotionAnalyzer>(),
        std::make_shared<FakeLlmClient>(),
        PersonaRuntimeOptions{.default_model = "test-model"},
        nullptr,
        nullptr,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        nullptr,
        async_llm);

    std::promise<core::Result<ChatResponse>> completed;
    auto completed_future = completed.get_future();
    ChatRequest request;
    request.session_id = "session-runtime";
    request.user_input = "cancel this request";
    request.trace_id = "trace-runtime-shutdown";
    ASSERT_TRUE(runtime.SubmitChat(
        std::move(request),
        [&completed](auto result) { completed.set_value(std::move(result)); }).ok());
    ASSERT_TRUE(async_llm->WaitForCount(1, std::chrono::seconds(1)));

    runtime.Shutdown();
    ASSERT_EQ(completed_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto result = completed_future.get();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), core::ErrorCode::Cancelled);

    sessions.Shutdown();
    llm_pool.Shutdown(true);
    EXPECT_EQ(llm_pool.Stats().scheduler.running_tasks, 0u);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(PersonaRuntimeTest, ShutdownWaitsForAcceptedAsyncMemoryContinuation) {
    core::ThreadPool compute({1, 32, "runtime-memory-shutdown-compute"});
    core::ThreadPool io({1, 32, "runtime-memory-shutdown-io"});
    core::ThreadPool llm_pool({1, 32, "runtime-memory-shutdown-llm",
                               std::make_shared<GatewaySessionAffinityScheduler>(
                                   GatewaySessionAffinitySchedulerOptions{
                                       .max_active_keys = 8,
                                       .max_outstanding_per_key = 4,
                                       .max_outstanding_per_fairness_key = 8,
                                       .max_outstanding_per_tenant = 16})});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(llm_pool.Start().ok());

    SessionManager sessions(
        compute, io, {}, core::LoggerAdapter::ForModule("service"), &llm_pool);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto memory = std::make_shared<ManualMemoryContextProvider>();
    auto async_llm = std::make_shared<ManualAsyncLlmClient>();
    PersonaRuntime runtime(
        sessions,
        memory,
        std::make_shared<NeutralEmotionAnalyzer>(),
        std::make_shared<FakeLlmClient>(),
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr,
        nullptr,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        nullptr,
        async_llm);

    std::promise<core::Result<ChatResponse>> completed;
    auto completed_future = completed.get_future();
    ChatRequest request;
    request.session_id = "session-runtime";
    request.user_input = "等待异步记忆";
    request.trace_id = "trace-memory-shutdown";
    ASSERT_TRUE(runtime.SubmitChat(
        std::move(request),
        [&completed](core::Result<ChatResponse> result) mutable {
            completed.set_value(std::move(result));
        }).ok());
    ASSERT_TRUE(memory->WaitUntilPending());

    auto shutdown = std::async(std::launch::async, [&runtime] {
        runtime.Shutdown();
    });
    EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);

    memory->Complete(core::Status::Error(
        core::ErrorCode::Cancelled,
        "memory cancelled during shutdown"));
    ASSERT_EQ(completed_future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    auto result = completed_future.get();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), core::ErrorCode::Cancelled);
    EXPECT_EQ(shutdown.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);

    sessions.Shutdown();
    llm_pool.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(PersonaRuntimeTest, BuildsMessagesFromL0AndLastTenRawTurns) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    for (int i = 0; i < 12; ++i) {
        agent::service::persona::ConversationTurn turn;
        turn.user_input = "u" + std::to_string(i);
        turn.response = "a" + std::to_string(i);
        ASSERT_TRUE(sessions.AddTurn("session-runtime", std::move(turn), "trace-seed").ok());
    }

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"});

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "当前问题";
    chat.trace_id = "trace-chat";
    chat.model = "test-model";
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    ASSERT_EQ(result.value().messages.size(), 22u);
    EXPECT_NE(result.value().messages[0].content.find("<memory_l0>"), std::string::npos);
    EXPECT_NE(result.value().messages[0].content.find("历史筛选上下文"), std::string::npos);
    EXPECT_EQ(result.value().messages[1].content, "u2");
    EXPECT_EQ(result.value().messages[2].content, "a2");
    EXPECT_EQ(result.value().messages[19].content, "u11");
    EXPECT_EQ(result.value().messages[20].content, "a11");
    EXPECT_EQ(result.value().messages[21].content, "当前问题");
    EXPECT_TRUE(result.value().l0_hit);

    {
        std::lock_guard lock(llm->mutex_);
        EXPECT_EQ(llm->last_request.messages.size(), result.value().messages.size());
        EXPECT_EQ(llm->last_request.model, "test-model");
    }
    {
        std::lock_guard lock(cache->mutex_);
        EXPECT_EQ(cache->last_lookup.tenant_id, "default");
        EXPECT_EQ(cache->last_lookup.user_id, "user-runtime");
        EXPECT_EQ(cache->last_lookup.session_id, "session-runtime");
        EXPECT_EQ(cache->stores.size(), 1u);
        EXPECT_EQ(cache->stores[0].origin.tenant_id, "default");
        EXPECT_EQ(cache->stores[0].origin.user_id, "user-runtime");
        EXPECT_EQ(cache->stores[0].response_payload, "这是回复");
    }

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, AppliesEmotionAdaptiveGenerationWithoutBehaviorTonePrompting) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<FixedEmotionAnalyzer>(MakeEmotion("curiosity", 0.8));
    auto llm = std::make_shared<FakeLlmClient>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"});

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "为什么天空是蓝色的？";
    chat.trace_id = "trace-adaptive";
    chat.generation_override = GenerationParams{0.7, 2000, 0.9};
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    std::lock_guard lock(llm->mutex_);
    EXPECT_GE(llm->last_request.max_tokens, 2000);
    EXPECT_LE(llm->last_request.max_tokens, 2500);
    ASSERT_FALSE(llm->last_request.messages.empty());
    EXPECT_EQ(llm->last_request.messages.front().content.find("unknown"), std::string::npos);
    EXPECT_EQ(llm->last_request.messages.front().content.find("neutral"), std::string::npos);

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, BuildsProactivePromptWithTriggerInsteadOfUserUtterance) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"});

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "[proactive] frontend requested proactive generation";
    chat.trace_id = "trace-proactive";
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    std::lock_guard lock(llm->mutex_);
    ASSERT_GE(llm->last_request.messages.size(), 2u);
    EXPECT_NE(llm->last_request.messages.front().content.find("<proactive_trigger>"), std::string::npos);
    EXPECT_NE(llm->last_request.messages.front().content.find("你可以主动找话题聊"), std::string::npos);
    EXPECT_EQ(llm->last_request.messages.back().content, "...");

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, DoesNotRecordEmotionCalibrationSamplesByDefault) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<FixedEmotionAnalyzer>(MakeUncertainEmotion());
    auto llm = std::make_shared<FakeLlmClient>();
    auto sink = std::make_shared<RecordingEmotionCalibrationSink>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr,
        nullptr,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        sink);

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "我其实也不知道自己还能不能学会这个知识点";
    chat.trace_id = "trace-calibration-disabled";
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    std::lock_guard lock(sink->mutex_);
    EXPECT_TRUE(sink->samples.empty());

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, RecordsUncertainEmotionCalibrationSampleWhenEnabled) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    agent::service::persona::ConversationTurn seed;
    seed.user_input = "前面的问题我一直没太懂";
    seed.response = "我们可以慢慢拆开看。";
    ASSERT_TRUE(sessions.AddTurn("session-runtime", std::move(seed), "trace-seed").ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<FixedEmotionAnalyzer>(MakeUncertainEmotion());
    auto llm = std::make_shared<FakeLlmClient>();
    auto sink = std::make_shared<RecordingEmotionCalibrationSink>();
    PersonaRuntimeOptions options{.recent_raw_turns = 10, .default_model = "test-model"};
    options.emotion_calibration.enabled = true;
    options.emotion_calibration.low_confidence_threshold = 0.45;
    options.emotion_calibration.top_margin_threshold = 0.15;
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        options,
        nullptr,
        nullptr,
        nullptr,
        core::LoggerAdapter::ForModule("service"),
        sink);

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "我其实也不知道自己还能不能学会这个知识点";
    chat.trace_id = "trace-calibration-enabled";
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    std::lock_guard lock(sink->mutex_);
    ASSERT_EQ(sink->samples.size(), 1u);
    EXPECT_EQ(sink->samples[0].trace_id, "trace-calibration-enabled");
    EXPECT_EQ(sink->samples[0].session_id, "session-runtime");
    EXPECT_EQ(sink->samples[0].user_uuid, "user-runtime");
    EXPECT_EQ(sink->samples[0].persona_id, "persona-main");
    EXPECT_EQ(sink->samples[0].bert_result.emotion.primary, "neutral");
    EXPECT_EQ(sink->samples[0].reason, "low_confidence");
    EXPECT_EQ(sink->samples[0].recent_turns.size(), 1u);

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, InjectsTriggeredL4ToolMemoryIntoSystemPrompt) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();
    auto tool_memory = std::make_shared<FakeToolMemoryProvider>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr,
        tool_memory);

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "你看一下现在画面里有什么";
    chat.trace_id = "trace-l4-tool-memory";
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    EXPECT_TRUE(result.value().l4_hit);
    std::lock_guard lock(llm->mutex_);
    ASSERT_FALSE(llm->last_request.messages.empty());
    EXPECT_NE(llm->last_request.messages.front().content.find("<tool_memory_l4>"), std::string::npos);
    EXPECT_NE(llm->last_request.messages.front().content.find("vision.observe"), std::string::npos);

    {
        std::lock_guard tool_lock(tool_memory->mutex_);
        EXPECT_EQ(tool_memory->last_query.user_uuid, "user-runtime");
        EXPECT_EQ(tool_memory->last_query.query, "你看一下现在画面里有什么");
    }

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, StartsStatefulSkillSessionWhenL4ToolIsTriggered) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();
    auto tool_memory = std::make_shared<FakeToolMemoryProvider>();
    tool_memory->hits.push_back(ToolMemoryHit{.tool_id = "test.runtime.triggered"});
    auto skill_sessions = std::make_shared<SkillSessionManager>();
    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr,
        tool_memory,
        skill_sessions);

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "你看一下现在画面里有什么";
    chat.trace_id = "trace-vision-skill-start";
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    auto session = skill_sessions->Get("session-runtime", "test.runtime.triggered");
    ASSERT_TRUE(session.ok()) << session.status().message();
    ASSERT_TRUE(session.value().has_value());
    EXPECT_EQ(session.value()->state, SkillSessionState::Starting);

    std::lock_guard lock(llm->mutex_);
    ASSERT_FALSE(llm->last_request.messages.empty());
    EXPECT_NE(llm->last_request.messages.front().content.find("<skill_status"), std::string::npos);
    EXPECT_NE(
        llm->last_request.messages.front().content.find("test.runtime.triggered"),
        std::string::npos);

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, InjectsVisionObservationFromRunningSkillSession) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();
    auto skill_sessions = std::make_shared<SkillSessionManager>();

    SkillSessionStartRequest start;
    start.skill_id = "vision.observe";
    start.session_id = "session-runtime";
    start.user_uuid = "user-runtime";
    start.persona_id = "persona-main";
    start.trace_id = "trace-seed";
    start.source = "test";
    ASSERT_TRUE(skill_sessions->Start(start).ok());
    ASSERT_TRUE(skill_sessions->MarkReady("session-runtime", "vision.observe", "vision ready", "trace-ready").ok());
    SkillObservation observation;
    observation.skill_id = "vision.observe";
    observation.session_id = "session-runtime";
    observation.trace_id = "trace-observation";
    observation.summary = "画面中检测到明显移动";
    observation.confidence = 0.72;
    observation.source = "vlm";
    observation.should_inject_prompt = true;
    ASSERT_TRUE(skill_sessions->RecordObservation(observation).ok());

    PersonaRuntime runtime(
        sessions,
        memory,
        emotion,
        llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr,
        nullptr,
        skill_sessions);

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest chat;
    chat.session_id = "session-runtime";
    chat.user_input = "现在情况怎么样？";
    chat.trace_id = "trace-vision-observation";
    auto submit = runtime.SubmitChat(
        std::move(chat),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    ASSERT_TRUE(submit.ok()) << submit.message();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();

    std::lock_guard lock(llm->mutex_);
    ASSERT_FALSE(llm->last_request.messages.empty());
    EXPECT_NE(llm->last_request.messages.front().content.find("<skill_observation"), std::string::npos);
    EXPECT_NE(llm->last_request.messages.front().content.find("画面中检测到明显移动"), std::string::npos);

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SkillSessionManagerTest, RunsStartReadyObservationAndStopLifecycle) {
    SkillSessionManager manager;
    SkillSessionStartRequest start;
    start.skill_id = "vision.observe";
    start.session_id = "session-runtime";
    start.user_uuid = "user-runtime";
    start.persona_id = "persona-main";
    start.trace_id = "trace-skill-start";
    start.source = "regex";
    start.reason = "用户请求观察画面";

    auto started = manager.Start(start);

    ASSERT_TRUE(started.ok()) << started.status().message();
    EXPECT_EQ(started.value().state, SkillSessionState::Starting);
    EXPECT_EQ(started.value().skill_id, "vision.observe");
    EXPECT_EQ(started.value().session_id, "session-runtime");

    auto ready = manager.MarkReady("session-runtime", "vision.observe", "vision ready", "trace-ready");
    ASSERT_TRUE(ready.ok()) << ready.message();

    SkillObservation observation;
    observation.skill_id = "vision.observe";
    observation.session_id = "session-runtime";
    observation.trace_id = "trace-observation";
    observation.summary = "画面中检测到明显移动";
    observation.confidence = 0.72;
    observation.source = "vlm";
    observation.should_inject_prompt = true;
    auto recorded = manager.RecordObservation(observation);
    ASSERT_TRUE(recorded.ok()) << recorded.message();

    auto current = manager.Get("session-runtime", "vision.observe");
    ASSERT_TRUE(current.ok()) << current.status().message();
    ASSERT_TRUE(current.value().has_value());
    EXPECT_EQ(current.value()->state, SkillSessionState::Running);
    EXPECT_EQ(current.value()->last_observation, "画面中检测到明显移动");
    ASSERT_EQ(current.value()->recent_observations.size(), 1u);

    SkillSessionStopRequest stop;
    stop.skill_id = "vision.observe";
    stop.session_id = "session-runtime";
    stop.trace_id = "trace-stop";
    stop.source = "user";
    stop.reason = "用户关闭视觉观察";
    auto stopped = manager.Stop(stop);
    ASSERT_TRUE(stopped.ok()) << stopped.status().message();
    EXPECT_EQ(stopped.value().state, SkillSessionState::Closing);
    ASSERT_TRUE(manager.CompleteClosing(
        start.session_id,
        start.skill_id,
        started.value().execution_id,
        "closed after async drain",
        "trace-stop").ok());
    current = manager.Get(start.session_id, start.skill_id);
    ASSERT_TRUE(current.ok());
    ASSERT_TRUE(current.value().has_value());
    EXPECT_EQ(current.value()->state, SkillSessionState::Closed);
    EXPECT_EQ(stopped.value().close_reason, "用户关闭视觉观察");
}

TEST(SkillSessionManagerTest, ExpiresStartingSessionThroughMaintenanceTask) {
    SkillSessionOptions options;
    options.startup_timeout = std::chrono::milliseconds(1);
    options.max_duration = std::chrono::seconds(10);
    options.idle_timeout = std::chrono::seconds(10);
    auto manager = std::make_shared<SkillSessionManager>(options);

    SkillSessionStartRequest start;
    start.skill_id = "vision.observe";
    start.session_id = "session-timeout";
    start.trace_id = "trace-timeout";
    start.source = "vector";
    auto started = manager->Start(start);
    ASSERT_TRUE(started.ok()) << started.status().message();

    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    agent::service::gateway::SkillSessionMaintenanceTask task(
        manager,
        std::chrono::milliseconds(10));
    auto tick = task.Tick(std::stop_token{});

    ASSERT_TRUE(tick.ok()) << tick.message();
    auto current = manager->Get("session-timeout", "vision.observe");
    ASSERT_TRUE(current.ok()) << current.status().message();
    ASSERT_TRUE(current.value().has_value());
    EXPECT_EQ(current.value()->state, SkillSessionState::Expired);
    EXPECT_EQ(current.value()->last_error, "startup_timeout");
}

TEST(PersonaRuntimeTest, CompletesSynchronousToolCallFollowUpRoundTrip) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());

    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();
    llm->tool_round_trip = true;
    auto tool_memory = std::make_shared<FakeToolMemoryProvider>();
    tool_memory->prompt_block = "<tool_memory_l4>vision.observe</tool_memory_l4>";
    auto coordinator = std::make_shared<RecordingToolCallCoordinator>();
    PersonaRuntime runtime(
        sessions, memory, emotion, llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr, tool_memory, nullptr, core::LoggerAdapter::ForModule("test"),
        nullptr, nullptr, nullptr, coordinator);

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest request;
    request.session_id = "session-runtime";
    request.user_input = "请观察当前画面";
    request.trace_id = "trace-tool-round-trip";
    ASSERT_TRUE(runtime.SubmitChat(std::move(request),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        }).ok());
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().response, "工具结果后的最终回复");
    EXPECT_EQ(coordinator->execute_count, 1);
    {
        std::lock_guard lock(llm->mutex_);
        EXPECT_EQ(llm->call_count, 2);
        ASSERT_EQ(llm->last_request.messages.size(), 4u);
        EXPECT_EQ(llm->last_request.messages[2].role, agent::llm::ChatRole::Assistant);
        EXPECT_EQ(llm->last_request.messages[3].role, agent::llm::ChatRole::Tool);
        EXPECT_EQ(llm->last_request.messages[3].tool_call_id, "persona-call-1");
    }
    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(PersonaRuntimeTest, PropagatesToolExecutionFailureWithoutFollowUpLlmCall) {
    core::ThreadPool compute({1, 32, "runtime-compute"});
    core::ThreadPool io({1, 32, "runtime-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    SessionManager sessions(compute, io);
    ASSERT_TRUE(sessions.CreateSession(MakeSessionRequest()).ok());
    auto cache = std::make_shared<FakeSemanticCache>();
    cache->lookup_hit = false;
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();
    llm->tool_round_trip = true;
    auto tool_memory = std::make_shared<FakeToolMemoryProvider>();
    auto coordinator = std::make_shared<RecordingToolCallCoordinator>();
    coordinator->status = core::Status::Error(core::ErrorCode::Unavailable, "executor unavailable");
    PersonaRuntime runtime(
        sessions, memory, emotion, llm,
        PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"},
        nullptr, tool_memory, nullptr, core::LoggerAdapter::ForModule("test"),
        nullptr, nullptr, nullptr, coordinator);

    std::promise<core::Result<ChatResponse>> promise;
    auto future = promise.get_future();
    ChatRequest request;
    request.session_id = "session-runtime";
    request.user_input = "请观察当前画面";
    request.trace_id = "trace-tool-failure";
    ASSERT_TRUE(runtime.SubmitChat(std::move(request),
        [&promise](core::Result<ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        }).ok());
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), core::ErrorCode::Unavailable);
    EXPECT_EQ(coordinator->execute_count, 1);
    {
        std::lock_guard lock(llm->mutex_);
        EXPECT_EQ(llm->call_count, 1);
    }
    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SkillSessionManagerTest, ExpiresClosingSessionThatMissesAtomicDrainDeadline) {
    SkillSessionOptions options;
    options.closing_timeout = std::chrono::milliseconds(5);
    SkillSessionManager manager(options);
    SkillSessionStartRequest start;
    start.execution_id = "closing-timeout-execution";
    start.skill_id = "vision.observe";
    start.session_id = "closing-timeout-session";
    auto started = manager.Start(start);
    ASSERT_TRUE(started.ok());
    ASSERT_TRUE(manager.MarkReady(start.session_id, start.skill_id, "ready", "trace").ok());
    ASSERT_TRUE(manager.BeginClosing(
        start.session_id, start.skill_id, start.execution_id, "draining", "trace").ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    EXPECT_EQ(manager.CleanupExpired({}), 1u);
    auto current = manager.Get(start.session_id, start.skill_id);
    ASSERT_TRUE(current.ok());
    ASSERT_TRUE(current.value().has_value());
    EXPECT_EQ(current.value()->state, SkillSessionState::Expired);
}

} // namespace
