#include "../../src/service/agent/agent_task_runtime.h"
#include <gtest/gtest.h>
#include <future>
#include <condition_variable>
#include <thread>
#include <algorithm>

namespace agent::runtime {
namespace {
using core::ErrorCode;
using core::Status;
using namespace std::chrono_literals;

// 手动触发时钟使 deadline/晚到竞态可重复，不依赖短 sleep 抢时序。
class ManualDeadline final : public skill::ISkillDeadline {
public:
    void Cancel() noexcept override { cancelled = true; }
    std::atomic<bool> cancelled{false};
};
class ManualScheduler final : public skill::ISkillDeadlineScheduler {
public:
    core::Result<std::shared_ptr<skill::ISkillDeadline>> Schedule(
        std::chrono::steady_clock::time_point, std::function<void()> callback) override {
        auto handle = std::make_shared<ManualDeadline>();
        std::lock_guard lock(mutex);
        timers.emplace_back(handle, std::move(callback));
        return std::shared_ptr<skill::ISkillDeadline>(handle);
    }
    void Fire() {
        std::vector<std::pair<std::shared_ptr<ManualDeadline>, std::function<void()>>> copy;
        { std::lock_guard lock(mutex); copy = timers; }
        for (auto& [handle, callback] : copy) if (!handle->cancelled) callback();
    }
private:
    std::mutex mutex;
    std::vector<std::pair<std::shared_ptr<ManualDeadline>, std::function<void()>>> timers;
};
class Operation final : public llm::IAsyncLlmOperation {
public:
    void Cancel() noexcept override { cancelled = true; }
    std::atomic<bool> cancelled{false};
};
class ScriptedLlm final : public llm::IAsyncLlmClient {
public:
    std::function<llm::ChatCompletionResponse(const llm::ChatCompletionRequest&)> respond;
    bool hang = false;
    std::shared_ptr<Operation> operation = std::make_shared<Operation>();
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<llm::ChatCompletionRequest> requests;
    std::vector<Callback> pending;
    core::Result<std::shared_ptr<llm::IAsyncLlmOperation>> CompleteAsync(
        llm::ChatCompletionRequest request, Callback complete) override {
        {
            std::lock_guard lock(mutex);
            requests.push_back(request);
            if (hang) pending.push_back(complete);
        }
        ready.notify_all();
        if (!hang) complete(respond(request));
        return std::shared_ptr<llm::IAsyncLlmOperation>(operation);
    }
    bool WaitRequests(std::size_t count) {
        std::unique_lock lock(mutex);
        return ready.wait_for(lock, 3s, [&] { return requests.size() >= count; });
    }
    void Release() {
        std::vector<Callback> callbacks;
        { std::lock_guard lock(mutex); callbacks.swap(pending); }
        for (auto& callback : callbacks) callback(Status::Error(ErrorCode::Cancelled, "provider drained"));
    }
};
llm::ChatCompletionResponse Finish(std::string summary = "done") {
    llm::ChatCompletionResponse result;
    result.tool_calls.push_back({"finish", "complete_task", "{\"summary\":\"" + summary + "\"}"});
    result.total_tokens = 10;
    return result;
}
class Lookup final : public skill::ISkillExecutor {
public:
    std::atomic<int> calls{0};
    core::Result<service::persona::SkillSessionSnapshot> Start(
        const skill::SkillExecutionRequest& request, skill::SkillExecutionCallbacks callbacks) override {
        ++calls;
        EXPECT_EQ(request.session_id, "business-session");
        EXPECT_EQ(request.trace_id, "parent-turn");
        skill::SkillResult result;
        result.result_json = R"({"fact":"evidence"})";
        callbacks.on_result(std::move(result));
        return service::persona::SkillSessionSnapshot{};
    }
    Status Cancel(std::string_view) override { return Status::Ok(); }
};

class AgentTaskRuntimeTest : public testing::Test {
protected:
    std::shared_ptr<core::ThreadPool> pool;
    std::shared_ptr<ScriptedLlm> llm = std::make_shared<ScriptedLlm>();
    std::shared_ptr<ManualScheduler> deadlines = std::make_shared<ManualScheduler>();
    std::shared_ptr<skill::InMemorySkillRegistry> registry = std::make_shared<skill::InMemorySkillRegistry>();
    std::shared_ptr<skill::InMemorySkillExecutorFactory> factory = std::make_shared<skill::InMemorySkillExecutorFactory>();
    std::shared_ptr<Lookup> lookup = std::make_shared<Lookup>();
    std::shared_ptr<AgentTaskRuntime> runtime;
    void SetUp() override {
        core::ThreadPoolOptions pool_options;
        pool_options.worker_count = 1;
        pool = std::make_shared<core::ThreadPool>(pool_options);
        ASSERT_TRUE(pool->Start().ok());
        skill::SkillManifest manifest;
        manifest.skill_id = "lookup"; manifest.tool_name = "lookup"; manifest.version = "1";
        manifest.description = "Find evidence";
        manifest.executor.reference = "lookup";
        ASSERT_TRUE(registry->Register(manifest).ok());
        ASSERT_TRUE(factory->Register("native", lookup).ok());
        llm->respond = [](const auto&) { return Finish(); };
        MakeRuntime();
    }
    void MakeRuntime(AgentTaskOptions options = {}) {
        options.model = "fixture";
        runtime = std::make_shared<AgentTaskRuntime>(llm, registry, factory, deadlines, pool, options);
    }
    AgentTaskRequest Request() {
        AgentTaskRequest request;
        request.task = "Find evidence and summarize";
        request.context.session_id = "business-session";
        request.context.trace_id = "parent-turn";
        request.context.user_uuid = "owner";
        request.allowed_skill_ids = {"lookup"};
        return request;
    }
    bool Drain() {
        const auto until = std::chrono::steady_clock::now() + 3s;
        while (runtime->ActiveTasks() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        return runtime->ActiveTasks() == 0;
    }
    void TearDown() override {
        if (runtime) runtime->Close();
        llm->Release();
        if (runtime) EXPECT_TRUE(Drain());
        pool->Shutdown(true);
    }
};

TEST_F(AgentTaskRuntimeTest, MultipleToolRoundsPreserveBusinessIdentityAndPrivateTranscript) {
    llm->respond = [](const auto& request) {
        const auto count = std::count_if(request.messages.begin(), request.messages.end(),
            [](const auto& message) { return message.role == llm::ChatRole::Tool; });
        if (count == 2) return Finish("evidence summary");
        llm::ChatCompletionResponse result;
        result.tool_calls.push_back({"lookup-" + std::to_string(count), "lookup", "{}"});
        result.total_tokens = 10;
        return result;
    };
    std::promise<core::Result<AgentTaskResult>> completed;
    auto future = completed.get_future();
    auto lease = runtime->Start(Request(), [&](auto value) { completed.set_value(std::move(value)); });
    ASSERT_TRUE(lease.ok());
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().model_requests, 3U);
    EXPECT_EQ(result.value().tool_calls, 2U);
    EXPECT_EQ(result.value().total_tokens, 30U);
    EXPECT_EQ(lookup->calls, 2);
}

TEST_F(AgentTaskRuntimeTest, TimeoutCompletesOnceButRetainsAdmissionUntilProviderDrains) {
    AgentTaskOptions options; options.max_active_tasks = 1; MakeRuntime(options);
    llm->hang = true;
    std::atomic<int> completions{0};
    std::promise<ErrorCode> completed; auto future = completed.get_future();
    auto lease = runtime->Start(Request(), [&](auto result) {
        if (++completions == 1) completed.set_value(result.status().code());
    });
    ASSERT_TRUE(lease.ok()); ASSERT_TRUE(llm->WaitRequests(1));
    deadlines->Fire();
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::Timeout);
    EXPECT_TRUE(llm->operation->cancelled);
    EXPECT_EQ(runtime->ActiveTasks(), 1U);
    auto rejected = runtime->Start(Request(), [](auto) { FAIL() << "rejected task completed"; });
    EXPECT_EQ(rejected.status().code(), ErrorCode::ResourceExhausted);
    llm->Release(); EXPECT_TRUE(Drain()); EXPECT_EQ(completions, 1);
}

TEST_F(AgentTaskRuntimeTest, LeaseReleaseCancelsAndCloseRejectsNewWork) {
    llm->hang = true;
    std::promise<ErrorCode> completed; auto future = completed.get_future();
    {
        auto lease = runtime->Start(Request(), [&](auto result) { completed.set_value(result.status().code()); });
        ASSERT_TRUE(lease.ok()); ASSERT_TRUE(llm->WaitRequests(1));
    }
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::Cancelled);
    runtime->Close();
    EXPECT_EQ(runtime->Start(Request(), [](auto) {}).status().code(), ErrorCode::Unavailable);
}

