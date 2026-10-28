#include "../../src/skill/skill_turn_plan.h"
#include "../../src/service/persona/context_plan.h"
#include "../../src/service/persona/tool_memory_provider.h"
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <future>

namespace {
namespace skill = agent::skill;
namespace persona = agent::service::persona;

skill::SkillManifest Manifest(std::string id) {
    skill::SkillManifest result;
    result.skill_id = id;
    result.tool_name = id;
    result.version = "1";
    result.executor.reference = id;
    result.description = "canonical description";
    return result;
}
TEST(SkillTurnPlanTest, RecallUsesCanonicalManifestAndExcludesStaleVersions) {
    skill::InMemorySkillRegistry registry;
    ASSERT_TRUE(registry.Register(Manifest("lookup")).ok());
    auto plan = skill::BuildSkillTurnPlan(registry, {{"lookup", "1"}, {"missing", "1"}, {"lookup", "old"}}, {}, "q");
    ASSERT_TRUE(plan.ok());
    ASSERT_EQ(plan.value().tools.size(), 1u);
    EXPECT_EQ(plan.value().tools[0].description, "canonical description");
    EXPECT_EQ(plan.value().degraded_reasons.size(), 2u);
}

TEST(ContextPlanTest, KeepsRequiredEvidenceDropsWholeOptionalRecordsAndReportsDuplicates) {
    persona::ContextBudgets budgets;
    budgets[1] = {4, true};
    std::vector<persona::ContextItem> items{
        {persona::ContextSource::L3, "optional", "abcd"},
        {persona::ContextSource::L3, "required", "yes", "{}", true},
        {persona::ContextSource::L3, "required", "yes"}};
    persona::ContextBudgetReport report;
    auto result = persona::CompileContext(items, budgets, report);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(report.dropped_ids, std::vector<std::string>{"optional"});
    EXPECT_EQ(report.duplicate_ids, std::vector<std::string>{"required"});
    EXPECT_NE(result.value().find("unknown"), std::string::npos);
    items[1].content = "exceeds";
    EXPECT_EQ(persona::CompileContext(items, budgets, report).status().code(), core::ErrorCode::ResourceExhausted);
}

TEST(ContextPlanTest, LegacyFactsAndObservationsRemainUntrustedAndPreserveProvenance) {
    std::vector<persona::ContextItem> items{
        {persona::ContextSource::L3, "old-fact", "model used to suggest tea", R"({"date":"2026-10-28"})"},
        {persona::ContextSource::Observation, "observation", "may be tired", R"({"source_role":"tool_observation","uncertainty":0.7})"}};
    persona::ContextBudgetReport report;
    auto result = persona::CompileContext(items, {}, report);
    ASSERT_TRUE(result.ok());
    EXPECT_NE(result.value().find("unknown"), std::string::npos);
    EXPECT_NE(result.value().find("tool_observation"), std::string::npos);
    EXPECT_NE(result.value().find("untrusted_data"), std::string::npos);
    EXPECT_NE(result.value().find("0.7"), std::string::npos);
}

class HangingInvocation final : public skill::ISkillInvocationService {
public:
    core::Result<persona::SkillSessionSnapshot> Invoke(skill::SkillExecutionRequest request,
        skill::SkillExecutionCallbacks callbacks) override {
        callback = std::move(callbacks.on_result);
        persona::SkillSessionSnapshot snapshot;
        snapshot.execution_id = request.call.call_id;
        return snapshot;
    }
    core::Status Cancel(std::string_view) override { ++cancels; return core::Status::Ok(); }
    std::atomic<int> cancels{0};
    std::function<void(skill::SkillResult)> callback;
};

TEST(SkillInvocationCancellationTest, ReusedModelCallIdsAcrossSessionsRemainIndependent) {
    class Executor final : public skill::ISkillExecutor {
    public:
        core::Result<persona::SkillSessionSnapshot> Start(const skill::SkillExecutionRequest& request,
            skill::SkillExecutionCallbacks callbacks) override {
            ids.push_back(request.execution_id);
            results.push_back(std::move(callbacks.on_result));
            return persona::SkillSessionSnapshot{};
        }
        core::Status Cancel(std::string_view id) override { cancelled_id = id; return core::Status::Ok(); }
        std::vector<std::string> ids;
        std::vector<std::function<void(skill::SkillResult)>> results;
        std::string cancelled_id;
    };
    auto registry = std::make_shared<skill::InMemorySkillRegistry>();
    ASSERT_TRUE(registry->Register(Manifest("lookup")).ok());
    auto executor = std::make_shared<Executor>();
    auto factory = std::make_shared<skill::InMemorySkillExecutorFactory>();
    ASSERT_TRUE(factory->Register("native", executor).ok());
    auto sessions = std::make_shared<persona::SkillSessionManager>();
    auto invocation = std::make_shared<skill::SkillInvocationService>(registry, factory, sessions);
    skill::SkillExecutionRequest request;
    request.call = {"same-model-call", "lookup", "lookup", "1", "{}"};
    request.session_id = "session-a";
    int callbacks_a = 0;
    int callbacks_b = 0;
    auto first = invocation->Invoke(request, {[&](auto result) {
        ++callbacks_a;
        EXPECT_EQ(result.status.code(), core::ErrorCode::Cancelled);
    }, {}});
    ASSERT_TRUE(first.ok());
    request.session_id = "session-b";
    auto second = invocation->Invoke(request, {[&](auto result) {
        ++callbacks_b;
        EXPECT_TRUE(result.status.ok());
    }, {}});
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(executor->ids.size(), 2u);
    EXPECT_NE(executor->ids[0], executor->ids[1]);
    ASSERT_TRUE(invocation->Cancel(first.value().execution_id).ok());
    EXPECT_EQ(executor->cancelled_id, executor->ids[0]);
    EXPECT_EQ(callbacks_a, 1);
    EXPECT_EQ(callbacks_b, 0);
    executor->results[0](skill::SkillResult{}); // 取消后的晚到结果不能改写终态。
    EXPECT_EQ(callbacks_a, 1);
    executor->results[1](skill::SkillResult{});
    EXPECT_EQ(callbacks_b, 1);
    EXPECT_EQ(sessions->Get("session-a", "lookup").value()->state, persona::SkillSessionState::Failed);
}

TEST(SkillTurnDeadlineTest, DeadlineCompletesHangingExecutorOnceAndSuppressesLateResult) {
    auto registry = std::make_shared<skill::InMemorySkillRegistry>();
    ASSERT_TRUE(registry->Register(Manifest("lookup")).ok());
    auto invocation = std::make_shared<HangingInvocation>();
    skill::SkillToolCallCoordinator coordinator(registry, invocation);
    core::ThreadPool pool({1, 16, "deadline-test"});
    ASSERT_TRUE(pool.Start().ok());
    boost::asio::io_context io;
    skill::SkillToolCallContext context;
    context.timeout = std::chrono::milliseconds(1);
    context.deadline_scheduler = skill::MakeSkillDeadlineScheduler(io.get_executor(), pool);
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"call", "lookup", "{}"});
    std::promise<core::ErrorCode> done;
    auto future = done.get_future();
    std::atomic<int> callbacks{0};
    ASSERT_TRUE(coordinator.ExecuteAsync(response, context,
        [&](core::Result<std::vector<agent::llm::ChatMessage>> result) {
        if (callbacks.fetch_add(1) == 0) {
            EXPECT_TRUE(result.ok());
            done.set_value(result.ok() ? static_cast<core::ErrorCode>(
                nlohmann::json::parse(result.value().front().content)["error"]["code"].get<int>()) : result.status().code());
        }
    }).ok());
    io.run_for(std::chrono::seconds(1));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), core::ErrorCode::Timeout);
    EXPECT_EQ(invocation->cancels, 1);
    invocation->callback(skill::SkillResult{});
    EXPECT_EQ(callbacks, 1);
    pool.Shutdown(true);
}

