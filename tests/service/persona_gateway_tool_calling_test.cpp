#include "persona_gateway_server.h"
#include "beast_http_client.h"
#include "../../src/skill/skill_executor.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>

namespace {

namespace gateway = agent::service::gateway;
namespace persona = agent::service::persona;
namespace skill = agent::skill;
namespace llm = agent::llm;
using Json = nlohmann::json;

constexpr auto kSession = "gateway-tool-session";
constexpr auto kUser = "gateway-tool-user";
constexpr auto kSkill = "test.lookup";
constexpr auto kCall = "gateway-tool-call";

class TestMemory final : public persona::IMemoryContextProvider {
public:
    core::Result<persona::RecalledContext> BuildContext(
        const persona::MemoryContextRequest&) override {
        return persona::RecalledContext{};
    }
    core::Status AdmitTurn(std::string_view, std::string_view, std::string_view,
                           const persona::ConversationTurn&, std::string_view) override {
        ++admissions;
        return core::Status::Ok();
    }
    std::atomic<int> admissions{0};
};

class TestToolMemory final : public persona::IToolMemoryProvider {
public:
    core::Result<persona::ToolMemoryContext> Query(const persona::ToolMemoryQuery&) override {
        persona::ToolMemoryContext context;
        context.hit = true;
        context.prompt_block = "Use test_lookup to retrieve the requested fact.";
        context.tools.push_back({"test_lookup", "retrieve a fact",
                                 R"({"type":"object","properties":{}})"});
        return context;
    }
};

class ImmediateExecutor final : public skill::ISkillExecutor {
public:
    core::Result<persona::SkillSessionSnapshot> Start(
        const skill::SkillExecutionRequest& request,
        skill::SkillExecutionCallbacks callbacks) override {
        ++starts;
        EXPECT_EQ(request.session_id, kSession);
        EXPECT_EQ(request.user_uuid, kUser);
        EXPECT_EQ(request.call.skill_id, kSkill);
        skill::SkillResult result;
        result.call_id = request.call.call_id;
        result.result_json = R"({"fact":"verified"})";
        callbacks.on_result(std::move(result));
        return persona::SkillSessionSnapshot{};
    }
    core::Status Cancel(std::string_view) override { return core::Status::Ok(); }
    std::atomic<int> starts{0};
};

class CompletedLlmOperation final : public llm::IAsyncLlmOperation {
public:
    // 测试客户端在接纳调用时已完成回调，取消已完成的操作没有副作用。
    void Cancel() noexcept override {}
};

class ToolCallingLlm final : public llm::ILlmClient, public llm::IAsyncLlmClient {
public:
    core::Result<llm::ChatCompletionResponse> Complete(
        const llm::ChatCompletionRequest& request) override {
        if (auto status = llm::ValidateChatCompletionRequest(request); !status.ok()) {
            return status;
        }
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
        llm::ChatCompletionResponse response;
        response.model = "gateway-tool-test";
        if (requests_.size() == 1) {
            response.tool_calls.push_back({kCall, "test_lookup", "{}"});
        } else {
            response.content = "grounded final reply";
        }
        return response;
    }
    core::Result<std::shared_ptr<llm::IAsyncLlmOperation>> CompleteAsync(
        llm::ChatCompletionRequest request, Callback callback) override {
        ++async_calls;
        callback(Complete(request));
        return std::shared_ptr<llm::IAsyncLlmOperation>(
            std::make_shared<CompletedLlmOperation>());
    }
    std::vector<llm::ChatCompletionRequest> Requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::atomic<int> async_calls{0};
private:
    mutable std::mutex mutex_;
    std::vector<llm::ChatCompletionRequest> requests_;
};

struct Harness {
    std::shared_ptr<TestMemory> memory = std::make_shared<TestMemory>();
    std::shared_ptr<ToolCallingLlm> llm_client = std::make_shared<ToolCallingLlm>();
    std::shared_ptr<skill::InMemorySkillRegistry> registry =
        std::make_shared<skill::InMemorySkillRegistry>();
    std::shared_ptr<skill::InMemorySkillExecutorFactory> factory =
        std::make_shared<skill::InMemorySkillExecutorFactory>();
    std::shared_ptr<persona::SkillSessionManager> sessions =
        std::make_shared<persona::SkillSessionManager>();
    std::shared_ptr<ImmediateExecutor> executor = std::make_shared<ImmediateExecutor>();

    Harness() {
        skill::SkillManifest manifest;
        manifest.skill_id = kSkill;
        manifest.tool_name = "test_lookup";
        manifest.version = "1.0.0";
        manifest.description = "retrieve a fact";
        manifest.executor.reference = kSkill;
        EXPECT_TRUE(registry->Register(std::move(manifest)).ok());
        EXPECT_TRUE(factory->RegisterReference("native", kSkill, executor).ok());
    }

