#include "../../src/skill/skill_registry.h"
#include "../../src/skill/skill_executor.h"
#include "../../src/service/persona/stateful_skill_registry.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <future>

namespace {

class RegisteredStatefulSkill final : public agent::service::persona::IStatefulSkillExecution {
public:
    explicit RegisteredStatefulSkill(
        agent::service::persona::StatefulSkillExecutionContext context)
        : context_(std::move(context)) {}

    core::Status Start() override {
        return context_.sessions->MarkReady(
            context_.start_request.session_id,
            context_.start_request.skill_id,
            "registered skill ready",
            context_.start_request.trace_id);
    }

    core::Status Stop(const agent::service::persona::SkillSessionStopRequest& request) override {
        auto status = context_.sessions->BeginClosing(
            request.session_id, request.skill_id,
            context_.start_request.execution_id,
            request.reason, request.trace_id);
        if (!status.ok()) return status;
        return context_.sessions->CompleteClosing(
            request.session_id, request.skill_id,
            context_.start_request.execution_id,
            "registered skill closed", request.trace_id);
    }

private:
    agent::service::persona::StatefulSkillExecutionContext context_;
};

REGISTER_STATEFUL_SKILL(RegisteredStatefulSkill, "test.stateful", "1.0.0");

agent::skill::SkillManifest MakeManifest() {
    agent::skill::SkillManifest manifest;
    manifest.skill_id = "vision.observe";
    manifest.version = "1.0.0";
    manifest.input_schema_json = R"({"type":"object","required":["reason"]})";
    manifest.executor.reference = manifest.skill_id;
    return manifest;
}

TEST(SkillManifestTest, RejectsInvalidSchema) {
    auto manifest = MakeManifest();
    manifest.input_schema_json = "{";
    EXPECT_EQ(agent::skill::ValidateManifest(manifest).code(),
              core::ErrorCode::InvalidArgument);
}

TEST(SkillRegistryTest, RegistersAndFindsVersionedManifest) {
    agent::skill::InMemorySkillRegistry registry;
    ASSERT_TRUE(registry.Register(MakeManifest()).ok());
    auto found = registry.Require("vision.observe", "1.0.0");
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value().skill_id, "vision.observe");
}

TEST(SkillRegistryTest, RejectsDuplicateManifest) {
    agent::skill::InMemorySkillRegistry registry;
    ASSERT_TRUE(registry.Register(MakeManifest()).ok());
    EXPECT_EQ(registry.Register(MakeManifest()).code(), core::ErrorCode::AlreadyExists);
}

TEST(SkillRegistryTest, RegistersJsonManifest) {
    agent::skill::InMemorySkillRegistry registry;
    ASSERT_TRUE(registry.RegisterJson(R"({"skill_id":"crm.lookup","version":"1.0.0","executor":{"type":"native","reference":"crm.lookup"}})").ok());
    EXPECT_TRUE(registry.Require("crm.lookup", "1.0.0").ok());
}

TEST(SkillRegistryTest, ParsesL4SeedFieldsFromJson) {
    agent::skill::InMemorySkillRegistry registry;
    ASSERT_TRUE(registry.RegisterJson(
        R"({"skill_id":"vision.observe","version":"1.0.0",)"
        R"("l4_payload":"用户请求观察画面时使用该工具。","intent":"camera",)"
        R"("negative_keywords":["不用看","别看"],)"
        R"("executor":{"type":"native","reference":"vision.observe"}})").ok());
    auto manifest = registry.Require("vision.observe", "1.0.0");
    ASSERT_TRUE(manifest.ok());
    EXPECT_EQ(manifest.value().l4_payload, "用户请求观察画面时使用该工具。");
    EXPECT_EQ(manifest.value().intent, "camera");
    ASSERT_EQ(manifest.value().negative_keywords.size(), 2u);
    EXPECT_EQ(manifest.value().negative_keywords[0], "不用看");
    EXPECT_EQ(manifest.value().negative_keywords[1], "别看");
}