TEST_F(AgentTaskRuntimeTest, ParentCancellationPropagates) {
    llm->hang = true;
    std::stop_source parent;
    auto request = Request(); request.context.cancellation = parent.get_token();
    std::promise<ErrorCode> completed; auto future = completed.get_future();
    auto lease = runtime->Start(std::move(request), [&](auto result) { completed.set_value(result.status().code()); });
    ASSERT_TRUE(lease.ok()); ASSERT_TRUE(llm->WaitRequests(1));
    parent.request_stop();
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::Cancelled);
}

TEST_F(AgentTaskRuntimeTest, ModelBudgetStopsRepeatedTools) {
    AgentTaskOptions options; options.max_model_requests = 2; MakeRuntime(options);
    llm->respond = [](const auto&) { llm::ChatCompletionResponse result;
        result.tool_calls.push_back({"repeat", "lookup", "{}"}); return result; };
    std::promise<ErrorCode> completed; auto future = completed.get_future();
    auto lease = runtime->Start(Request(), [&](auto result) { completed.set_value(result.status().code()); });
    ASSERT_TRUE(lease.ok()); ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::ResourceExhausted);
    EXPECT_EQ(lookup->calls, 2);
}

TEST_F(AgentTaskRuntimeTest, CompletionCannotBeMixedWithSideEffects) {
    llm->respond = [](const auto&) { auto result = Finish(); result.tool_calls.push_back({"extra", "lookup", "{}"}); return result; };
    std::promise<ErrorCode> completed; auto future = completed.get_future();
    auto lease = runtime->Start(Request(), [&](auto result) { completed.set_value(result.status().code()); });
    ASSERT_TRUE(lease.ok()); ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::InvalidArgument); EXPECT_EQ(lookup->calls, 0);
}