TEST(SkillTurnDeadlineTest, ExplicitCancellationCompletesWithoutWaitingForExecutor) {
    auto registry = std::make_shared<skill::InMemorySkillRegistry>();
    ASSERT_TRUE(registry->Register(Manifest("lookup")).ok());
    auto invocation = std::make_shared<HangingInvocation>();
    skill::SkillToolCallCoordinator coordinator(registry, invocation);
    std::stop_source cancellation;
    skill::SkillToolCallContext context;
    context.cancellation = cancellation.get_token();
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"call", "lookup", "{}"});
    int count = 0;
    ASSERT_TRUE(coordinator.ExecuteAsync(response, context, [&](auto result) {
        ++count;
        EXPECT_EQ(result.status().code(), core::ErrorCode::Cancelled);
    }).ok());
    cancellation.request_stop();
    EXPECT_EQ(count, 1);
    EXPECT_EQ(invocation->cancels, 1);
    invocation->callback(skill::SkillResult{});
    EXPECT_EQ(count, 1);
}

TEST(SkillTurnDeadlineTest, CancellationBeforeInvokeReturnsUsesOnlyAcceptedResourceId) {
    class Invocation final : public skill::ISkillInvocationService {
    public:
        explicit Invocation(std::stop_source& source) : source(source) {}
        core::Result<persona::SkillSessionSnapshot> Invoke(skill::SkillExecutionRequest,
            skill::SkillExecutionCallbacks) override {
            source.request_stop();
            persona::SkillSessionSnapshot result;
            result.execution_id = "accepted-resource";
            return result;
        }
        core::Status Cancel(std::string_view id) override { cancelled.emplace_back(id); return core::Status::Ok(); }
        std::stop_source& source;
        std::vector<std::string> cancelled;
    };
    auto registry = std::make_shared<skill::InMemorySkillRegistry>();
    ASSERT_TRUE(registry->Register(Manifest("lookup")).ok());
    std::stop_source cancellation;
    auto invocation = std::make_shared<Invocation>(cancellation);
    skill::SkillToolCallCoordinator coordinator(registry, invocation);
    skill::SkillToolCallContext context;
    context.cancellation = cancellation.get_token();
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"other-execution-id", "lookup", "{}"});
    int callbacks = 0;
    ASSERT_TRUE(coordinator.ExecuteAsync(response, context, [&](auto result) {
        ++callbacks;
        EXPECT_EQ(result.status().code(), core::ErrorCode::Cancelled);
    }).ok());
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(invocation->cancelled, std::vector<std::string>{"accepted-resource"});
}
}