TEST(SkillRegistryTest, RegistersAllManifestsAtomicallyForStartupValidation) {
    agent::skill::InMemorySkillRegistry registry;
    auto first = MakeManifest();
    auto second = MakeManifest();
    second.skill_id = "document.analyze";
    second.executor.reference = second.skill_id;
    ASSERT_TRUE(registry.RegisterAll({first, second}).ok());
    EXPECT_TRUE(registry.Require("vision.observe").ok());
    EXPECT_TRUE(registry.Require("document.analyze").ok());
}

TEST(StatefulSkillRegistryTest, MacroRegistersFactoryAndRouterOwnsExecutionLifecycle) {
    auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
    agent::service::persona::StatefulSkillExecutionRouter router(sessions);
    agent::service::persona::SkillSessionStartRequest start;
    start.skill_id = "test.stateful";
    start.session_id = "stateful-session";
    start.user_uuid = "stateful-user";
    start.trace_id = "stateful-trace";
    start.reason = "test";

    auto started = router.Start(start, "1.0.0");
    ASSERT_TRUE(started.ok()) << started.status().message();
    EXPECT_EQ(started.value().state,
              agent::service::persona::SkillSessionState::Ready);

    auto repeated = router.Start(start, "1.0.0");
    ASSERT_TRUE(repeated.ok()) << repeated.status().message();
    EXPECT_EQ(repeated.value().execution_id, started.value().execution_id);

    agent::service::persona::SkillSessionStopRequest stop;
    stop.skill_id = start.skill_id;
    stop.session_id = start.session_id;
    stop.authenticated_user_uuid = start.user_uuid;
    stop.execution_id = started.value().execution_id;
    stop.trace_id = start.trace_id;
    stop.reason = "test complete";
    auto stopped = router.Stop(stop);
    ASSERT_TRUE(stopped.ok()) << stopped.status().message();
    EXPECT_EQ(stopped.value().state,
              agent::service::persona::SkillSessionState::Closed);
}

TEST(SkillExecutorFactoryTest, ResolvesRegisteredExecutorType) {
    class FakeExecutor final : public agent::skill::ISkillExecutor {
    public:
        core::Result<agent::service::persona::SkillSessionSnapshot> Start(
            const agent::skill::SkillExecutionRequest&, agent::skill::SkillExecutionCallbacks) override {
            return core::Status::Error(core::ErrorCode::Unavailable, "fake");
        }
        core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
    };
    agent::skill::InMemorySkillExecutorFactory factory;
    auto executor = std::make_shared<FakeExecutor>();
    ASSERT_TRUE(factory.Register("native", executor).ok());
    auto manifest = MakeManifest();
    auto resolved = factory.Resolve(manifest);
    ASSERT_TRUE(resolved.ok()) << resolved.status().message();
    EXPECT_EQ(resolved.value(), executor);
}

TEST(SkillInvocationServiceTest, RoutesObservationAndResultThroughSessionLifecycle) {
    class CompletingExecutor final : public agent::skill::ISkillExecutor {
    public:
        core::Result<agent::service::persona::SkillSessionSnapshot> Start(
            const agent::skill::SkillExecutionRequest& request,
            agent::skill::SkillExecutionCallbacks callbacks) override {
            if (callbacks.on_observation) {
                agent::service::persona::SkillObservation observation;
                observation.session_id = request.session_id;
                observation.skill_id = request.call.skill_id;
                observation.summary = "completed";
                callbacks.on_observation(std::move(observation));
            }
            if (callbacks.on_result) {
                agent::skill::SkillResult result;
                result.call_id = request.call.call_id;
                result.result_json = "{\"ok\":true}";
                callbacks.on_result(std::move(result));
            }
            return agent::service::persona::SkillSessionSnapshot{};
        }
        core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
    };
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = MakeManifest();
    ASSERT_TRUE(registry->Register(manifest).ok());
    auto factory = std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
    ASSERT_TRUE(factory->RegisterReference("native", manifest.skill_id,
                                           std::make_shared<CompletingExecutor>()).ok());
    auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
    agent::skill::SkillInvocationService service(registry, factory, sessions);
    agent::skill::SkillExecutionRequest request;
    request.call = {"call-1", {}, manifest.skill_id, manifest.version, "{}"};
    request.session_id = "session-1";
    auto invoked = service.Invoke(std::move(request), {});
    ASSERT_TRUE(invoked.ok()) << invoked.status().message();
    auto snapshot = sessions->Get("session-1", manifest.skill_id);
    ASSERT_TRUE(snapshot.ok());
    ASSERT_TRUE(snapshot.value().has_value());
    EXPECT_EQ(snapshot.value()->state, agent::service::persona::SkillSessionState::Closed);
}