TEST_F(AgentTaskRuntimeTest, ScopedCatalogDoesNotExposeUnselectedTools) {
    auto request = Request(); request.allowed_skill_ids.clear();
    llm->respond = [](const auto& input) {
        EXPECT_EQ(input.tools.size(), 1U);
        EXPECT_EQ(input.tools.front().name, "complete_task");
        return Finish();
    };
    std::promise<core::Result<AgentTaskResult>> completed; auto future = completed.get_future();
    auto lease = runtime->Start(std::move(request), [&](auto result) { completed.set_value(std::move(result)); });
    ASSERT_TRUE(lease.ok()); ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(future.get().ok()); EXPECT_EQ(lookup->calls, 0);
}

TEST_F(AgentTaskRuntimeTest, InvalidCompletionAndMissingCompletionFailExplicitly) {
    for (const auto& arguments : {std::string("{}"), std::string(R"({"summary":3})")}) {
        llm->respond = [arguments](const auto&) {
            auto response = Finish(); response.tool_calls.front().arguments_json = arguments; return response;
        };
        std::promise<ErrorCode> done; auto future = done.get_future();
        auto lease = runtime->Start(Request(), [&](auto result) { done.set_value(result.status().code()); });
        ASSERT_TRUE(lease.ok()); ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
        EXPECT_EQ(future.get(), ErrorCode::InvalidArgument); EXPECT_TRUE(Drain());
    }
    llm->respond = [](const auto&) { llm::ChatCompletionResponse result; result.content = "finished"; return result; };
    std::promise<ErrorCode> done; auto future = done.get_future();
    auto lease = runtime->Start(Request(), [&](auto result) { done.set_value(result.status().code()); });
    ASSERT_TRUE(lease.ok()); ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::FailedPrecondition);
}