    gateway::PersonaGatewayServerOptions Options() const {
        gateway::PersonaGatewayServerOptions options;
        options.http.address = "127.0.0.1";
        options.http.port = 0;
        options.http.io_threads = 1;
        options.compute_pool.worker_count = 1;
        options.io_pool.worker_count = 1;
        options.compute_pool.queue_capacity = 32;
        options.io_pool.queue_capacity = 32;
        options.runtime.default_model = "gateway-tool-test";
        gateway::PersonaMetadataRecord metadata;
        metadata.user_uuid = kUser;
        metadata.persona_id = "tool-persona";
        metadata.personality.name = "tool-persona";
        options.default_personas.push_back(std::move(metadata));
        return options;
    }

    gateway::PersonaGatewayServerDependencies Dependencies(bool async) const {
        gateway::PersonaGatewayServerDependencies dependencies;
        dependencies.memory_provider = memory;
        dependencies.emotion_analyzer = std::make_shared<persona::NeutralEmotionAnalyzer>();
        dependencies.llm_client = llm_client;
        if (async) dependencies.async_llm_client = llm_client;
        dependencies.tool_memory_provider = std::make_shared<TestToolMemory>();
        dependencies.skill_registry = registry;
        dependencies.skill_session_manager = sessions;
        dependencies.skill_executor_factory = factory;
        return dependencies;
    }

    void CheckFollowUp(bool async, bool has_executor = true) const {
        const auto requests = llm_client->Requests();
        ASSERT_EQ(requests.size(), 2u);
        ASSERT_EQ(requests.front().tools.size(), 1u);
        EXPECT_EQ(requests.front().tools.front().name, "test_lookup");
        const auto& messages = requests.back().messages;
        ASSERT_GE(messages.size(), 2u);
        ASSERT_EQ(messages.back().role, llm::ChatRole::Tool);
        EXPECT_EQ(messages.back().tool_call_id, kCall);
        const auto& assistant = messages[messages.size() - 2];
        ASSERT_EQ(assistant.role, llm::ChatRole::Assistant);
        ASSERT_EQ(assistant.tool_calls.size(), 1u);
        EXPECT_EQ(assistant.tool_calls.front().id, kCall);
        const auto output = Json::parse(messages.back().content);
        if (has_executor) {
            EXPECT_EQ(output.at("fact"), "verified");
        } else {
            EXPECT_EQ(output.at("error").at("code"),
                      static_cast<int>(core::ErrorCode::NotFound));
        }
        EXPECT_EQ(executor->starts.load(), has_executor ? 1 : 0);
        EXPECT_EQ(llm_client->async_calls.load(), async ? 2 : 0);
        EXPECT_EQ(memory->admissions.load(), 1);
    }
};

void CreateSession(gateway::PersonaGatewayServer& server) {
    gateway::CreateSessionGatewayRequest request;
    request.session_id = kSession;
    request.user_uuid = kUser;
    request.persona_id = "tool-persona";
    auto created = server.service().CreateSession(std::move(request));
    ASSERT_TRUE(created.ok()) << created.status().message();
}

void Chat(gateway::PersonaGatewayServer& server) {
    gateway::ChatGatewayRequest request;
    request.session_id = kSession;
    request.authenticated_user_uuid = kUser;
    request.message = "retrieve the fact";
    auto reply = server.service().Chat(std::move(request));
    ASSERT_TRUE(reply.ok()) << reply.status().message();
    EXPECT_EQ(reply.value().content, "grounded final reply");
    auto snapshot = server.sessions().GetSessionSnapshot(kSession);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.value().recent_turn_count, 1u);
}

class GatewayToolCallingTest : public testing::TestWithParam<bool> {};

TEST_P(GatewayToolCallingTest, AutomaticallyAssemblesCoordinatorAndCompletesSkill) {
    Harness harness;
    gateway::PersonaGatewayServer server(harness.Options(), harness.Dependencies(GetParam()));
    ASSERT_TRUE(server.Start().ok());
    CreateSession(server);
    Chat(server);
    harness.CheckFollowUp(GetParam());
    auto skill_session = harness.sessions->Get(kSession, kSkill);
    ASSERT_TRUE(skill_session.ok());
    ASSERT_TRUE(skill_session.value().has_value());
    EXPECT_EQ(skill_session.value()->state, persona::SkillSessionState::Closed);
}