TEST(SkillToolCallCoordinatorTest, ExecutesStandardToolCallAndBuildsToolResult) {
    class ImmediateExecutor final : public agent::skill::ISkillExecutor {
    public:
        core::Result<agent::service::persona::SkillSessionSnapshot> Start(
            const agent::skill::SkillExecutionRequest& request,
            agent::skill::SkillExecutionCallbacks callbacks) override {
            agent::skill::SkillResult result;
            result.call_id = request.call.call_id;
            result.result_json = R"({"value":42})";
            callbacks.on_result(std::move(result));
            return agent::service::persona::SkillSessionSnapshot{};
        }
        core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
    };
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = MakeManifest();
    ASSERT_TRUE(registry->Register(manifest).ok());
    auto factory = std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
    ASSERT_TRUE(factory->RegisterReference("native", manifest.skill_id,
                                           std::make_shared<ImmediateExecutor>()).ok());
    auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
    auto invocation = std::make_shared<agent::skill::SkillInvocationService>(registry, factory, sessions);
    agent::skill::SkillToolCallCoordinator coordinator(registry, invocation);
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"call-1", manifest.skill_id, R"({"reason":"look"})"});
    auto result = coordinator.Execute(response, {"session-1", "user-1", "persona-1", "trace-1", std::chrono::seconds(1)});
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].role, agent::llm::ChatRole::Tool);
    EXPECT_EQ(result.value()[0].tool_call_id, "call-1");
    EXPECT_EQ(result.value()[0].content, R"({"value":42})");
}

TEST(SkillToolCallCoordinatorTest, RejectsToolOutsideTurnAllowlistWithoutExecutingIt) {
    class CountingExecutor final : public agent::skill::ISkillExecutor {
    public:
        core::Result<agent::service::persona::SkillSessionSnapshot> Start(
            const agent::skill::SkillExecutionRequest&,
            agent::skill::SkillExecutionCallbacks) override {
            ++calls;
            return agent::service::persona::SkillSessionSnapshot{};
        }
        core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
        int calls = 0;
    };
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = MakeManifest();
    ASSERT_TRUE(registry->Register(manifest).ok());
    auto executor = std::make_shared<CountingExecutor>();
    auto factory = std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
    ASSERT_TRUE(factory->RegisterReference("native", manifest.skill_id, executor).ok());
    auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
    auto invocation = std::make_shared<agent::skill::SkillInvocationService>(registry, factory, sessions);
    agent::skill::SkillToolCallCoordinator coordinator(registry, invocation);
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"call-1", registry->Require(manifest.skill_id).value().tool_name,
                                   R"({"reason":"look"})"});
    agent::skill::SkillToolCallContext context{
        "session-allowlist", "user", "persona", "trace", std::chrono::seconds(1),
        std::vector<std::string>{"some_other_tool"}};
    auto result = coordinator.Execute(response, context);
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(nlohmann::json::parse(result.value().front().content)["error"]["code"],
              static_cast<int>(core::ErrorCode::PermissionDenied));
    EXPECT_EQ(executor->calls, 0);
}