TEST_F(AgentTaskRuntimeTest, RequiredToolCannotBeBypassedByCompletion) {
    class Policy final : public skill::ISkillToolPolicy {
    public:
        Status Authorize(const skill::SkillToolCallContext&, const skill::SkillCall&, const skill::SkillManifest&) const override { return Status::Ok(); }
        Status ValidateResult(const skill::SkillToolCallContext&, const skill::SkillCall&, const skill::SkillManifest&, const skill::SkillResult&) const override { return Status::Ok(); }
        core::Result<std::vector<skill::SkillToolRequirement>> RequiredTools(const skill::SkillToolCallContext&) const override {
            return std::vector<skill::SkillToolRequirement>{{"lookup", "1"}};
        }
    };
    auto request = Request(); request.context.policy = std::make_shared<Policy>();
    std::promise<ErrorCode> done; auto future = done.get_future();
    auto lease = runtime->Start(std::move(request), [&](auto result) { done.set_value(result.status().code()); });
    ASSERT_TRUE(lease.ok()); ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::FailedPrecondition);
}

TEST_F(AgentTaskRuntimeTest, SameBusinessSessionHasIndependentConcurrentToolExecutions) {
    class HoldingLookup final : public skill::ISkillExecutor {
    public:
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<skill::SkillExecutionCallbacks> pending;
        std::vector<std::string> execution_ids;
        core::Result<service::persona::SkillSessionSnapshot> Start(
            const skill::SkillExecutionRequest& request, skill::SkillExecutionCallbacks callbacks) override {
            { std::lock_guard lock(mutex); pending.push_back(std::move(callbacks)); execution_ids.push_back(request.execution_id); }
            ready.notify_all(); return service::persona::SkillSessionSnapshot{};
        }
        Status Cancel(std::string_view) override { return Status::Ok(); }
    };
    auto holding = std::make_shared<HoldingLookup>();
    factory = std::make_shared<skill::InMemorySkillExecutorFactory>();
    ASSERT_TRUE(factory->Register("native", holding).ok()); MakeRuntime();
    llm->respond = [](const auto& request) {
        if (request.messages.back().role == llm::ChatRole::Tool) return Finish();
        llm::ChatCompletionResponse response;
        response.tool_calls.push_back({"same-model-id", "lookup", "{}"}); return response;
    };
    std::promise<bool> first, second; auto a = first.get_future(); auto b = second.get_future();
    auto lease_a = runtime->Start(Request(), [&](auto result) { first.set_value(result.ok()); });
    auto lease_b = runtime->Start(Request(), [&](auto result) { second.set_value(result.ok()); });
    ASSERT_TRUE(lease_a.ok()); ASSERT_TRUE(lease_b.ok());
    std::vector<skill::SkillExecutionCallbacks> pending;
    {
        std::unique_lock lock(holding->mutex);
        ASSERT_TRUE(holding->ready.wait_for(lock, 3s, [&] { return holding->pending.size() == 2; }));
        EXPECT_NE(holding->execution_ids[0], holding->execution_ids[1]);
        pending.swap(holding->pending);
    }
    for (auto& callback : pending) { skill::SkillResult result; result.result_json = "{}"; callback.on_result(std::move(result)); }
    ASSERT_EQ(a.wait_for(3s), std::future_status::ready); ASSERT_EQ(b.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(a.get()); EXPECT_TRUE(b.get());
}

TEST_F(AgentTaskRuntimeTest, SingleWorkerBoundedLoadReleasesAllLeases) {
    const auto began = std::chrono::steady_clock::now();
    // 固定八个在途槽位，共 256 次任务；检查队列恢复与完整回收，不模拟云模型吞吐。
    for (int batch = 0; batch < 32; ++batch) {
        std::vector<std::shared_ptr<IAgentTaskLease>> leases;
        std::vector<std::future<bool>> futures;
        for (int index = 0; index < 8; ++index) {
            auto done = std::make_shared<std::promise<bool>>(); futures.push_back(done->get_future());
            auto lease = runtime->Start(Request(), [done](auto result) { done->set_value(result.ok()); });
            ASSERT_TRUE(lease.ok()) << lease.status().message(); leases.push_back(lease.value());
        }
        for (auto& future : futures) { ASSERT_EQ(future.wait_for(3s), std::future_status::ready); EXPECT_TRUE(future.get()); }
        ASSERT_TRUE(Drain());
    }
    RecordProperty("tasks", 256);
    RecordProperty("elapsed_us", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - began).count()));
}