TEST_P(GatewayToolCallingTest, DefaultEmptyFactoryReturnsStructuredToolError) {
    Harness harness;
    auto dependencies = harness.Dependencies(GetParam());
    dependencies.skill_executor_factory.reset();
    gateway::PersonaGatewayServer server(harness.Options(), std::move(dependencies));
    ASSERT_TRUE(server.Start().ok());
    CreateSession(server);
    Chat(server);
    harness.CheckFollowUp(GetParam(), false);
}

TEST_P(GatewayToolCallingTest, ExplicitCoordinatorOverridesAutomaticAssembly) {
    Harness harness;
    auto dependencies = harness.Dependencies(GetParam());
    dependencies.skill_executor_factory = std::make_shared<skill::InMemorySkillExecutorFactory>();
    auto invocation = std::make_shared<skill::SkillInvocationService>(
        harness.registry, harness.factory, harness.sessions);
    dependencies.skill_tool_coordinator = std::make_shared<skill::SkillToolCallCoordinator>(
        harness.registry, std::move(invocation));
    gateway::PersonaGatewayServer server(harness.Options(), std::move(dependencies));
    ASSERT_TRUE(server.Start().ok());
    CreateSession(server);
    Chat(server);
    harness.CheckFollowUp(GetParam());
}

// SQLite 测试文件由 RAII 清理，真实 socket E2E 无需外部数据库或上游模型。
class TestAuthDatabase {
public:
    TestAuthDatabase()
        : path(std::filesystem::current_path() /
               ("gateway_tool_auth_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite")) {}
    ~TestAuthDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
    std::filesystem::path path;
};

TEST_P(GatewayToolCallingTest, HttpChatRunsToolAndReturnsGroundedFollowUp) {
    Harness harness;
    TestAuthDatabase database;
    auto options = harness.Options();
    auto keys = gateway::GenerateDevelopmentRsaKeyPair();
    ASSERT_TRUE(keys.ok()) << keys.status().message();
    options.auth.enabled = true;
    options.auth.require_auth_for_api = true;
    options.auth.allow_dev_identity = false;
    options.auth.enable_dev_registration = true;
    options.auth.public_key_pem = keys.value().public_key_pem;
    options.auth.private_key_pem = keys.value().private_key_pem;
    options.auth.session_database_path = database.path.string();
    gateway::PersonaGatewayServer server(std::move(options), harness.Dependencies(GetParam()));
    ASSERT_TRUE(server.Start().ok());
    auto client = agent::net::BeastHttpClient::Create({});
    ASSERT_TRUE(client.ok());
    std::string cookie;
    auto post = [&](std::string_view target, const Json& body) {
        agent::net::HttpClientRequest request;
        request.url = "http://127.0.0.1:" + std::to_string(server.port()) + std::string(target);
        request.headers.push_back({"Content-Type", "application/json"});
        if (!cookie.empty()) request.headers.push_back({"Cookie", cookie});
        request.body = body.dump();
        request.timeout_ms = 5000;
        return client.value()->Execute(request);
    };
    auto auth = post("/api/auth/register", {{"userUuid", kUser}, {"ttlSeconds", 60}});
    ASSERT_TRUE(auth.ok()) << auth.status().message();
    ASSERT_EQ(auth.value().status, 200) << auth.value().body;
    for (const auto& header : auth.value().headers) {
        if (header.name == "Set-Cookie") cookie = header.value;
    }
    ASSERT_FALSE(cookie.empty());
    auto create = post("/api/session/create", {{"sessionId", kSession}, {"personaId", "tool-persona"}});
    ASSERT_TRUE(create.ok()) << create.status().message();
    ASSERT_EQ(create.value().status, 200) << create.value().body;
    auto chat = post("/api/chat/message", {{"sessionId", kSession}, {"message", "retrieve the fact"}});
    ASSERT_TRUE(chat.ok()) << chat.status().message();
    ASSERT_EQ(chat.value().status, 200) << chat.value().body;
    const auto body = Json::parse(chat.value().body);
    EXPECT_TRUE(body.at("ok").get<bool>());
    EXPECT_EQ(body.at("data").at("reply").at("content"), "grounded final reply");
    EXPECT_EQ(body.at("data").at("turnIndex"), 1);
    harness.CheckFollowUp(GetParam());
}

INSTANTIATE_TEST_SUITE_P(SyncAndAsync, GatewayToolCallingTest, testing::Bool());

TEST(GatewayToolCallingConfigurationTest, RejectsExecutorFactoryWithoutSkillDependencies) {
    Harness harness;
    auto dependencies = harness.Dependencies(false);
    dependencies.skill_session_manager.reset();
    gateway::PersonaGatewayServer server(harness.Options(), std::move(dependencies));
    auto started = server.Start();
    EXPECT_EQ(started.code(), core::ErrorCode::FailedPrecondition);
}

}