TEST(SkillToolCallCoordinatorTest, CompletesTwoRoundToolCallingProtocol) {
    class ImmediateExecutor final : public agent::skill::ISkillExecutor {
    public:
        core::Result<agent::service::persona::SkillSessionSnapshot> Start(
            const agent::skill::SkillExecutionRequest& request,
            agent::skill::SkillExecutionCallbacks callbacks) override {
            agent::skill::SkillResult result;
            result.call_id = request.call.call_id;
            result.result_json = R"({"answer":"observed"})";
            callbacks.on_result(std::move(result));
            return agent::service::persona::SkillSessionSnapshot{};
        }
        core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
    };
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = MakeManifest();
    ASSERT_TRUE(registry->Register(manifest).ok());
    auto factory = std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
    ASSERT_TRUE(factory->RegisterReference("native", manifest.skill_id,
                                           std::make_shared<ImmediateExecutor>()).ok());
    auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
    auto invocation = std::make_shared<agent::skill::SkillInvocationService>(registry, factory, sessions);
    agent::skill::SkillToolCallCoordinator coordinator(registry, invocation);

    agent::llm::ChatCompletionRequest request;
    request.tools.push_back({manifest.skill_id, "observe", manifest.input_schema_json});
    request.tool_choice = "auto";
    request.messages.push_back({agent::llm::ChatRole::User, "look"});
    agent::llm::ChatCompletionResponse first;
    first.tool_calls.push_back({"call-1", manifest.skill_id, R"({"reason":"look"})"});
    auto tool_messages = coordinator.Execute(first, {"session-2", "user-1", "persona-1", "trace-2", std::chrono::seconds(1)});
    ASSERT_TRUE(tool_messages.ok()) << tool_messages.status().message();
    ASSERT_EQ(tool_messages.value().size(), 1u);

    agent::llm::ChatMessage assistant;
    assistant.role = agent::llm::ChatRole::Assistant;
    assistant.tool_calls = first.tool_calls;
    request.messages.push_back(std::move(assistant));
    request.messages.push_back(tool_messages.value().front());
    request.messages.push_back({agent::llm::ChatRole::Assistant, "observed"});
    EXPECT_TRUE(agent::llm::ValidateChatCompletionRequest(request).ok());
    EXPECT_EQ(request.messages.back().content, "observed");
}

TEST(SkillToolCallCoordinatorTest, ConvertsExecutionTimeoutToToolErrorAndCancels) {
    class BlockingExecutor final : public agent::skill::ISkillExecutor {
    public:
        core::Result<agent::service::persona::SkillSessionSnapshot> Start(
            const agent::skill::SkillExecutionRequest&,
            agent::skill::SkillExecutionCallbacks) override {
            return agent::service::persona::SkillSessionSnapshot{};
        }
        core::Status Cancel(std::string_view) override {
            ++cancel_count;
            return core::Status::Ok();
        }
        int cancel_count = 0;
    };
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = MakeManifest();
    ASSERT_TRUE(registry->Register(manifest).ok());
    auto factory = std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
    auto executor = std::make_shared<BlockingExecutor>();
    ASSERT_TRUE(factory->RegisterReference("native", manifest.skill_id, executor).ok());
    auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
    auto invocation = std::make_shared<agent::skill::SkillInvocationService>(registry, factory, sessions);
    agent::skill::SkillToolCallCoordinator coordinator(registry, invocation);
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"timeout-call", manifest.skill_id, R"({"reason":"wait"})"});
    auto result = coordinator.Execute(response, {"session-timeout", "user", "persona", "trace", std::chrono::milliseconds(1)});
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].role, agent::llm::ChatRole::Tool);
    EXPECT_NE(result.value()[0].content.find("timed out"), std::string::npos);
    EXPECT_EQ(executor->cancel_count, 1);
}