class Binding final : public ISubagentContextProvider {
public:
    core::Result<AgentTaskRequest> Prepare(const skill::SkillExecutionRequest&) const override {
        AgentTaskRequest request;
        request.allowed_skill_ids = {"lookup"};
        request.trusted_context = "Use the current approved evidence snapshot.";
        return request;
    }
};
TEST_F(AgentTaskRuntimeTest, SharedFactoryDoesNotKeepRuntimeAlive) {
    auto executor = std::make_shared<SubagentSkillExecutor>(runtime, std::make_shared<Binding>());
    ASSERT_TRUE(factory->RegisterReference("native", "agent.spawn", executor).ok());
    std::weak_ptr<AgentTaskRuntime> weak = runtime;
    runtime.reset();
    EXPECT_TRUE(weak.expired());
    skill::SkillExecutionRequest request;
    request.execution_id = "closed-runtime";
    auto started = executor->Start(request, {.on_result = [](auto) { FAIL() << "rejected task completed"; }});
    EXPECT_FALSE(started.ok());
}
TEST_F(AgentTaskRuntimeTest, ParentSkillCoordinatorDelegatesAndReceivesValidatedToolResult) {
    auto parent_registry = std::make_shared<skill::InMemorySkillRegistry>();
    ASSERT_TRUE(parent_registry->Register(SubagentSkillManifest()).ok());
    auto parent_factory = std::make_shared<skill::InMemorySkillExecutorFactory>();
    auto executor = std::make_shared<SubagentSkillExecutor>(runtime, std::make_shared<Binding>());
    ASSERT_TRUE(parent_factory->RegisterReference("native", "agent.spawn", executor).ok());
    auto invocation = std::make_shared<skill::SkillInvocationService>(parent_registry, parent_factory,
        std::make_shared<service::persona::SkillSessionManager>());
    skill::SkillToolCallCoordinator parent(parent_registry, invocation);
    auto context = Request().context;
    context.allowed_tool_names = std::vector<std::string>{"spawn_agent"};
    context.deadline_scheduler = deadlines;
    llm::ChatCompletionResponse calls;
    calls.tool_calls.push_back({"spawn-1", "spawn_agent", R"({"task":"summarize evidence","timeout_ms":10000})"});
    std::promise<core::Result<std::vector<llm::ChatMessage>>> completed;
    auto future = completed.get_future();
    ASSERT_TRUE(parent.ExecuteAsync(calls, context, [&](auto result) { completed.set_value(std::move(result)); }).ok());
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    auto result = future.get(); ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value().front().tool_call_id, "spawn-1");
    EXPECT_NE(result.value().front().content.find("done"), std::string::npos);
}
}
}