TEST(SkillToolCallCoordinatorTest, ExecutesAsyncWithoutBlockingUntilExecutorCompletes) {
    class DeferredExecutor final : public agent::skill::ISkillExecutor {
    public:
        core::Result<agent::service::persona::SkillSessionSnapshot> Start(
            const agent::skill::SkillExecutionRequest& request,
            agent::skill::SkillExecutionCallbacks callbacks) override {
            request_ = request;
            callbacks_ = std::move(callbacks);
            return agent::service::persona::SkillSessionSnapshot{};
        }
        core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
        void Complete() {
            agent::skill::SkillResult result;
            result.call_id = request_.call.call_id;
            result.result_json = R"({"async":true})";
            callbacks_.on_result(std::move(result));
        }

    private:
        agent::skill::SkillExecutionRequest request_;
        agent::skill::SkillExecutionCallbacks callbacks_;
    };
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = MakeManifest();
    ASSERT_TRUE(registry->Register(manifest).ok());
    auto factory = std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
    auto executor = std::make_shared<DeferredExecutor>();
    ASSERT_TRUE(factory->RegisterReference("native", manifest.skill_id, executor).ok());
    auto sessions = std::make_shared<agent::service::persona::SkillSessionManager>();
    auto invocation = std::make_shared<agent::skill::SkillInvocationService>(registry, factory, sessions);
    agent::skill::SkillToolCallCoordinator coordinator(registry, invocation);

    std::promise<core::Result<std::vector<agent::llm::ChatMessage>>> promise;
    auto future = promise.get_future();
    agent::llm::ChatCompletionResponse response;
    response.tool_calls.push_back({"async-call", manifest.skill_id, R"({"reason":"look"})"});
    ASSERT_TRUE(coordinator.ExecuteAsync(
        response,
        {"async-session", "user", "persona", "trace", std::chrono::seconds(1)},
        [&promise](auto result) mutable { promise.set_value(std::move(result)); }).ok());
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    executor->Complete();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].role, agent::llm::ChatRole::Tool);
    EXPECT_EQ(result.value()[0].content, R"({"async":true})");
}

// 同步/异步使用同一业务替身，验证授权、输出 schema 与来源门禁的公共契约。
class ContractInvocation final : public agent::skill::ISkillInvocationService {
public:
    core::Result<agent::service::persona::SkillSessionSnapshot> Invoke(
        agent::skill::SkillExecutionRequest request, agent::skill::SkillExecutionCallbacks callbacks) override {
        ++calls;
        if (deferred) {
            pending = std::move(callbacks.on_result);
        } else {
            result.call_id = spoof_identity ? "wrong-call" : request.call.call_id;
            callbacks.on_result(result);
        }
        agent::service::persona::SkillSessionSnapshot snapshot;
        snapshot.execution_id = request.call.call_id;
        return snapshot;
    }
    core::Status Cancel(std::string_view) override {
        ++cancels;
        if (pending) {
            auto completed = std::move(pending);
            auto cancelled = result;
            cancelled.status = core::Status::Error(core::ErrorCode::Cancelled, "cancelled");
            completed(std::move(cancelled));
        }
        return core::Status::Ok();
    }
    agent::skill::SkillResult result;
    std::function<void(agent::skill::SkillResult)> pending;
    int calls = 0;
    int cancels = 0;
    bool deferred = false;
    bool spoof_identity = false;
};

class ContractPolicy final : public agent::skill::ISkillToolPolicy {
public:
    core::Status Authorize(const agent::skill::SkillToolCallContext& context,
        const agent::skill::SkillCall&, const agent::skill::SkillManifest& manifest) const override {
        EXPECT_EQ(context.user_uuid, "trusted-user");
        EXPECT_EQ(manifest.version, "1.0.0");
        return authorize;
    }
    core::Status ValidateResult(const agent::skill::SkillToolCallContext&,
        const agent::skill::SkillCall&, const agent::skill::SkillManifest&,
        const agent::skill::SkillResult&) const override { return validate; }
    core::Status authorize = core::Status::Ok();
    core::Status validate = core::Status::Ok();
};

class SkillCoordinatorContractTest : public ::testing::TestWithParam<bool> {};

TEST_P(SkillCoordinatorContractTest, SharesAuthorizationAndResultValidationAcrossExecutionPaths) {
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    auto manifest = MakeManifest();
    manifest.output_schema_json = R"({"type":"object","properties":{"value":{"type":"integer"}},"required":["value"],"additionalProperties":false})";
    ASSERT_TRUE(registry->Register(manifest).ok());
    auto invocation = std::make_shared<ContractInvocation>();
    auto policy = std::make_shared<ContractPolicy>();
    agent::skill::SkillToolCallCoordinator coordinator(registry, invocation);
    agent::llm::ChatCompletionResponse response;
    response.tool_calls = {{"contract-call", "vision_observe", R"({"reason":"look"})"}};
    agent::skill::SkillToolCallContext context;
    context.user_uuid = "trusted-user";
    context.allowed_tool_names = std::vector<std::string>{"vision_observe"};
    context.policy = policy;
    const auto execute = [&] {
        if (!GetParam()) return coordinator.Execute(response, context);
        std::promise<core::Result<std::vector<agent::llm::ChatMessage>>> promise;
        auto future = promise.get_future();
        auto status = coordinator.ExecuteAsync(response, context, [&promise](auto result) {
            promise.set_value(std::move(result));
        });
        if (!status.ok()) return core::Result<std::vector<agent::llm::ChatMessage>>(status);
        return future.get();
    };
    const auto error = [&] {
        auto result = execute();
        EXPECT_TRUE(result.ok());
        return nlohmann::json::parse(result.value().front().content)["error"]["code"].get<int>();
    };
    invocation->result.result_json = R"({"value":42})";
    context.allowed_tool_names = std::vector<std::string>{};
    EXPECT_EQ(error(), static_cast<int>(core::ErrorCode::PermissionDenied));
    EXPECT_EQ(invocation->calls, 0);
    context.allowed_tool_names = std::vector<std::string>{"vision_observe"};
    policy->authorize = core::Status::Error(core::ErrorCode::PermissionDenied, "business denial");
    EXPECT_EQ(error(), static_cast<int>(core::ErrorCode::PermissionDenied));
    EXPECT_EQ(invocation->calls, 0);
    policy->authorize = core::Status::Ok();
    invocation->result.result_json = R"({"value":"wrong"})";
    EXPECT_EQ(error(), static_cast<int>(core::ErrorCode::InvalidArgument));
    invocation->result.result_json = R"({"value":42})";
    policy->validate = core::Status::Error(core::ErrorCode::DataLoss, "source version rejected");
    EXPECT_EQ(error(), static_cast<int>(core::ErrorCode::DataLoss));
    policy->validate = core::Status::Ok();
    invocation->spoof_identity = true;
    EXPECT_EQ(error(), static_cast<int>(core::ErrorCode::DataLoss));
    invocation->spoof_identity = false;
    context.max_result_bytes = 8;
    EXPECT_EQ(error(), static_cast<int>(core::ErrorCode::ResourceExhausted));
    context.max_result_bytes = 65536;
    context.result_format = agent::skill::ToolResultFormat::EnvelopeV1;
    auto result = execute();
    ASSERT_TRUE(result.ok());
    const auto body = nlohmann::json::parse(result.value().front().content);
    EXPECT_EQ(body["status"], "success");
    EXPECT_EQ(body["call_id"], "contract-call");
    EXPECT_EQ(body["result"]["value"], 42);
}

INSTANTIATE_TEST_SUITE_P(SyncAndAsync, SkillCoordinatorContractTest, ::testing::Bool());

TEST(SkillToolCallCoordinatorTest, TimeoutCancellationCanCompleteSynchronouslyWithoutDeadlock) {
    auto registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
    ASSERT_TRUE(registry->Register(MakeManifest()).ok());
    auto invocation = std::make_shared<ContractInvocation>();
    invocation->deferred = true;
    agent::skill::SkillToolCallCoordinator coordinator(registry, invocation);
    agent::llm::ChatCompletionResponse response;
    response.tool_calls = {{"timeout-reentrant", "vision_observe", R"({"reason":"wait"})"}};
    agent::skill::SkillToolCallContext context;
    context.timeout = std::chrono::milliseconds(1);
    auto result = coordinator.Execute(response, context);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(invocation->cancels, 1);
    EXPECT_EQ(nlohmann::json::parse(result.value().front().content)["error"]["code"],
              static_cast<int>(core::ErrorCode::Timeout));
}

}
