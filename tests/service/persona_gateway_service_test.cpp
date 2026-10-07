#include "persona_gateway_http_adapter.h"
#include "persona_gateway_server.h"
#include "persona_gateway_service.h"
#include "persona_interaction.h"
#include "http_server.h"
#include "redis_connection_pool.h"
#include "document_analysis_service.h"
#include "document_file_store.h"
#include "sqlite/sqlite_connection_pool.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zip.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <future>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

using agent::service::gateway::ChatGatewayRequest;
using agent::service::gateway::AuthIdentity;
using agent::service::gateway::AuthRegistrationRequest;
using agent::service::gateway::AuthRegistrationResult;
using agent::service::gateway::AuthLoginRequest;
using agent::service::gateway::ClassroomMessageGatewayRequest;
using agent::service::gateway::ClassroomProactiveGatewayRequest;
using agent::service::gateway::ClassroomPollGatewayRequest;
using agent::service::gateway::ClassroomScheduler;
using agent::service::gateway::CloseSessionGatewayRequest;
using agent::service::gateway::CreateSessionGatewayRequest;
using agent::service::gateway::PersonaMetadataGatewayRequest;
using agent::service::gateway::PersonaGatewayHttpAdapter;
using agent::service::gateway::InMemoryPersonaMetadataStore;
using agent::service::gateway::OverlayPersonaMetadataStore;
using agent::service::gateway::ServerDefaultPersonaMetadataStore;
using agent::service::gateway::SqlitePersonaMetadataStore;
using agent::service::gateway::RedisPersonaMetadataCache;
using agent::service::gateway::CachedPersonaMetadataStore;
using agent::service::gateway::PersonaGatewayServer;
using agent::service::gateway::PersonaGatewayServerDependencies;
using agent::service::gateway::PersonaGatewayServerOptions;
using agent::service::gateway::PersonaGatewayService;
using agent::service::gateway::IGatewayAuthenticator;
using agent::service::gateway::IAuthRegistrationService;
using agent::service::gateway::IReportEvaluator;
using agent::service::gateway::ReportEvaluationRequest;
using agent::service::gateway::SqliteAuthSessionStore;
using agent::service::gateway::RedisAuthSessionStore;
using agent::service::gateway::AuthSessionRecord;
using agent::service::gateway::AuthUserRecord;
using agent::service::gateway::TrainingReportGatewayRequest;
using agent::service::gateway::GatewayAuthOptions;
using agent::service::gateway::JwtAuthRegistrationService;
using agent::service::gateway::GenerateDevelopmentRsaKeyPair;
using storage::sqlite::SqliteConnectionPool;
using storage::sqlite::SqliteConnectionPoolOptions;
using agent::document::DocumentAnalysisService;
using agent::document::DocumentFileStore;
using agent::document::DocumentFileStoreOptions;
using agent::service::persona::NeutralEmotionAnalyzer;
using agent::service::persona::PersonaRuntime;
using agent::service::persona::PersonaRuntimeOptions;
using agent::service::persona::PersonaInteraction;
using agent::service::persona::PersonaSessionQuery;
using agent::service::persona::ClosePersonaSessionRequest;
using agent::service::persona::PersonalityConfig;
using agent::service::persona::SemanticMemoryContextProvider;
using agent::service::persona::SessionManager;
using agent::service::persona::SkillSessionManager;
using Json = nlohmann::json;
namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

class FakeSemanticCache final : public agent::semantic_cache::ISemanticCache {
public:
    core::Result<agent::semantic_cache::CacheLookupResult> Lookup(
        const agent::semantic_cache::CacheLookupRequest&) override {
        agent::semantic_cache::CacheLookupResult result;
        result.hit = true;
        result.payload = "remembered context";
        result.similarity_score = 0.95f;
        return result;
    }

    core::Status Store(const agent::semantic_cache::CacheStoreRequest& req) override {
        std::lock_guard lock(mutex_);
        stored_payloads.push_back(req.response_payload);
        return core::Status::Ok();
    }

    std::mutex mutex_;
    std::vector<std::string> stored_payloads;
};

class FakeLlmClient final : public agent::llm::ILlmClient {
public:
    core::Result<agent::llm::ChatCompletionResponse> Complete(
        const agent::llm::ChatCompletionRequest& req) override {
        std::lock_guard lock(mutex_);
        last_request = req;
        agent::llm::ChatCompletionResponse response;
        response.model = req.model;
        response.content = "student reply";
        response.prompt_tokens = 10;
        response.completion_tokens = 6;
        response.total_tokens = 16;
        return response;
    }

    std::mutex mutex_;
    agent::llm::ChatCompletionRequest last_request;
};

class FakeReportEvaluator final : public IReportEvaluator {
public:
    explicit FakeReportEvaluator(bool fail = false) : fail_(fail) {}

    core::Result<Json> Evaluate(const ReportEvaluationRequest& request) override {
        last_request = request;
        if (fail_) {
            return core::Status::Error(core::ErrorCode::Unavailable, "evaluation backend unavailable");
        }
        return Json{{"provider", "test"}, {"score", 0.8}};
    }

    ReportEvaluationRequest last_request;

private:
    bool fail_ = false;
};

class FixedAuthenticator final : public IGatewayAuthenticator {
public:
    explicit FixedAuthenticator(AuthIdentity identity)
        : identity_(std::move(identity)) {}

    core::Result<AuthIdentity> Authenticate(const ::net::BeastHttpRequest&) const override {
        return identity_;
    }

private:
    AuthIdentity identity_;
};

class RejectingAuthenticator final : public IGatewayAuthenticator {
public:
    core::Result<AuthIdentity> Authenticate(const ::net::BeastHttpRequest&) const override {
        return core::Status::Error(core::ErrorCode::PermissionDenied, "auth token is missing");
    }
};

AuthIdentity TestAuthIdentity(std::string user_uuid = "test-user-001") {
    AuthIdentity identity;
    identity.authenticated = true;
    identity.user_uuid = std::move(user_uuid);
    identity.tenant_id = "default";
    identity.subject = identity.user_uuid;
    return identity;
}

class FixedAuthRegistrationService final : public IAuthRegistrationService {
public:
    core::Result<AuthRegistrationResult> Register(const AuthRegistrationRequest& request) override {
        last_request = request;
        return BuildResult(
            request.user_uuid.empty() ? "generated-user-001" : request.user_uuid,
            request.tenant_id.empty() ? "default" : request.tenant_id,
            request.subject);
    }

    core::Result<AuthRegistrationResult> Login(const AuthLoginRequest& request) override {
        last_login_request = request;
        if (request.username != "student@example.test" || request.password != "correct-password") {
            return core::Status::Error(core::ErrorCode::PermissionDenied, "invalid username or password");
        }
        return BuildResult("generated-user-001", "default", {});
    }

    AuthRegistrationRequest last_request;
    AuthLoginRequest last_login_request;

private:
    AuthRegistrationResult BuildResult(std::string user_uuid,
                                       std::string tenant_id,
                                       std::string subject) const {
        AuthRegistrationResult result;
        result.identity.user_uuid = std::move(user_uuid);
        result.identity.tenant_id = std::move(tenant_id);
        result.identity.subject = subject.empty() ? result.identity.user_uuid : std::move(subject);
        result.identity.token_id = "token-001";
        result.identity.authenticated = true;
        result.issued_at = std::chrono::system_clock::now();
        result.identity.expires_at = result.issued_at + std::chrono::hours(1);
        result.token = "jwt-token";
        result.cookie_header = "agent_auth=jwt-token; Path=/; HttpOnly; SameSite=Lax";
        return result;
    }
};

struct GatewayFixture {
    core::ThreadPool compute{{1, 64, "gateway-compute"}};
    core::ThreadPool io{{1, 64, "gateway-io"}};
    SessionManager sessions;
    std::shared_ptr<FakeSemanticCache> cache;
    std::shared_ptr<NeutralEmotionAnalyzer> emotion;
    std::shared_ptr<FakeLlmClient> llm;
    std::shared_ptr<SemanticMemoryContextProvider> memory;
    std::shared_ptr<SkillSessionManager> skill_sessions;
    PersonaRuntime runtime;
    ClassroomScheduler classroom_scheduler;
    std::shared_ptr<InMemoryPersonaMetadataStore> persona_metadata_store;
    PersonaGatewayService gateway;

    explicit GatewayFixture(bool enable_persona_metadata_store = false,
                            std::shared_ptr<IReportEvaluator> report_evaluator = nullptr)
        : sessions(compute, io),
          cache(std::make_shared<FakeSemanticCache>()),
          emotion(std::make_shared<NeutralEmotionAnalyzer>()),
          llm(std::make_shared<FakeLlmClient>()),
          memory(std::make_shared<SemanticMemoryContextProvider>(cache)),
          skill_sessions(std::make_shared<SkillSessionManager>()),
          runtime(sessions, memory, emotion, llm, PersonaRuntimeOptions{.recent_raw_turns = 10, .default_model = "test-model"}, nullptr, nullptr, skill_sessions),
          persona_metadata_store(enable_persona_metadata_store ? std::make_shared<InMemoryPersonaMetadataStore>() : nullptr),
          gateway(sessions, runtime, &classroom_scheduler, std::move(report_evaluator), persona_metadata_store) {
        EXPECT_TRUE(compute.Start().ok());
        EXPECT_TRUE(io.Start().ok());
    }

    ~GatewayFixture() {
        compute.Shutdown(true);
        io.Shutdown(true);
    }
};

CreateSessionGatewayRequest MakeCreateRequest() {
    PersonalityConfig personality;
    personality.name = "dazhi";
    personality.description = "classroom student persona";

    CreateSessionGatewayRequest req;
    req.trace_id = "trace-create";
    req.session_id = "session-gateway";
    req.user_uuid = "user-gateway";
    req.persona_id = "dazhi";
    req.classroom_id = "classroom-a";
    req.context_ids = {"group_1"};
    req.default_persona = true;
    req.personality = std::move(personality);
    req.emotion_state_config.noise_sigma = 0.0;
    return req;
}

TEST(PersonaInteractionTest, EnsuresSessionAndEnforcesTrustedOwner) {
    GatewayFixture fixture;
    PersonaInteraction interaction(fixture.sessions, fixture.runtime);

    auto request = MakeCreateRequest();
    agent::service::persona::CreateSessionRequest create;
    create.session_id = request.session_id;
    create.user_uuid = request.user_uuid;
    create.persona_id = request.persona_id;
    create.trace_id = request.trace_id;
    create.personality = request.personality;
    create.emotion_state_config.noise_sigma = 0.0;

    auto created = interaction.EnsureSession(create);
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto ensured = interaction.EnsureSession(std::move(create));
    ASSERT_TRUE(ensured.ok()) << ensured.status().message();
    EXPECT_EQ(fixture.sessions.SessionCount(), 1u);

    // Application 使用的公共 PersonaInteraction 边界必须保留 Runtime 返回的协议用量。
    agent::service::persona::PersonaTurnRequest turn;
    turn.trusted_user_uuid = request.user_uuid;
    turn.turn.session_id = request.session_id;
    turn.turn.user_input = "usage propagation";
    std::promise<core::Result<agent::service::persona::ChatResponse>> completed;
    auto future = completed.get_future();
    ASSERT_TRUE(interaction.SubmitTurn(std::move(turn), [&completed](auto result) {
        completed.set_value(std::move(result));
    }).ok());
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().prompt_tokens, 10);
    EXPECT_EQ(result.value().completion_tokens, 6);
    EXPECT_EQ(result.value().total_tokens, 16);

    auto denied = interaction.GetSession(PersonaSessionQuery{
        created.value().session_id,
        "trace-denied",
        "another-user",
    });
    EXPECT_EQ(denied.status().code(), core::ErrorCode::PermissionDenied);

    auto closed = interaction.CloseSession(ClosePersonaSessionRequest{
        created.value().session_id,
        "trace-close",
        request.user_uuid,
        "consumer_close",
    });
    ASSERT_TRUE(closed.ok()) << closed.status().message();
    EXPECT_EQ(closed.value().status, agent::service::persona::SessionStatus::Closed);
    EXPECT_EQ(closed.value().close_reason, "consumer_close");
    EXPECT_EQ(fixture.sessions.SessionCount(), 0u);
}

TEST(PersonaInteractionTest, CancelsActiveTurnAfterValidatingSessionOwner) {
    GatewayFixture fixture;
    PersonaInteraction interaction(fixture.sessions, fixture.runtime);
    auto create = MakeCreateRequest();
    agent::service::persona::CreateSessionRequest session;
    session.session_id = create.session_id;
    session.user_uuid = create.user_uuid;
    session.persona_id = create.persona_id;
    session.personality = create.personality;
    ASSERT_TRUE(interaction.CreateSession(std::move(session)).ok());

    auto wrong_owner = interaction.CancelTurn({create.session_id, "trace-denied", "another-user"});
    EXPECT_EQ(wrong_owner.code(), core::ErrorCode::PermissionDenied);
    auto missing_turn = interaction.CancelTurn({create.session_id, "trace-missing", create.user_uuid});
    EXPECT_EQ(missing_turn.code(), core::ErrorCode::NotFound);
}

PersonaMetadataGatewayRequest MakePersonaMetadataRequest(std::string user_uuid = "user-gateway",
                                                         std::string persona_id = "dazhi",
                                                         std::string description = "stored persona") {
    PersonaMetadataGatewayRequest req;
    req.trace_id = "trace-persona-upsert";
    req.tenant_id = "default";
    req.user_uuid = std::move(user_uuid);
    req.persona_id = std::move(persona_id);
    req.personality.name = req.persona_id;
    req.personality.description = std::move(description);
    req.emotion_state_config.noise_sigma = 0.0;
    return req;
}

::net::BeastHttpResponse SendJsonRequest(std::uint16_t port,
                                         ::net::http::verb method,
                                         std::string target,
                                         Json body,
                                         std::vector<std::pair<std::string, std::string>> headers = {}) {
    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(port)));

    ::net::BeastHttpRequest req{method, std::move(target), 11};
    req.set(::net::http::field::host, "127.0.0.1");
    req.set(::net::http::field::content_type, "application/json");
    req.set("X-Trace-Id", body.value("traceId", "trace-http"));
    for (const auto& [name, value] : headers) {
        req.set(name, value);
    }
    req.body() = body.dump();
    req.prepare_payload();
    ::net::http::write(stream, req);

    beast::flat_buffer buffer;
    ::net::BeastHttpResponse response;
    ::net::http::read(stream, buffer, response);
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream.socket().close(ec);
    return response;
}

std::string RegisterDevAuthCookie(std::uint16_t port, std::string user_uuid = "test-user-001") {
    auto response = SendJsonRequest(
        port,
        ::net::http::verb::post,
        "/api/auth/register",
        Json{
            {"traceId", "trace-dev-register"},
            {"userUuid", std::move(user_uuid)},
            {"tenantId", "default"},
            {"ttlSeconds", 600},
        });
    EXPECT_EQ(response.result(), ::net::http::status::ok);
    return std::string(response[::net::http::field::set_cookie]);
}

void EnableGatewayTestAuth(PersonaGatewayServerOptions& options, const std::filesystem::path& database_path) {
    auto keys = GenerateDevelopmentRsaKeyPair();
    ASSERT_TRUE(keys.ok()) << keys.status().message();
    options.auth.enabled = true;
    options.auth.public_key_pem = keys.value().public_key_pem;
    options.auth.private_key_pem = keys.value().private_key_pem;
    options.auth.session_store_backend = "sqlite";
    options.auth.session_database_path = database_path.string();
    options.auth.enable_dev_registration = true;
    options.auth.allow_dev_identity = false;
    options.auth.require_auth_for_api = true;
}

Json SendWebSocketJson(std::uint16_t port, std::string target, Json body) {
    asio::io_context io;
    tcp::resolver resolver(io);
    beast::websocket::stream<tcp::socket> ws(io);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
    asio::connect(ws.next_layer(), endpoints);
    ws.handshake("127.0.0.1", target);
    ws.text(true);
    ws.write(asio::buffer(body.dump()));

    beast::flat_buffer buffer;
    ws.read(buffer);
    auto response = Json::parse(beast::buffers_to_string(buffer.data()));
    beast::error_code ec;
    ws.close(beast::websocket::close_code::normal, ec);
    return response;
}

std::string Base64Encode(std::string_view input) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const auto b0 = static_cast<unsigned char>(input[i++]);
        const auto b1 = static_cast<unsigned char>(input[i++]);
        const auto b2 = static_cast<unsigned char>(input[i++]);
        out.push_back(kTable[b0 >> 2]);
        out.push_back(kTable[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(kTable[((b1 & 0x0f) << 2) | (b2 >> 6)]);
        out.push_back(kTable[b2 & 0x3f]);
    }
    const auto remaining = input.size() - i;
    if (remaining == 1) {
        const auto b0 = static_cast<unsigned char>(input[i]);
        out.push_back(kTable[b0 >> 2]);
        out.push_back(kTable[(b0 & 0x03) << 4]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const auto b0 = static_cast<unsigned char>(input[i++]);
        const auto b1 = static_cast<unsigned char>(input[i]);
        out.push_back(kTable[b0 >> 2]);
        out.push_back(kTable[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(kTable[(b1 & 0x0f) << 2]);
        out.push_back('=');
    }
    return out;
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string data;
    input.seekg(0, std::ios::end);
    data.resize(static_cast<std::size_t>(input.tellg()));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(data.data(), static_cast<std::streamsize>(data.size()));
    }
    return data;
}

void AddZipText(zip_t* archive, const char* name, const std::string& text) {
    auto* source = zip_source_buffer(archive, text.data(), text.size(), 0);
    ASSERT_NE(source, nullptr);
    ASSERT_GE(zip_file_add(archive, name, source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8), 0);
}

std::filesystem::path TempPath(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

std::filesystem::path UploadTempPathForTest(const std::string& upload_id) {
    std::string file_name = "agent_document_upload_";
    for (char ch : upload_id) {
        const auto safe = std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
        file_name.push_back(safe ? ch : '_');
    }
    file_name += ".tmp";
    return std::filesystem::temp_directory_path() / file_name;
}

bool FileExistsForTest(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string PathUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

void WriteMinimalDocx(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    int error = 0;
    zip_t* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    ASSERT_NE(archive, nullptr);
    const std::string content_types_xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
</Types>)";
    AddZipText(archive, "[Content_Types].xml", content_types_xml);
    const std::string document_xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body>
    <w:p>
      <w:pPr><w:pStyle w:val="Heading1"/></w:pPr>
      <w:r><w:t>函数基础</w:t></w:r>
    </w:p>
    <w:p>
      <w:r><w:t>目标：理解函数输入与输出之间的对应关系。例题：根据图像判断函数单调性。练习：完成课后检测题。</w:t></w:r>
    </w:p>
  </w:body>
</w:document>)";
    AddZipText(archive, "word/document.xml", document_xml);
    ASSERT_EQ(zip_close(archive), 0);
}

void WriteCorruptDocx(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    out << "not a zip archive";
}

TEST(PersonaGatewayServiceTest, RunsCreateChatReportAndCloseLifecycle) {
    GatewayFixture f;

    auto created = f.gateway.CreateSession(MakeCreateRequest());
    ASSERT_TRUE(created.ok()) << created.status().message();
    EXPECT_EQ(created.value().session.status, agent::service::persona::SessionStatus::Active);

    ChatGatewayRequest chat;
    chat.trace_id = "trace-chat";
    chat.session_id = "session-gateway";
    chat.persona_id = "dazhi";
    chat.message = "hello";
    auto reply = f.gateway.Chat(std::move(chat));
    ASSERT_TRUE(reply.ok()) << reply.status().message();
    EXPECT_EQ(reply.value().content, "student reply");
    EXPECT_EQ(reply.value().turn_index, 1u);
    EXPECT_TRUE(reply.value().l0_hit);

    TrainingReportGatewayRequest report;
    report.trace_id = "trace-report";
    report.session_id = "session-gateway";
    auto report_result = f.gateway.TrainingReport(std::move(report));
    ASSERT_TRUE(report_result.ok()) << report_result.status().message();
    EXPECT_EQ(report_result.value().total_turns, 1u);
    EXPECT_EQ(report_result.value().metrics.request_count, 1u);

    ClassroomProactiveGatewayRequest proactive;
    proactive.trace_id = "trace-proactive";
    proactive.classroom_id = "classroom-a";
    proactive.session_id = "session-gateway";
    proactive.persona_id = "dazhi";
    auto proactive_result = f.gateway.ClassroomProactive(std::move(proactive));
    ASSERT_TRUE(proactive_result.ok()) << proactive_result.status().message();
    EXPECT_TRUE(proactive_result.value().should_speak);
    EXPECT_EQ(proactive_result.value().turn_index, 2u);

    CloseSessionGatewayRequest close;
    close.trace_id = "trace-close";
    close.session_id = "session-gateway";
    auto closed = f.gateway.CloseSession(std::move(close));
    ASSERT_TRUE(closed.ok()) << closed.status().message();
    EXPECT_EQ(closed.value().session.status, agent::service::persona::SessionStatus::Closed);

    ChatGatewayRequest after_close;
    after_close.trace_id = "trace-after-close";
    after_close.session_id = "session-gateway";
    after_close.message = "again";
    auto rejected = f.gateway.Chat(std::move(after_close));
    EXPECT_FALSE(rejected.ok());
}

TEST(PersonaGatewayServiceTest, UsesInjectedReportEvaluatorWithoutOwningDomainConfiguration) {
    auto evaluator = std::make_shared<FakeReportEvaluator>();
    GatewayFixture f(false, evaluator);

    auto created = f.gateway.CreateSession(MakeCreateRequest());
    ASSERT_TRUE(created.ok()) << created.status().message();

    TrainingReportGatewayRequest report;
    report.trace_id = "trace-evaluation";
    report.session_id = "session-gateway";
    auto result = f.gateway.TrainingReport(std::move(report));

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().evaluation["provider"], "test");
    EXPECT_DOUBLE_EQ(result.value().evaluation["score"].get<double>(), 0.8);
    EXPECT_EQ(evaluator->last_request.trace_id, "trace-evaluation");
    EXPECT_EQ(evaluator->last_request.session_id, "session-gateway");
    EXPECT_EQ(evaluator->last_request.user_uuid, "user-gateway");
}

TEST(PersonaGatewayServiceTest, KeepsSessionMetricsWhenInjectedReportEvaluatorFails) {
    auto evaluator = std::make_shared<FakeReportEvaluator>(true);
    GatewayFixture f(false, evaluator);

    auto created = f.gateway.CreateSession(MakeCreateRequest());
    ASSERT_TRUE(created.ok()) << created.status().message();

    TrainingReportGatewayRequest report;
    report.trace_id = "trace-evaluation-failure";
    report.session_id = "session-gateway";
    auto result = f.gateway.TrainingReport(std::move(report));

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().total_turns, 0u);
    EXPECT_EQ(result.value().evaluation["error"], "evaluation backend unavailable");
    EXPECT_EQ(result.value().summary, "Session report evaluation unavailable; metrics are available.");
}

TEST(PersonaGatewayServiceTest, RequiresAccountPersonaMetadataWhenStoreIsConfigured) {
    GatewayFixture f(true);

    auto missing = f.gateway.CreateSession(MakeCreateRequest());
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), core::ErrorCode::NotFound);

    auto upsert = f.gateway.UpsertPersonaMetadata(MakePersonaMetadataRequest());
    ASSERT_TRUE(upsert.ok()) << upsert.status().message();

    auto req = MakeCreateRequest();
    req.personality.description = "forged session body persona";
    auto created = f.gateway.CreateSession(std::move(req));
    ASSERT_TRUE(created.ok()) << created.status().message();

    ChatGatewayRequest chat;
    chat.trace_id = "trace-persona-metadata-chat";
    chat.session_id = "session-gateway";
    chat.message = "hello";
    auto reply = f.gateway.Chat(std::move(chat));
    ASSERT_TRUE(reply.ok()) << reply.status().message();
    {
        std::lock_guard lock(f.llm->mutex_);
        ASSERT_FALSE(f.llm->last_request.messages.empty());
        EXPECT_NE(f.llm->last_request.messages.front().content.find("stored persona"), std::string::npos);
        EXPECT_EQ(f.llm->last_request.messages.front().content.find("forged session body persona"), std::string::npos);
    }
}

TEST(PersonaGatewayServiceTest, PersonaMetadataIsScopedToAuthenticatedAccount) {
    GatewayFixture f(true);
    auto upsert = f.gateway.UpsertPersonaMetadata(MakePersonaMetadataRequest("owner-user", "dazhi", "owner persona"));
    ASSERT_TRUE(upsert.ok()) << upsert.status().message();

    auto attacker = MakeCreateRequest();
    attacker.user_uuid = "attacker-user";
    attacker.session_id = "session-attacker-persona";
    auto rejected = f.gateway.CreateSession(std::move(attacker));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), core::ErrorCode::NotFound);

    auto owner = MakeCreateRequest();
    owner.user_uuid = "owner-user";
    auto created = f.gateway.CreateSession(std::move(owner));
    ASSERT_TRUE(created.ok()) << created.status().message();
}

TEST(PersonaGatewayServiceTest, RoutesClassroomMessageByContextAndPollsProactiveState) {
    GatewayFixture f;

    auto created_a = f.gateway.CreateSession(MakeCreateRequest());
    ASSERT_TRUE(created_a.ok()) << created_a.status().message();

    auto req_b = MakeCreateRequest();
    req_b.trace_id = "trace-create-b";
    req_b.session_id = "session-b";
    req_b.persona_id = "xiaozhi";
    req_b.personality.name = "xiaozhi";
    req_b.context_ids = {"group_2"};
    req_b.default_persona = false;
    req_b.proactive_level = "medium";
    auto created_b = f.gateway.CreateSession(std::move(req_b));
    ASSERT_TRUE(created_b.ok()) << created_b.status().message();

    ClassroomMessageGatewayRequest message;
    message.trace_id = "trace-classroom-message";
    message.classroom_id = "classroom-a";
    message.context_id = "group_2";
    message.message = "hello xiaozhi";
    auto reply = f.gateway.ClassroomMessage(std::move(message));
    ASSERT_TRUE(reply.ok()) << reply.status().message();
    EXPECT_EQ(reply.value().session_id, "session-b");
    EXPECT_EQ(reply.value().speaker_persona_id, "xiaozhi");
    EXPECT_EQ(reply.value().classroom_id, "classroom-a");

    ClassroomPollGatewayRequest poll;
    poll.trace_id = "trace-poll";
    poll.classroom_id = "classroom-a";
    poll.persona_id = "xiaozhi";
    auto no_speak = f.gateway.ClassroomPoll(std::move(poll));
    ASSERT_TRUE(no_speak.ok()) << no_speak.status().message();
    EXPECT_FALSE(no_speak.value().should_speak);

    ClassroomPollGatewayRequest system_poll;
    system_poll.trace_id = "trace-system-poll";
    system_poll.classroom_id = "classroom-a";
    system_poll.persona_id = "xiaozhi";
    system_poll.system_event = true;
    system_poll.system_event_content = "teacher asks xiaozhi";
    auto proactive = f.gateway.ClassroomPoll(std::move(system_poll));
    ASSERT_TRUE(proactive.ok()) << proactive.status().message();
    EXPECT_TRUE(proactive.value().should_speak);
    EXPECT_EQ(proactive.value().speaker_persona_id, "xiaozhi");
}

TEST(PersonaGatewayServiceTest, ClassroomSessionBootstrapsAllAccountPersonas) {
    GatewayFixture f(true);
    ASSERT_TRUE(f.gateway.UpsertPersonaMetadata(
        MakePersonaMetadataRequest("user-gateway", "dazhi", "primary classroom persona")).ok());
    ASSERT_TRUE(f.gateway.UpsertPersonaMetadata(
        MakePersonaMetadataRequest("user-gateway", "xiaozhi", "secondary classroom persona")).ok());

    auto req = MakeCreateRequest();
    req.session_id = "session-primary-classroom";
    req.persona_id = "dazhi";
    req.default_persona = true;
    auto created = f.gateway.CreateSession(std::move(req));
    ASSERT_TRUE(created.ok()) << created.status().message();
    EXPECT_EQ(f.sessions.SessionCount(), 2u);

    ClassroomMessageGatewayRequest message;
    message.trace_id = "trace-classroom-bootstrap-message";
    message.classroom_id = "classroom-a";
    message.target_persona_id = "xiaozhi";
    message.authenticated_user_uuid = "user-gateway";
    message.message = "hello secondary persona";
    auto reply = f.gateway.ClassroomMessage(std::move(message));
    ASSERT_TRUE(reply.ok()) << reply.status().message();
    EXPECT_EQ(reply.value().speaker_persona_id, "xiaozhi");
    EXPECT_NE(reply.value().session_id, "session-primary-classroom");

    {
        std::lock_guard lock(f.llm->mutex_);
        ASSERT_FALSE(f.llm->last_request.messages.empty());
        EXPECT_NE(f.llm->last_request.messages.front().content.find("secondary classroom persona"), std::string::npos);
    }
}

TEST(PersonaGatewayServiceTest, RejectsChatAndClassroomWhenAuthenticatedUserDoesNotOwnSession) {
    GatewayFixture f;
    auto created = f.gateway.CreateSession(MakeCreateRequest());
    ASSERT_TRUE(created.ok()) << created.status().message();

    ChatGatewayRequest chat;
    chat.trace_id = "trace-chat-owner-mismatch";
    chat.session_id = "session-gateway";
    chat.authenticated_user_uuid = "attacker-user";
    chat.message = "expensive call";
    auto rejected_chat = f.gateway.Chat(std::move(chat));
    EXPECT_FALSE(rejected_chat.ok());
    EXPECT_EQ(rejected_chat.status().code(), core::ErrorCode::PermissionDenied);

    ClassroomMessageGatewayRequest classroom;
    classroom.trace_id = "trace-classroom-owner-mismatch";
    classroom.classroom_id = "classroom-a";
    classroom.session_id = "session-gateway";
    classroom.target_persona_id = "dazhi";
    classroom.authenticated_user_uuid = "attacker-user";
    classroom.message = "expensive classroom call";
    auto rejected_classroom = f.gateway.ClassroomMessage(std::move(classroom));
    EXPECT_FALSE(rejected_classroom.ok());
    EXPECT_EQ(rejected_classroom.status().code(), core::ErrorCode::PermissionDenied);
}

TEST(PersonaGatewayHttpAdapterTest, HandlesSessionCreateAndChatJsonRoutes) {
    GatewayFixture f;
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("user-http")));
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto create = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/session/create",
        Json{
            {"traceId", "trace-http-create"},
            {"sessionId", "session-http"},
            {"userUuid", "user-http"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "student"}}},
        });
    EXPECT_EQ(create.result(), ::net::http::status::ok);
    auto create_body = Json::parse(create.body());
    EXPECT_TRUE(create_body["ok"].get<bool>());
    EXPECT_EQ(create_body["data"]["sessionId"], "session-http");

    auto chat = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/chat/message",
        Json{
            {"traceId", "trace-http-chat"},
            {"sessionId", "session-http"},
            {"personaId", "dazhi"},
            {"message", "hello"},
        });
    EXPECT_EQ(chat.result(), ::net::http::status::ok);
    auto chat_body = Json::parse(chat.body());
    EXPECT_TRUE(chat_body["ok"].get<bool>());
    EXPECT_EQ(chat_body["data"]["reply"]["content"], "student reply");
    EXPECT_EQ(chat_body["data"]["turnIndex"], 1);
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, ManagesPersonaMetadataByAuthenticatedAccount) {
    GatewayFixture f(true);
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("persona-http-user")));
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto upsert = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/persona",
        Json{
            {"traceId", "trace-http-persona-upsert"},
            {"userUuid", "forged-user"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "stored http persona"}}},
        });
    ASSERT_EQ(upsert.result(), ::net::http::status::ok);
    auto upsert_body = Json::parse(upsert.body());
    EXPECT_EQ(upsert_body["data"]["userUuid"], "persona-http-user");
    EXPECT_EQ(upsert_body["data"]["personality"]["description"], "stored http persona");

    auto get = SendJsonRequest(
        server.port(),
        ::net::http::verb::get,
        "/api/persona/dazhi",
        Json{{"traceId", "trace-http-persona-get"}});
    ASSERT_EQ(get.result(), ::net::http::status::ok);
    EXPECT_EQ(Json::parse(get.body())["data"]["personality"]["description"], "stored http persona");

    auto list = SendJsonRequest(
        server.port(),
        ::net::http::verb::get,
        "/api/personas",
        Json{{"traceId", "trace-http-persona-list"}});
    ASSERT_EQ(list.result(), ::net::http::status::ok);
    auto list_body = Json::parse(list.body());
    ASSERT_TRUE(list_body["ok"].get<bool>());
    ASSERT_TRUE(list_body["data"].is_array());
    ASSERT_EQ(list_body["data"].size(), 1u);
    EXPECT_EQ(list_body["data"][0]["userUuid"], "persona-http-user");
    EXPECT_EQ(list_body["data"][0]["personaId"], "dazhi");
    EXPECT_EQ(list_body["data"][0]["personality"]["description"], "stored http persona");

    auto create = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/session/create",
        Json{
            {"traceId", "trace-http-persona-session-create"},
            {"sessionId", "session-http-persona"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "forged create session persona"}}},
        });
    EXPECT_EQ(create.result(), ::net::http::status::ok);
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RoutesWebSocketMessagesThroughRegistry) {
    GatewayFixture f;
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("user-gateway")));
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetWebSocketStreamHandler("/ws/session", [&adapter](std::shared_ptr<::net::IWebSocketStreamRequest> request) {
        adapter.HandleWebSocket(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto created = f.gateway.CreateSession(MakeCreateRequest());
    ASSERT_TRUE(created.ok()) << created.status().message();

    auto chat = SendWebSocketJson(
        server.port(),
        "/ws/session",
        Json{
            {"type", "chat.message"},
            {"traceId", "trace-ws-chat"},
            {"payload", {
                {"sessionId", "session-gateway"},
                {"personaId", "dazhi"},
                {"message", "hello ws"},
            }},
        });
    EXPECT_EQ(chat["type"], "chat.final");
    EXPECT_EQ(chat["payload"]["traceId"], "trace-ws-chat");
    EXPECT_EQ(chat["payload"]["data"]["reply"]["content"], "student reply");

    auto unknown = SendWebSocketJson(
        server.port(),
        "/ws/session",
        Json{
            {"type", "unknown.message"},
            {"traceId", "trace-ws-unknown"},
        });
    EXPECT_EQ(unknown["type"], "error");
    EXPECT_EQ(unknown["payload"]["error"]["code"], "INVALID_ARGUMENT");

    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, HandlesSkillSessionHttpControlRoutes) {
    GatewayFixture f;
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("skill-user-001")),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        f.skill_sessions);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto started = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/skill/session/start",
        Json{
            {"traceId", "trace-skill-http-start"},
            {"skillId", "vision.observe"},
            {"sessionId", "session-skill-http"},
            {"userUuid", "forged-skill-user"},
            {"personaId", "dazhi"},
            {"source", "test"},
            {"reason", "start vision"},
            {"arguments", {{"mode", "camera"}}},
            {"maxDurationMs", 120000},
        });
    ASSERT_EQ(started.result(), ::net::http::status::ok);
    auto started_body = Json::parse(started.body());
    ASSERT_TRUE(started_body["ok"].get<bool>());
    EXPECT_EQ(started_body["data"]["skillId"], "vision.observe");
    EXPECT_EQ(started_body["data"]["sessionId"], "session-skill-http");
    EXPECT_EQ(started_body["data"]["userUuid"], "skill-user-001");
    EXPECT_EQ(started_body["data"]["state"], "starting");

    auto status = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/skill/session/status",
        Json{
            {"traceId", "trace-skill-http-status"},
            {"skillId", "vision.observe"},
            {"sessionId", "session-skill-http"},
        });
    ASSERT_EQ(status.result(), ::net::http::status::ok);
    auto status_body = Json::parse(status.body());
    ASSERT_TRUE(status_body["ok"].get<bool>());
    EXPECT_EQ(status_body["data"]["state"], "starting");

    auto stopped = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/skill/session/stop",
        Json{
            {"traceId", "trace-skill-http-stop"},
            {"skillId", "vision.observe"},
            {"sessionId", "session-skill-http"},
            {"source", "test"},
            {"reason", "stop vision"},
        });
    ASSERT_EQ(stopped.result(), ::net::http::status::ok);
    auto stopped_body = Json::parse(stopped.body());
    ASSERT_TRUE(stopped_body["ok"].get<bool>());
    EXPECT_EQ(stopped_body["data"]["state"], "closing");
    EXPECT_TRUE(stopped_body["data"]["executionId"].is_string());
    EXPECT_EQ(stopped_body["data"]["closeReason"], "stop vision");

    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, HandlesSkillSessionWebSocketControlRoutes) {
    GatewayFixture f;
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("skill-ws-user-001")),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        f.skill_sessions);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetWebSocketStreamHandler("/ws/session", [&adapter](std::shared_ptr<::net::IWebSocketStreamRequest> request) {
        adapter.HandleWebSocket(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto started = SendWebSocketJson(
        server.port(),
        "/ws/session",
        Json{
            {"type", "skill.session.start"},
            {"traceId", "trace-skill-ws-start"},
            {"payload", {
                {"skillId", "vision.observe"},
                {"sessionId", "session-skill-ws"},
                {"userUuid", "forged-skill-ws-user"},
                {"personaId", "dazhi"},
            }},
        });
    ASSERT_EQ(started["type"], "skill.session.started");
    EXPECT_EQ(started["payload"]["data"]["userUuid"], "skill-ws-user-001");
    EXPECT_EQ(started["payload"]["data"]["state"], "starting");

    auto status = SendWebSocketJson(
        server.port(),
        "/ws/session",
        Json{
            {"type", "skill.session.status"},
            {"traceId", "trace-skill-ws-status"},
            {"payload", {
                {"skillId", "vision.observe"},
                {"sessionId", "session-skill-ws"},
            }},
        });
    ASSERT_EQ(status["type"], "skill.session.status");
    EXPECT_EQ(status["payload"]["data"]["sessionId"], "session-skill-ws");
    EXPECT_EQ(status["payload"]["data"]["state"], "starting");

    auto stopped = SendWebSocketJson(
        server.port(),
        "/ws/session",
        Json{
            {"type", "skill.session.stop"},
            {"traceId", "trace-skill-ws-stop"},
            {"payload", {
                {"skillId", "vision.observe"},
                {"sessionId", "session-skill-ws"},
                {"reason", "ws stop"},
            }},
        });
    ASSERT_EQ(stopped["type"], "skill.session.stopped");
    EXPECT_EQ(stopped["payload"]["data"]["state"], "closing");
    EXPECT_TRUE(stopped["payload"]["data"]["executionId"].is_string());
    EXPECT_EQ(stopped["payload"]["data"]["closeReason"], "ws stop");

    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsSkillSessionAccessWhenJwtUserDoesNotOwnSession) {
    GatewayFixture f;
    PersonaGatewayHttpAdapter owner_adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("skill-owner")),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        f.skill_sessions);
    ::net::HttpServer owner_server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    owner_server.SetHttpRequestHandler([&owner_adapter](std::shared_ptr<::net::IHttpRequest> request) {
        owner_adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(owner_server.Start().ok());
    auto started = SendJsonRequest(
        owner_server.port(),
        ::net::http::verb::post,
        "/api/skill/session/start",
        Json{
            {"traceId", "trace-skill-owner-start"},
            {"skillId", "vision.observe"},
            {"sessionId", "session-skill-owned"},
            {"personaId", "dazhi"},
        });
    ASSERT_EQ(started.result(), ::net::http::status::ok);
    owner_server.Stop();

    PersonaGatewayHttpAdapter attacker_adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("skill-attacker")),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        f.skill_sessions);
    ::net::HttpServer attacker_server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    attacker_server.SetHttpRequestHandler([&attacker_adapter](std::shared_ptr<::net::IHttpRequest> request) {
        attacker_adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(attacker_server.Start().ok());

    auto status = SendJsonRequest(
        attacker_server.port(),
        ::net::http::verb::post,
        "/api/skill/session/status",
        Json{
            {"traceId", "trace-skill-attacker-status"},
            {"skillId", "vision.observe"},
            {"sessionId", "session-skill-owned"},
        });
    EXPECT_EQ(status.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(status.body())["error"]["code"], "PERMISSION_DENIED");

    auto stopped = SendJsonRequest(
        attacker_server.port(),
        ::net::http::verb::post,
        "/api/skill/session/stop",
        Json{
            {"traceId", "trace-skill-attacker-stop"},
            {"skillId", "vision.observe"},
            {"sessionId", "session-skill-owned"},
        });
    EXPECT_EQ(stopped.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(stopped.body())["error"]["code"], "PERMISSION_DENIED");
    attacker_server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsUnauthenticatedSkillSessionRoutes) {
    GatewayFixture f;
    AuthIdentity unauthenticated;
    unauthenticated.authenticated = false;
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(unauthenticated),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        f.skill_sessions);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    server.SetWebSocketStreamHandler("/ws/session", [&adapter](std::shared_ptr<::net::IWebSocketStreamRequest> request) {
        adapter.HandleWebSocket(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto started = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/skill/session/start",
        Json{
            {"traceId", "trace-unauth-skill-start"},
            {"skillId", "vision.observe"},
            {"sessionId", "session-unauth-skill"},
        });
    EXPECT_EQ(started.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(started.body())["error"]["code"], "PERMISSION_DENIED");

    auto ws_started = SendWebSocketJson(
        server.port(),
        "/ws/session",
        Json{
            {"type", "skill.session.start"},
            {"traceId", "trace-unauth-skill-ws-start"},
            {"payload", {
                {"skillId", "vision.observe"},
                {"sessionId", "session-unauth-skill-ws"},
            }},
        });
    EXPECT_EQ(ws_started["type"], "error");
    EXPECT_EQ(ws_started["payload"]["error"]["code"], "PERMISSION_DENIED");
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsUnauthenticatedDocumentUploadWebSocketRoute) {
    GatewayFixture f;
    auto document_service = std::make_shared<DocumentAnalysisService>(f.compute, f.io);
    AuthIdentity unauthenticated;
    unauthenticated.authenticated = false;
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(unauthenticated),
        nullptr,
        document_service,
        f.llm);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetWebSocketStreamHandler("/ws/session", [&adapter](std::shared_ptr<::net::IWebSocketStreamRequest> request) {
        adapter.HandleWebSocket(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto response = SendWebSocketJson(
        server.port(),
        "/ws/session",
        Json{
            {"type", "document.upload.start"},
            {"traceId", "trace-unauth-ws-upload"},
            {"payload", {
                {"fileName", "blocked.docx"},
                {"totalBytes", 10},
            }},
        });
    EXPECT_EQ(response["type"], "error");
    EXPECT_EQ(response["payload"]["error"]["code"], "PERMISSION_DENIED");
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsDocumentUploadChunkFromDifferentWebSocketConnection) {
    GatewayFixture f;
    auto document_service = std::make_shared<DocumentAnalysisService>(f.compute, f.io);
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("doc-user-001")),
        nullptr,
        document_service,
        f.llm,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        agent::service::gateway::PersonaGatewayHttpAdapterOptions{
            .enable_path_analyze_test_endpoint = true});
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetWebSocketStreamHandler("/ws/session", [&adapter](std::shared_ptr<::net::IWebSocketStreamRequest> request) {
        adapter.HandleWebSocket(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    asio::io_context io;
    tcp::resolver resolver(io);
    auto connect_ws = [&]() {
        beast::websocket::stream<tcp::socket> ws(io);
        auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
        asio::connect(ws.next_layer(), endpoints);
        ws.handshake("127.0.0.1", "/ws/session");
        ws.text(true);
        return ws;
    };
    auto send_ws = [](beast::websocket::stream<tcp::socket>& ws, Json body) {
        ws.write(asio::buffer(body.dump()));
        beast::flat_buffer buffer;
        ws.read(buffer);
        return Json::parse(beast::buffers_to_string(buffer.data()));
    };

    auto owner_ws = connect_ws();
    auto attacker_ws = connect_ws();
    auto started = send_ws(owner_ws, Json{
        {"type", "document.upload.start"},
        {"traceId", "trace-ws-upload-owner-start"},
        {"payload", {
            {"fileName", "owner-upload.docx"},
            {"totalBytes", 3},
        }},
    });
    ASSERT_EQ(started["type"], "document.upload.started");
    const auto upload_id = started["payload"]["uploadId"].get<std::string>();

    auto rejected = send_ws(attacker_ws, Json{
        {"type", "document.upload.chunk"},
        {"traceId", "trace-ws-upload-cross-connection"},
        {"payload", {
            {"uploadId", upload_id},
            {"offset", 0},
            {"data", Base64Encode("abc")},
        }},
    });
    EXPECT_EQ(rejected["type"], "error");
    EXPECT_EQ(rejected["payload"]["error"]["code"], "PERMISSION_DENIED");

    beast::error_code ec;
    owner_ws.close(beast::websocket::close_code::normal, ec);
    attacker_ws.close(beast::websocket::close_code::normal, ec);
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, HandlesDocumentAnalyzeRoute) {
    GatewayFixture f;
    auto document_service = std::make_shared<DocumentAnalysisService>(f.compute, f.io);
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("doc-user-001")),
        nullptr,
        document_service,
        f.llm,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        agent::service::gateway::PersonaGatewayHttpAdapterOptions{
            .enable_path_analyze_test_endpoint = true});
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    const auto path = TempPath("agent_gateway_document_route.docx");
    WriteMinimalDocx(path);

    auto response = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/document/analyze",
        Json{
            {"traceId", "trace-document-http"},
            {"documentId", "doc-http-1"},
            {"path", path.string()},
            {"fileName", "gateway-doc.docx"},
            {"maxChunkSlices", 3},
        });
    EXPECT_EQ(response.result(), ::net::http::status::ok);
    auto body = Json::parse(response.body());
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_EQ(body["traceId"], "trace-document-http");
    EXPECT_EQ(body["documentId"], "doc-http-1");
    EXPECT_EQ(body["data"]["schemaVersion"], "document_analysis.v1");
    EXPECT_EQ(body["data"]["fileName"], "gateway-doc.docx");
    EXPECT_TRUE(body["data"].contains("mindmap"));
    EXPECT_TRUE(body["data"].contains("diagnosis"));
    EXPECT_GE(body["data"]["chunks"].size(), 1u);

    server.Stop();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(PersonaGatewayHttpAdapterTest, RejectsDocumentPathAnalyzeWhenTestEndpointDisabled) {
    GatewayFixture f;
    auto document_service = std::make_shared<DocumentAnalysisService>(f.compute, f.io);
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("doc-user-001")),
        nullptr,
        document_service,
        f.llm);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    const auto path = TempPath("agent_gateway_document_path_analyze_disabled.docx");
    WriteMinimalDocx(path);

    auto response = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/document/analyze",
        Json{
            {"traceId", "trace-document-path-analyze-disabled"},
            {"path", PathUtf8(path)},
            {"fileName", "blocked-doc.docx"},
        });
    EXPECT_EQ(response.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(response.body())["error"]["code"], "PERMISSION_DENIED");

    server.Stop();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(PersonaGatewayHttpAdapterTest, RegistersManagedDocumentAndAnalyzesByDocumentId) {
    GatewayFixture f;
    const auto source_path = TempPath("agent_gateway_document_register.docx");
    const auto db_path = TempPath("agent_gateway_document_register.sqlite");
    const auto store_root = TempPath("agent_gateway_document_register_store");
    WriteMinimalDocx(source_path);
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove_all(store_root, ec);

    storage::sqlite::SqliteConnectionPoolOptions pool_options;
    pool_options.path = db_path.string();
    pool_options.read_connection_count = 1;
    pool_options.write_connection_count = 1;
    pool_options.busy_timeout_ms = 250;
    auto repository = std::make_shared<storage::sqlite::SqliteConnectionPool>(pool_options);
    ASSERT_TRUE(repository->Start().ok());

    auto document_service = std::make_shared<DocumentAnalysisService>(f.compute, f.io);
    ASSERT_TRUE(document_service->SetRepository(repository).ok());
    ASSERT_TRUE(document_service->SetFileStore(std::make_shared<DocumentFileStore>(DocumentFileStoreOptions{store_root})).ok());

    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("doc-user-001")),
        nullptr,
        document_service,
        f.llm,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        agent::service::gateway::PersonaGatewayHttpAdapterOptions{
            .enable_path_register_test_endpoint = true});
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto registered = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/document/register",
        Json{
            {"traceId", "trace-document-register"},
            {"path", PathUtf8(source_path)},
            {"fileName", "registered-doc.docx"},
            {"sessionId", "doc-session-001"},
        });
    ASSERT_EQ(registered.result(), ::net::http::status::ok);
    auto registered_body = Json::parse(registered.body());
    ASSERT_TRUE(registered_body["ok"].get<bool>());
    const auto document_id = registered_body["documentId"].get<std::string>();
    EXPECT_FALSE(document_id.empty());
    EXPECT_EQ(registered_body["data"]["ownerUserUuid"], "doc-user-001");
    EXPECT_EQ(registered_body["data"]["sessionId"], "doc-session-001");
    EXPECT_EQ(registered_body["data"]["analysisStatus"], "uploaded");

    auto analyzed = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/document/analyze",
        Json{
            {"traceId", "trace-document-id-analyze"},
            {"documentId", document_id},
            {"maxChunkSlices", 3},
        });
    EXPECT_EQ(analyzed.result(), ::net::http::status::ok);
    auto analyzed_body = Json::parse(analyzed.body());
    EXPECT_TRUE(analyzed_body["ok"].get<bool>());
    EXPECT_EQ(analyzed_body["documentId"], document_id);
    EXPECT_EQ(analyzed_body["data"]["fileName"], "registered-doc.docx");
    EXPECT_EQ(analyzed_body["data"]["fileType"], "docx");
    EXPECT_TRUE(analyzed_body["data"].contains("mindmap"));

    PersonaGatewayHttpAdapter attacker_adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("doc-attacker-001")),
        nullptr,
        document_service,
        f.llm);
    ::net::HttpServer attacker_server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    attacker_server.SetHttpRequestHandler([&attacker_adapter](std::shared_ptr<::net::IHttpRequest> request) {
        attacker_adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(attacker_server.Start().ok());

    auto rejected = SendJsonRequest(
        attacker_server.port(),
        ::net::http::verb::post,
        "/api/document/analyze",
        Json{
            {"traceId", "trace-document-attacker-analyze"},
            {"documentId", document_id},
        });
    EXPECT_EQ(rejected.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(rejected.body())["error"]["code"], "PERMISSION_DENIED");
    attacker_server.Stop();

    server.Stop();
    repository->Close();
    std::filesystem::remove(source_path, ec);
    std::filesystem::remove_all(store_root, ec);
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);
}

TEST(PersonaGatewayHttpAdapterTest, RejectsDocumentPathRegisterWhenTestEndpointDisabled) {
    GatewayFixture f;
    const auto source_path = TempPath("agent_gateway_document_register_disabled.docx");
    const auto db_path = TempPath("agent_gateway_document_register_disabled.sqlite");
    const auto store_root = TempPath("agent_gateway_document_register_disabled_store");
    WriteMinimalDocx(source_path);
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove_all(store_root, ec);

    storage::sqlite::SqliteConnectionPoolOptions pool_options;
    pool_options.path = db_path.string();
    pool_options.read_connection_count = 1;
    pool_options.write_connection_count = 1;
    pool_options.busy_timeout_ms = 250;
    auto repository = std::make_shared<storage::sqlite::SqliteConnectionPool>(pool_options);
    ASSERT_TRUE(repository->Start().ok());

    auto document_service = std::make_shared<DocumentAnalysisService>(f.compute, f.io);
    ASSERT_TRUE(document_service->SetRepository(repository).ok());
    ASSERT_TRUE(document_service->SetFileStore(std::make_shared<DocumentFileStore>(DocumentFileStoreOptions{store_root})).ok());

    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(TestAuthIdentity("doc-user-001")),
        nullptr,
        document_service,
        f.llm);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto response = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/document/register",
        Json{
            {"traceId", "trace-document-register-disabled"},
            {"path", PathUtf8(source_path)},
            {"fileName", "registered-doc.docx"},
        });
    EXPECT_EQ(response.result(), ::net::http::status::forbidden);

    server.Stop();
    repository->Close();
    std::filesystem::remove(source_path, ec);
    std::filesystem::remove_all(store_root, ec);
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);
}

TEST(PersonaGatewayHttpAdapterTest, PassesAccountPersonaEmotionPromptsIntoSessionPromptBuilder) {
    GatewayFixture f(true);
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("user-emotion-prompt")));
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto upsert = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/persona",
        Json{
            {"traceId", "trace-emotion-prompt-upsert"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "student"}}},
            {"emotionPrompts", {
                {"emotionMap", {{"neutral", "用户状态平稳，保持自然教学节奏"}}},
                {"emotionReliability", {{"neutral", 1.0}}},
                {"confidenceThresholds", {{"strong", 0.5}, {"weak", 0.3}}},
                {"intensityLevels", {{"high_min", 0.7}}},
            }},
        });
    ASSERT_EQ(upsert.result(), ::net::http::status::ok);

    auto create = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/session/create",
        Json{
            {"traceId", "trace-emotion-prompt-create"},
            {"sessionId", "session-emotion-prompt"},
            {"userUuid", "user-emotion-prompt"},
            {"personaId", "dazhi"},
        });
    ASSERT_EQ(create.result(), ::net::http::status::ok);

    auto chat = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/chat/message",
        Json{
            {"traceId", "trace-emotion-prompt-chat"},
            {"sessionId", "session-emotion-prompt"},
            {"personaId", "dazhi"},
            {"message", "hello"},
        });
    ASSERT_EQ(chat.result(), ::net::http::status::ok);
    {
        std::lock_guard lock(f.llm->mutex_);
        ASSERT_FALSE(f.llm->last_request.messages.empty());
        EXPECT_NE(f.llm->last_request.messages.front().content.find("用户状态平稳"), std::string::npos);
    }
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, AuthIdentityOverridesCreateSessionUserUuid) {
    GatewayFixture f;
    auto identity = TestAuthIdentity("jwt-user-001");
    identity.tenant_id = "tenant-a";
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<FixedAuthenticator>(identity));
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto create = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/session/create",
        Json{
            {"traceId", "trace-auth-create"},
            {"sessionId", "session-auth"},
            {"userUuid", "forged-body-user"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "student"}}},
        });
    EXPECT_EQ(create.result(), ::net::http::status::ok);
    auto create_body = Json::parse(create.body());
    EXPECT_EQ(create_body["data"]["userUuid"], "jwt-user-001");
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsChatWhenJwtUserDoesNotOwnSession) {
    GatewayFixture f;
    {
        PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("owner-user")));
        ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
        server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
            adapter.HandleHttp(std::move(request));
        });
        ASSERT_TRUE(server.Start().ok());
        auto create = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/session/create",
            Json{
                {"traceId", "trace-owner-create"},
                {"sessionId", "session-owner-bound"},
                {"personaId", "dazhi"},
                {"personality", {{"name", "dazhi"}, {"description", "student"}}},
            });
        ASSERT_EQ(create.result(), ::net::http::status::ok);
        server.Stop();
    }

    PersonaGatewayHttpAdapter attacker_adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("attacker-user")));
    ::net::HttpServer attacker_server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    attacker_server.SetHttpRequestHandler([&attacker_adapter](std::shared_ptr<::net::IHttpRequest> request) {
        attacker_adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(attacker_server.Start().ok());

    auto chat = SendJsonRequest(
        attacker_server.port(),
        ::net::http::verb::post,
        "/api/chat/message",
        Json{
            {"traceId", "trace-attacker-chat"},
            {"sessionId", "session-owner-bound"},
            {"personaId", "dazhi"},
            {"message", "steal expensive LLM call"},
        });
    EXPECT_EQ(chat.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(chat.body())["error"]["code"], "PERMISSION_DENIED");
    attacker_server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsSessionReadAndCloseWhenJwtUserDoesNotOwnSession) {
    GatewayFixture f;
    {
        PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("session-owner")));
        ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
        server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
            adapter.HandleHttp(std::move(request));
        });
        ASSERT_TRUE(server.Start().ok());
        auto create = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/session/create",
            Json{
                {"traceId", "trace-session-owner-create"},
                {"sessionId", "session-owner-read-close"},
                {"personaId", "dazhi"},
                {"personality", {{"name", "dazhi"}, {"description", "student"}}},
            });
        ASSERT_EQ(create.result(), ::net::http::status::ok);
        server.Stop();
    }

    PersonaGatewayHttpAdapter attacker_adapter(f.gateway, std::make_shared<FixedAuthenticator>(TestAuthIdentity("session-attacker")));
    ::net::HttpServer attacker_server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    attacker_server.SetHttpRequestHandler([&attacker_adapter](std::shared_ptr<::net::IHttpRequest> request) {
        attacker_adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(attacker_server.Start().ok());

    auto read = SendJsonRequest(
        attacker_server.port(),
        ::net::http::verb::get,
        "/api/session/session-owner-read-close",
        Json{{"traceId", "trace-session-attacker-read"}});
    EXPECT_EQ(read.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(read.body())["error"]["code"], "PERMISSION_DENIED");

    auto close = SendJsonRequest(
        attacker_server.port(),
        ::net::http::verb::post,
        "/api/session/close",
        Json{
            {"traceId", "trace-session-attacker-close"},
            {"sessionId", "session-owner-read-close"},
        });
    EXPECT_EQ(close.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(close.body())["error"]["code"], "PERMISSION_DENIED");
    attacker_server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsApiWhenAuthenticatorRejects) {
    GatewayFixture f;
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<RejectingAuthenticator>());
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto create = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/session/create",
        Json{
            {"traceId", "trace-auth-reject"},
            {"sessionId", "session-auth-reject"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "student"}}},
        });
    EXPECT_EQ(create.result(), ::net::http::status::forbidden);
    auto body = Json::parse(create.body());
    EXPECT_FALSE(body["ok"].get<bool>());
    EXPECT_EQ(body["error"]["code"], "PERMISSION_DENIED");
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RejectsUnauthenticatedSessionCreateAndDocumentRequests) {
    GatewayFixture f;
    auto document_service = std::make_shared<DocumentAnalysisService>(f.compute, f.io);
    AuthIdentity unauthenticated;
    unauthenticated.authenticated = false;
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<FixedAuthenticator>(unauthenticated),
        nullptr,
        document_service,
        f.llm);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto create = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/session/create",
        Json{
            {"traceId", "trace-unauth-create"},
            {"sessionId", "session-unauth"},
            {"personaId", "dazhi"},
        });
    EXPECT_EQ(create.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(create.body())["error"]["code"], "PERMISSION_DENIED");

    auto document = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/document/analyze",
        Json{
            {"traceId", "trace-unauth-document"},
            {"documentId", "doc-unauth"},
        });
    EXPECT_EQ(document.result(), ::net::http::status::forbidden);
    EXPECT_EQ(Json::parse(document.body())["error"]["code"], "PERMISSION_DENIED");
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, HealthRouteBypassesAuthenticatorAndReportsReadiness) {
    GatewayFixture f;
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<RejectingAuthenticator>());
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto response = SendJsonRequest(
        server.port(),
        ::net::http::verb::get,
        "/api/health",
        Json{{"traceId", "trace-health"}});
    EXPECT_EQ(response.result(), ::net::http::status::ok);
    EXPECT_EQ(std::string(response["X-Trace-Id"]), "trace-health");
    auto body = Json::parse(response.body());
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_EQ(body["traceId"], "trace-health");
    EXPECT_EQ(body["data"]["status"], "ok");
    EXPECT_TRUE(body["data"].contains("sessionCount"));
    EXPECT_TRUE(body["data"]["pools"].contains("compute"));
    EXPECT_TRUE(body["data"]["pools"].contains("io"));
    EXPECT_TRUE(body["data"]["pools"].contains("llm"));
    for (const auto* pool_name : {"compute", "io", "llm"}) {
        const auto& scheduler = body["data"]["pools"][pool_name]["scheduler"];
        EXPECT_TRUE(scheduler.contains("activeKeys"));
        EXPECT_TRUE(scheduler.contains("readyKeys"));
        EXPECT_TRUE(scheduler.contains("queuedTasks"));
        EXPECT_TRUE(scheduler.contains("runningTasks"));
        EXPECT_TRUE(scheduler.contains("rejectedTasks"));
        EXPECT_TRUE(scheduler.contains("rejectedGlobal"));
        EXPECT_TRUE(scheduler.contains("rejectedPerKey"));
        EXPECT_TRUE(scheduler.contains("rejectedPerFairnessKey"));
        EXPECT_TRUE(scheduler.contains("rejectedPerTenant"));
        EXPECT_TRUE(scheduler.contains("maxLaneDepth"));
    }
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, RegisterRouteRejectsWhenDevRegistrationDisabled) {
    GatewayFixture f;
    auto registration = std::make_shared<FixedAuthRegistrationService>();
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<RejectingAuthenticator>(), registration);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto response = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/auth/register",
        Json{
            {"traceId", "trace-register"},
            {"userUuid", "e2e-user-001"},
            {"tenantId", "default"},
            {"subject", "student@example.test"},
            {"ttlSeconds", 600},
        });
    EXPECT_EQ(response.result(), ::net::http::status::forbidden);
    auto body = Json::parse(response.body());
    EXPECT_FALSE(body["ok"].get<bool>());
    EXPECT_EQ(body["error"]["code"], "PERMISSION_DENIED");
    EXPECT_EQ(registration->last_request.ttl.count(), 0);
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, SignupRouteIssuesGeneratedIdentityAndIgnoresClientClaims) {
    GatewayFixture f;
    auto registration = std::make_shared<FixedAuthRegistrationService>();
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<RejectingAuthenticator>(), registration);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto response = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/auth/signup",
        Json{
            {"traceId", "trace-signup"},
            {"userUuid", "attacker-user"},
            {"tenantId", "admin-tenant"},
            {"subject", "should-not-be-used"},
            {"username", "student@example.test"},
            {"password", "correct-password"},
            {"ttlSeconds", 600},
        });
    EXPECT_EQ(response.result(), ::net::http::status::ok);
    EXPECT_NE(std::string(response[::net::http::field::set_cookie]).find("agent_auth=jwt-token"), std::string::npos);
    auto body = Json::parse(response.body());
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_EQ(body["data"]["authenticated"], true);
    EXPECT_EQ(body["data"]["userUuid"], "generated-user-001");
    EXPECT_EQ(body["data"]["tenantId"], "default");
    EXPECT_EQ(body["data"]["subject"], "generated-user-001");
    EXPECT_EQ(body["data"]["token"], "jwt-token");
    EXPECT_EQ(registration->last_request.username, "student@example.test");
    EXPECT_EQ(registration->last_request.password, "correct-password");
    EXPECT_TRUE(registration->last_request.user_uuid.empty());
    EXPECT_EQ(registration->last_request.tenant_id, "default");
    EXPECT_TRUE(registration->last_request.subject.empty());
    EXPECT_EQ(registration->last_request.ttl.count(), 600);
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, LoginRouteChecksPasswordAndSetsCookie) {
    GatewayFixture f;
    auto registration = std::make_shared<FixedAuthRegistrationService>();
    PersonaGatewayHttpAdapter adapter(f.gateway, std::make_shared<RejectingAuthenticator>(), registration);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto rejected = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/auth/login",
        Json{
            {"traceId", "trace-login-reject"},
            {"username", "student@example.test"},
            {"password", "wrong-password"},
        });
    EXPECT_EQ(rejected.result(), ::net::http::status::forbidden);

    auto accepted = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/auth/login",
        Json{
            {"traceId", "trace-login"},
            {"username", "student@example.test"},
            {"password", "correct-password"},
            {"ttlSeconds", 600},
        });
    EXPECT_EQ(accepted.result(), ::net::http::status::ok);
    EXPECT_NE(std::string(accepted[::net::http::field::set_cookie]).find("agent_auth=jwt-token"), std::string::npos);
    auto body = Json::parse(accepted.body());
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_EQ(body["data"]["userUuid"], "generated-user-001");
    EXPECT_EQ(registration->last_login_request.username, "student@example.test");
    EXPECT_EQ(registration->last_login_request.password, "correct-password");
    EXPECT_EQ(registration->last_login_request.ttl.count(), 600);
    server.Stop();
}

TEST(PersonaGatewayHttpAdapterTest, DevRegisterRouteBypassesAuthenticatorAndSetsCookieWhenEnabled) {
    GatewayFixture f;
    auto registration = std::make_shared<FixedAuthRegistrationService>();
    PersonaGatewayHttpAdapter adapter(
        f.gateway,
        std::make_shared<RejectingAuthenticator>(),
        registration,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        true);
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetHttpRequestHandler([&adapter](std::shared_ptr<::net::IHttpRequest> request) {
        adapter.HandleHttp(std::move(request));
    });
    ASSERT_TRUE(server.Start().ok());

    auto response = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/auth/register",
        Json{
            {"traceId", "trace-register"},
            {"userUuid", "e2e-user-001"},
            {"tenantId", "default"},
            {"subject", "student@example.test"},
            {"ttlSeconds", 600},
        });
    EXPECT_EQ(response.result(), ::net::http::status::ok);
    EXPECT_NE(std::string(response[::net::http::field::set_cookie]).find("agent_auth=jwt-token"), std::string::npos);
    auto body = Json::parse(response.body());
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_EQ(body["data"]["authenticated"], true);
    EXPECT_EQ(body["data"]["userUuid"], "e2e-user-001");
    EXPECT_EQ(body["data"]["tenantId"], "default");
    EXPECT_EQ(body["data"]["subject"], "student@example.test");
    EXPECT_EQ(body["data"]["token"], "jwt-token");
    EXPECT_EQ(registration->last_request.ttl.count(), 600);
    server.Stop();
}

TEST(GatewayAuthSessionStoreTest, PersistsResolvesAndRevokesSessions) {
    const auto path = std::filesystem::temp_directory_path() / "agent_gateway_auth_sessions_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    SqliteAuthSessionStore store(path.string());
    ASSERT_TRUE(store.EnsureSchema().ok());

    AuthSessionRecord record;
    record.token_id = "token-001";
    record.user_uuid = "uuid-001";
    record.tenant_id = "tenant-a";
    record.subject = "subject-001";
    record.issued_at = std::chrono::system_clock::now();
    record.expires_at = record.issued_at + std::chrono::hours(1);
    ASSERT_TRUE(store.UpsertSession(record).ok());

    auto resolved = store.ResolveSession("token-001");
    ASSERT_TRUE(resolved.ok()) << resolved.status().message();
    EXPECT_EQ(resolved.value().user_uuid, "uuid-001");
    EXPECT_EQ(resolved.value().tenant_id, "tenant-a");
    EXPECT_FALSE(resolved.value().revoked);

    ASSERT_TRUE(store.RevokeSession("token-001", "logout").ok());
    auto revoked = store.ResolveSession("token-001");
    ASSERT_TRUE(revoked.ok()) << revoked.status().message();
    EXPECT_TRUE(revoked.value().revoked);

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(GatewaySqliteMetadataStoreTest, SerializesConcurrentAuthAndPersonaWritesOnSharedPool) {
    const auto path = std::filesystem::temp_directory_path() /
        "agent_gateway_shared_metadata_concurrency_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    SqliteConnectionPoolOptions pool_options;
    pool_options.path = path.string();
    pool_options.read_connection_count = 4;
    pool_options.write_connection_count = 1;
    pool_options.busy_timeout_ms = 1000;
    pool_options.enable_wal = true;
    auto pool = std::make_shared<SqliteConnectionPool>(std::move(pool_options));
    ASSERT_TRUE(pool->Start().ok());

    SqliteAuthSessionStore auth(pool);
    SqlitePersonaMetadataStore personas(pool);
    ASSERT_TRUE(auth.EnsureSchema().ok());
    ASSERT_TRUE(personas.EnsureSchema().ok());

    constexpr int kConcurrentWorkers = 16;
    constexpr int kRecords = 64;
    std::atomic<int> next_record{0};
    std::promise<void> start_signal;
    auto start_gate = start_signal.get_future().share();
    std::vector<std::future<std::string>> writers;
    writers.reserve(kConcurrentWorkers);
    for (int worker = 0; worker < kConcurrentWorkers; ++worker) {
        writers.push_back(std::async(std::launch::async, [&, start_gate] {
            start_gate.wait();
            while (true) {
                const int i = next_record.fetch_add(1, std::memory_order_relaxed);
                if (i >= kRecords) {
                    return std::string{};
                }

                AuthSessionRecord session;
                session.token_id = "token-" + std::to_string(i);
                session.user_uuid = "user-" + std::to_string(i);
                session.tenant_id = "default";
                session.subject = "subject-" + std::to_string(i);
                session.issued_at = std::chrono::system_clock::now();
                session.expires_at = session.issued_at + std::chrono::hours(1);
                if (auto status = auth.UpsertSession(session); !status.ok()) {
                    return status.message();
                }

                auto request = MakePersonaMetadataRequest(
                    session.user_uuid,
                    "persona-" + std::to_string(i),
                    "concurrent persona");
                agent::service::gateway::PersonaMetadataRecord record{
                    request.tenant_id,
                    request.user_uuid,
                    request.persona_id,
                    request.personality,
                    request.emotion_prompt_config,
                    request.emotion_state_config,
                };
                if (auto status = personas.Upsert(std::move(record)); !status.ok()) {
                    return status.message();
                }
            }
        }));
    }
    start_signal.set_value();

    for (auto& writer : writers) {
        ASSERT_EQ(writer.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_TRUE(writer.get().empty());
    }

    auto session = auth.ResolveSession("token-63");
    ASSERT_TRUE(session.ok()) << session.status().message();
    EXPECT_EQ(session.value().user_uuid, "user-63");
    auto persona = personas.Get("default", "user-63", "persona-63");
    ASSERT_TRUE(persona.ok()) << persona.status().message();
    EXPECT_EQ(persona.value().personality.description, "concurrent persona");

    const auto stats = pool->Stats();
    EXPECT_EQ(stats.total_write_connections, 1u);
    EXPECT_EQ(stats.leased_write_connections, 0u);
    pool->Close();

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(GatewayAuthSessionStoreTest, CleansExpiredSessionsInLruOrder) {
    const auto path = std::filesystem::temp_directory_path() / "agent_gateway_auth_sessions_cleanup_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    SqliteAuthSessionStore store(path.string());
    ASSERT_TRUE(store.EnsureSchema().ok());

    const auto now = std::chrono::system_clock::now();
    AuthSessionRecord oldest;
    oldest.token_id = "token-expired-oldest";
    oldest.user_uuid = "uuid-oldest";
    oldest.issued_at = now - std::chrono::hours(3);
    oldest.expires_at = now - std::chrono::hours(2);
    oldest.updated_at = now - std::chrono::hours(3);
    ASSERT_TRUE(store.UpsertSession(oldest).ok());

    AuthSessionRecord newer;
    newer.token_id = "token-expired-newer";
    newer.user_uuid = "uuid-newer";
    newer.issued_at = now - std::chrono::hours(2);
    newer.expires_at = now - std::chrono::hours(1);
    newer.updated_at = now - std::chrono::hours(1);
    ASSERT_TRUE(store.UpsertSession(newer).ok());

    AuthSessionRecord active;
    active.token_id = "token-active";
    active.user_uuid = "uuid-active";
    active.issued_at = now;
    active.expires_at = now + std::chrono::hours(1);
    active.updated_at = now;
    ASSERT_TRUE(store.UpsertSession(active).ok());

    auto first = store.CleanupExpired(now, 1);
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_EQ(first.value(), 1u);
    EXPECT_EQ(store.ResolveSession("token-expired-oldest").status().code(), core::ErrorCode::NotFound);
    EXPECT_TRUE(store.ResolveSession("token-expired-newer").ok());
    EXPECT_TRUE(store.ResolveSession("token-active").ok());

    auto second = store.CleanupExpired(now, 8);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value(), 1u);
    EXPECT_EQ(store.ResolveSession("token-expired-newer").status().code(), core::ErrorCode::NotFound);
    EXPECT_TRUE(store.ResolveSession("token-active").ok());

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(PersonaMetadataStoreTest, SqlitePersistsPersonaMetadataAcrossInstances) {
    const auto path = std::filesystem::temp_directory_path() / "agent_gateway_persona_metadata_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    {
        SqlitePersonaMetadataStore store(path.string());
        ASSERT_TRUE(store.EnsureSchema().ok());
        auto req = MakePersonaMetadataRequest("persona-owner", "dazhi", "sqlite stored persona");
        ASSERT_TRUE(store.Upsert(agent::service::gateway::PersonaMetadataRecord{
            req.tenant_id,
            req.user_uuid,
            req.persona_id,
            req.personality,
            req.emotion_prompt_config,
            req.emotion_state_config,
        }).ok());
    }

    SqlitePersonaMetadataStore reopened(path.string());
    ASSERT_TRUE(reopened.EnsureSchema().ok());
    auto loaded = reopened.Get("default", "persona-owner", "dazhi");
    ASSERT_TRUE(loaded.ok()) << loaded.status().message();
    EXPECT_EQ(loaded.value().personality.description, "sqlite stored persona");
    EXPECT_EQ(loaded.value().user_uuid, "persona-owner");

    auto missing = reopened.Get("default", "other-user", "dazhi");
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), core::ErrorCode::NotFound);

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(PersonaMetadataStoreTest, OverlayPrefersServerDefaultPersonasBeforeAccountRecords) {
    auto defaults = std::make_shared<ServerDefaultPersonaMetadataStore>(std::vector<agent::service::gateway::PersonaMetadataRecord>{
        agent::service::gateway::PersonaMetadataRecord{
            "server",
            "server",
            "dazhi",
            agent::service::persona::PersonalityConfig{
                "dazhi",
                "server default persona",
                {},
                0.5,
                0.5,
                0.5,
                0.5,
                0.5,
                0.5,
            },
            std::nullopt,
            {},
        },
    });
    auto account = std::make_shared<InMemoryPersonaMetadataStore>();
    ASSERT_TRUE(account->Upsert(agent::service::gateway::PersonaMetadataRecord{
        "default",
        "owner",
        "dazhi",
        agent::service::persona::PersonalityConfig{
            "dazhi",
            "account persona should not win",
            {},
            0.5,
            0.5,
            0.5,
            0.5,
            0.5,
            0.5,
        },
        std::nullopt,
        {},
    }).ok());
    ASSERT_TRUE(account->Upsert(agent::service::gateway::PersonaMetadataRecord{
        "default",
        "owner",
        "xiaozhi",
        agent::service::persona::PersonalityConfig{
            "xiaozhi",
            "account only persona",
            {},
            0.5,
            0.5,
            0.5,
            0.5,
            0.5,
            0.5,
        },
        std::nullopt,
        {},
    }).ok());

    OverlayPersonaMetadataStore store(defaults, account);
    auto dazhi = store.Get("default", "owner", "dazhi");
    ASSERT_TRUE(dazhi.ok()) << dazhi.status().message();
    EXPECT_EQ(dazhi.value().user_uuid, "owner");
    EXPECT_EQ(dazhi.value().personality.description, "server default persona");

    auto listed = store.ListByAccount("default", "owner");
    ASSERT_TRUE(listed.ok()) << listed.status().message();
    ASSERT_EQ(listed.value().size(), 2u);
    EXPECT_EQ(listed.value()[0].persona_id, "dazhi");
    EXPECT_EQ(listed.value()[0].personality.description, "server default persona");
    EXPECT_EQ(listed.value()[1].persona_id, "xiaozhi");
    EXPECT_EQ(listed.value()[1].personality.description, "account only persona");
}

TEST(GatewayAuthRegistrationServiceTest, StoresUsernameAndPasswordHashWithoutUsingSubject) {
    const auto path = std::filesystem::temp_directory_path() / "agent_gateway_auth_registration_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    auto keys = GenerateDevelopmentRsaKeyPair();
    ASSERT_TRUE(keys.ok()) << keys.status().message();

    auto store = std::make_shared<SqliteAuthSessionStore>(path.string());
    ASSERT_TRUE(store->EnsureSchema().ok());

    GatewayAuthOptions options;
    options.enabled = true;
    options.public_key_pem = keys.value().public_key_pem;
    options.private_key_pem = keys.value().private_key_pem;
    options.token_ttl = std::chrono::minutes(30);
    JwtAuthRegistrationService service(options, store);

    AuthRegistrationRequest req;
    req.username = "student@example.test";
    req.password = "correct-password";
    auto registered = service.Register(req);
    ASSERT_TRUE(registered.ok()) << registered.status().message();
    EXPECT_EQ(registered.value().identity.tenant_id, "default");
    EXPECT_EQ(registered.value().identity.subject, registered.value().identity.user_uuid);

    auto duplicate = service.Register(req);
    EXPECT_FALSE(duplicate.ok());
    EXPECT_EQ(duplicate.status().code(), core::ErrorCode::AlreadyExists);

    auto record = store->ResolveSession(registered.value().identity.token_id);
    ASSERT_TRUE(record.ok()) << record.status().message();
    EXPECT_EQ(record.value().subject, registered.value().identity.user_uuid);

    auto user = store->ResolveUserByUsername("student@example.test");
    ASSERT_TRUE(user.ok()) << user.status().message();
    EXPECT_EQ(user.value().user_uuid, registered.value().identity.user_uuid);
    EXPECT_EQ(user.value().username, "student@example.test");
    EXPECT_FALSE(user.value().password_hash.empty());
    EXPECT_NE(user.value().password_hash, "correct-password");
    EXPECT_FALSE(user.value().password_salt.empty());
    EXPECT_GT(user.value().password_iterations, 0);
    EXPECT_EQ(user.value().subject, registered.value().identity.user_uuid);

    AuthLoginRequest bad_login;
    bad_login.username = "student@example.test";
    bad_login.password = "wrong-password";
    auto rejected = service.Login(bad_login);
    EXPECT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), core::ErrorCode::PermissionDenied);

    AuthLoginRequest login;
    login.username = "student@example.test";
    login.password = "correct-password";
    auto accepted = service.Login(login);
    ASSERT_TRUE(accepted.ok()) << accepted.status().message();
    EXPECT_EQ(accepted.value().identity.user_uuid, registered.value().identity.user_uuid);
    EXPECT_EQ(accepted.value().identity.tenant_id, "default");
    EXPECT_NE(accepted.value().identity.token_id, registered.value().identity.token_id);

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(GatewayAuthSessionStoreTest, RedisPersistsResolvesAndRevokesSessionsWhenAvailable) {
    agent::semantic_cache::RedisPoolOptions options;
    options.host = "127.0.0.1";
    options.port = "5000";
    options.pool_size = 4;
    options.connect_timeout = std::chrono::seconds(1);
    options.command_timeout = std::chrono::milliseconds(1000);
    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(options);
    auto started = redis->Start();
    if (!started.ok()) {
        GTEST_SKIP() << started.message();
    }

    RedisAuthSessionStore store(redis, "agent:test:gateway:auth");
    ASSERT_TRUE(store.EnsureSchema().ok());

    AuthSessionRecord record;
    record.token_id = "redis-token-001";
    record.user_uuid = "redis-uuid-001";
    record.tenant_id = "tenant-a";
    record.subject = "subject-001";
    record.issued_at = std::chrono::system_clock::now();
    record.expires_at = record.issued_at + std::chrono::hours(1);
    ASSERT_TRUE(store.UpsertSession(record).ok());

    auto resolved = store.ResolveSession("redis-token-001");
    ASSERT_TRUE(resolved.ok()) << resolved.status().message();
    EXPECT_EQ(resolved.value().user_uuid, "redis-uuid-001");
    EXPECT_EQ(resolved.value().tenant_id, "tenant-a");
    EXPECT_FALSE(resolved.value().revoked);

    AuthUserRecord user;
    user.user_uuid = "redis-uuid-001";
    user.tenant_id = "tenant-a";
    user.username = "redis-student@example.test";
    user.password_hash = "redis-hash";
    user.password_salt = "redis-salt";
    user.password_iterations = 120000;
    user.subject = "subject-001";
    user.created_at = record.issued_at;
    user.updated_at = record.issued_at;
    ASSERT_TRUE(store.UpsertUser(user).ok());
    auto duplicate_user = store.UpsertUser(user);
    EXPECT_FALSE(duplicate_user.ok());
    EXPECT_EQ(duplicate_user.code(), core::ErrorCode::AlreadyExists);
    auto resolved_user = store.ResolveUserByUsername("redis-student@example.test");
    ASSERT_TRUE(resolved_user.ok()) << resolved_user.status().message();
    EXPECT_EQ(resolved_user.value().user_uuid, "redis-uuid-001");
    EXPECT_EQ(resolved_user.value().password_hash, "redis-hash");

    ASSERT_TRUE(store.RevokeSession("redis-token-001", "logout").ok());
    auto revoked = store.ResolveSession("redis-token-001");
    ASSERT_TRUE(revoked.ok()) << revoked.status().message();
    EXPECT_TRUE(revoked.value().revoked);

    auto del = redis->Del({
        "agent:test:gateway:auth:session:redis-token-001",
        "agent:test:gateway:auth:user:redis-uuid-001",
        "agent:test:gateway:auth:username:redis-student@example.test"});
    EXPECT_TRUE(del.ok()) << del.status().message();
    redis->Shutdown();
}

TEST(PersonaGatewayServerTest, HostsApiWebSocketAdapterAndStaticDistOnOneHttpServer) {
    const auto static_root = std::filesystem::current_path() / "persona_gateway_static_test";
    const auto auth_db_path = TempPath("agent_gateway_server_static_auth.sqlite");
    std::error_code cleanup_error;
    std::filesystem::remove(auth_db_path, cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-wal", cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-shm", cleanup_error);
    std::filesystem::create_directories(static_root);
    {
        std::ofstream index(static_root / "index.html", std::ios::trunc);
        index << "<!doctype html><title>Gateway Dist</title>";
    }

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();

    PersonaGatewayServerOptions options;
    options.http.address = "127.0.0.1";
    options.http.port = 0;
    options.http.io_threads = 1;
    options.compute_pool.worker_count = 1;
    options.compute_pool.queue_capacity = 64;
    options.io_pool.worker_count = 1;
    options.io_pool.queue_capacity = 64;
    options.runtime.default_model = "test-model";
    options.static_files = ::net::StaticFileOptions{.root = static_root, .index_file = "index.html", .spa_fallback = true};
    EnableGatewayTestAuth(options, auth_db_path);

    PersonaGatewayServerDependencies dependencies;
    dependencies.memory_provider = memory;
    dependencies.emotion_analyzer = emotion;
    dependencies.llm_client = llm;
   {
    PersonaGatewayServer server(std::move(options), std::move(dependencies));
    ASSERT_TRUE(server.Start().ok());
    const auto auth_cookie = RegisterDevAuthCookie(server.port(), "user-server");

    auto persona = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/persona",
        Json{
            {"traceId", "trace-server-persona"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "student"}}},
        },
        {{"Cookie", auth_cookie}});
    ASSERT_EQ(persona.result(), ::net::http::status::ok);

    auto create = SendJsonRequest(
        server.port(),
        ::net::http::verb::post,
        "/api/session/create",
        Json{
            {"traceId", "trace-server-create"},
            {"sessionId", "session-server"},
            {"userUuid", "user-server"},
            {"personaId", "dazhi"},
            {"personality", {{"name", "dazhi"}, {"description", "student"}}},
        },
        {{"Cookie", auth_cookie}});
    EXPECT_EQ(create.result(), ::net::http::status::ok);
    EXPECT_TRUE(Json::parse(create.body())["ok"].get<bool>());

    auto index = SendJsonRequest(server.port(), ::net::http::verb::get, "/", Json::object());
    EXPECT_EQ(index.result(), ::net::http::status::ok);
    EXPECT_NE(index.body().find("Gateway Dist"), std::string::npos);

    server.Stop();
   }
    std::filesystem::remove_all(static_root, cleanup_error);
    std::filesystem::remove(auth_db_path, cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-wal", cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-shm", cleanup_error);
}

TEST(PersonaGatewayServerTest, SerializesConcurrentAuthAndPersonaWritesOverHttp) {
    const auto auth_db_path = TempPath("agent_gateway_concurrent_metadata.sqlite");
    std::error_code cleanup_error;
    std::filesystem::remove(auth_db_path, cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-wal", cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-shm", cleanup_error);

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();

    PersonaGatewayServerOptions options;
    options.http.address = "127.0.0.1";
    options.http.port = 0;
    options.http.io_threads = 16;
    options.compute_pool.worker_count = 4;
    options.compute_pool.queue_capacity = 256;
    options.io_pool.worker_count = 4;
    options.io_pool.queue_capacity = 256;
    options.runtime.default_model = "test-model";
    EnableGatewayTestAuth(options, auth_db_path);

    PersonaGatewayServerDependencies dependencies;
    dependencies.memory_provider = std::move(memory);
    dependencies.emotion_analyzer = std::move(emotion);
    dependencies.llm_client = std::move(llm);

    {
        PersonaGatewayServer server(std::move(options), std::move(dependencies));
        ASSERT_TRUE(server.Start().ok());

        constexpr int kConcurrentClients = 16;
        constexpr int kAccounts = 64;
        std::atomic<int> next_account{0};
        std::promise<void> start_signal;
        auto start_gate = start_signal.get_future().share();
        std::vector<std::future<std::string>> clients;
        clients.reserve(kConcurrentClients);
        for (int worker = 0; worker < kConcurrentClients; ++worker) {
            clients.push_back(std::async(std::launch::async, [&, start_gate] {
                start_gate.wait();
                try {
                    while (true) {
                        const int i = next_account.fetch_add(1, std::memory_order_relaxed);
                        if (i >= kAccounts) {
                            return std::string{};
                        }
                        const auto user_uuid = "http-user-" + std::to_string(i);
                        auto registered = SendJsonRequest(
                            server.port(),
                            ::net::http::verb::post,
                            "/api/auth/register",
                            Json{
                                {"traceId", "trace-http-register-" + std::to_string(i)},
                                {"userUuid", user_uuid},
                                {"tenantId", "default"},
                                {"ttlSeconds", 600},
                            });
                        if (registered.result() != ::net::http::status::ok) {
                            return "register failed: " + registered.body();
                        }

                        auto persona = SendJsonRequest(
                            server.port(),
                            ::net::http::verb::post,
                            "/api/persona",
                            Json{
                                {"traceId", "trace-http-persona-" + std::to_string(i)},
                                {"personaId", "persona-" + std::to_string(i)},
                                {"personality", {
                                    {"name", "persona-" + std::to_string(i)},
                                    {"description", "concurrent HTTP persona"},
                                }},
                            },
                            {{"Cookie", std::string(registered[::net::http::field::set_cookie])}});
                        if (persona.result() != ::net::http::status::ok) {
                            return "persona upsert failed: " + persona.body();
                        }
                    }
                } catch (const std::exception& error) {
                    return std::string(error.what());
                }
            }));
        }
        start_signal.set_value();

        for (auto& client : clients) {
            ASSERT_EQ(client.wait_for(std::chrono::seconds(15)), std::future_status::ready);
            EXPECT_TRUE(client.get().empty());
        }
        server.Stop();
    }

    std::filesystem::remove(auth_db_path, cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-wal", cleanup_error);
    std::filesystem::remove(auth_db_path.string() + "-shm", cleanup_error);
}

TEST(PersonaGatewayServerTest, EnablesConfiguredDocumentStoreForRegisterAndAnalyze) {
    const auto source_path = TempPath("agent_gateway_server_document.docx");
    const auto db_path = TempPath("agent_gateway_server_document.sqlite");
    const auto auth_db_path = TempPath("agent_gateway_server_document_auth.sqlite");
    const auto store_root = TempPath("agent_gateway_server_document_store");
    WriteMinimalDocx(source_path);
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
    std::filesystem::remove_all(store_root, ec);

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();

    PersonaGatewayServerOptions options;
    options.http.address = "127.0.0.1";
    options.http.port = 0;
    options.http.io_threads = 1;
    options.compute_pool.worker_count = 1;
    options.compute_pool.queue_capacity = 64;
    options.io_pool.worker_count = 1;
    options.io_pool.queue_capacity = 64;
    options.runtime.default_model = "test-model";
    options.document_store.enabled = true;
    options.document_store.root = store_root;
    options.document_store.database_path = db_path;
    options.document_store.read_connection_count = 1;
    options.document_store.write_connection_count = 1;
    options.document_store.busy_timeout_ms = 250;
    options.document_store.enable_path_register_test_endpoint = true;
    EnableGatewayTestAuth(options, auth_db_path);

    PersonaGatewayServerDependencies dependencies;
    dependencies.memory_provider = memory;
    dependencies.emotion_analyzer = emotion;
    dependencies.llm_client = llm;

    {
        PersonaGatewayServer server(std::move(options), std::move(dependencies));
        ASSERT_TRUE(server.Start().ok());
        const auto auth_cookie = RegisterDevAuthCookie(server.port(), "doc-user-001");

        auto registered = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/document/register",
            Json{
                {"traceId", "trace-server-document-register"},
                {"path", PathUtf8(source_path)},
                {"fileName", "server-managed.docx"},
            },
            {{"Cookie", auth_cookie}});
        ASSERT_EQ(registered.result(), ::net::http::status::ok);
        auto registered_body = Json::parse(registered.body());
        ASSERT_TRUE(registered_body["ok"].get<bool>());
        const auto document_id = registered_body["documentId"].get<std::string>();

        auto analyzed = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/document/analyze",
            Json{
                {"traceId", "trace-server-document-analyze"},
                {"documentId", document_id},
            },
            {{"Cookie", auth_cookie}});
        ASSERT_EQ(analyzed.result(), ::net::http::status::ok);
        auto analyzed_body = Json::parse(analyzed.body());
        EXPECT_TRUE(analyzed_body["ok"].get<bool>());
        EXPECT_EQ(analyzed_body["documentId"], document_id);
        EXPECT_EQ(analyzed_body["data"]["fileName"], "server-managed.docx");

        server.Stop();
    }

    std::filesystem::remove(source_path, ec);
    std::filesystem::remove_all(store_root, ec);
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
}

TEST(PersonaGatewayServerTest, DeletesManagedDocumentRejectedByZipVerification) {
    const auto source_path = TempPath("agent_gateway_server_corrupt_document.docx");
    const auto db_path = TempPath("agent_gateway_server_corrupt_document.sqlite");
    const auto auth_db_path = TempPath("agent_gateway_server_corrupt_document_auth.sqlite");
    const auto store_root = TempPath("agent_gateway_server_corrupt_document_store");
    WriteCorruptDocx(source_path);
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
    std::filesystem::remove_all(store_root, ec);

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();

    PersonaGatewayServerOptions options;
    options.http.address = "127.0.0.1";
    options.http.port = 0;
    options.http.io_threads = 1;
    options.compute_pool.worker_count = 1;
    options.compute_pool.queue_capacity = 64;
    options.io_pool.worker_count = 1;
    options.io_pool.queue_capacity = 64;
    options.runtime.default_model = "test-model";
    options.document_store.enabled = true;
    options.document_store.root = store_root;
    options.document_store.database_path = db_path;
    options.document_store.read_connection_count = 1;
    options.document_store.write_connection_count = 1;
    options.document_store.busy_timeout_ms = 250;
    options.document_store.enable_path_register_test_endpoint = true;
    EnableGatewayTestAuth(options, auth_db_path);

    PersonaGatewayServerDependencies dependencies;
    dependencies.memory_provider = memory;
    dependencies.emotion_analyzer = emotion;
    dependencies.llm_client = llm;

    {
        PersonaGatewayServer server(std::move(options), std::move(dependencies));
        ASSERT_TRUE(server.Start().ok());
        const auto auth_cookie = RegisterDevAuthCookie(server.port(), "doc-corrupt-user-001");

        auto registered = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/document/register",
            Json{
                {"traceId", "trace-corrupt-document-register"},
                {"path", PathUtf8(source_path)},
                {"fileName", "corrupt-managed.docx"},
            },
            {{"Cookie", auth_cookie}});
        ASSERT_EQ(registered.result(), ::net::http::status::ok);
        auto registered_body = Json::parse(registered.body());
        ASSERT_TRUE(registered_body["ok"].get<bool>());
        const auto document_id = registered_body["documentId"].get<std::string>();
        const auto managed_path = store_root / (document_id + ".docx");
        ASSERT_TRUE(std::filesystem::exists(managed_path));

        auto analyzed = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/document/analyze",
            Json{
                {"traceId", "trace-corrupt-document-analyze"},
                {"documentId", document_id},
            },
            {{"Cookie", auth_cookie}});
        EXPECT_EQ(analyzed.result(), ::net::http::status::bad_request);
        EXPECT_FALSE(std::filesystem::exists(managed_path));

        auto second_analyze = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/document/analyze",
            Json{
                {"traceId", "trace-corrupt-document-analyze-again"},
                {"documentId", document_id},
            },
            {{"Cookie", auth_cookie}});
        EXPECT_EQ(second_analyze.result(), ::net::http::status::not_found);

        server.Stop();
    }

    std::filesystem::remove(source_path, ec);
    std::filesystem::remove_all(store_root, ec);
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
}

TEST(PersonaGatewayServerTest, UploadsDocumentOverWebSocketAndAnalyzesByDocumentId) {
    const auto source_path = TempPath("agent_gateway_server_ws_upload.docx");
    const auto db_path = TempPath("agent_gateway_server_ws_upload.sqlite");
    const auto auth_db_path = TempPath("agent_gateway_server_ws_upload_auth.sqlite");
    const auto store_root = TempPath("agent_gateway_server_ws_upload_store");
    WriteMinimalDocx(source_path);
    const auto file_bytes = ReadBinaryFile(source_path);
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
    std::filesystem::remove_all(store_root, ec);

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();

    PersonaGatewayServerOptions options;
    options.http.address = "127.0.0.1";
    options.http.port = 0;
    options.http.io_threads = 1;
    options.compute_pool.worker_count = 1;
    options.compute_pool.queue_capacity = 64;
    options.io_pool.worker_count = 1;
    options.io_pool.queue_capacity = 64;
    options.runtime.default_model = "test-model";
    options.document_store.enabled = true;
    options.document_store.root = store_root;
    options.document_store.database_path = db_path;
    options.document_store.read_connection_count = 1;
    options.document_store.write_connection_count = 1;
    options.document_store.busy_timeout_ms = 250;
    EnableGatewayTestAuth(options, auth_db_path);

    PersonaGatewayServerDependencies dependencies;
    dependencies.memory_provider = memory;
    dependencies.emotion_analyzer = emotion;
    dependencies.llm_client = llm;

    {
        PersonaGatewayServer server(std::move(options), std::move(dependencies));
        ASSERT_TRUE(server.Start().ok());
        const auto auth_cookie = RegisterDevAuthCookie(server.port(), "ws-doc-user-001");

        asio::io_context io;
        tcp::resolver resolver(io);
        beast::websocket::stream<tcp::socket> ws(io);
        auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
        asio::connect(ws.next_layer(), endpoints);
        ws.set_option(beast::websocket::stream_base::decorator([&auth_cookie](beast::websocket::request_type& req) {
            req.set(::net::http::field::cookie, auth_cookie);
        }));
        ws.handshake("127.0.0.1", "/ws/session");
        ws.text(true);

        auto send_ws = [&](Json body) {
            ws.write(asio::buffer(body.dump()));
            beast::flat_buffer buffer;
            ws.read(buffer);
            return Json::parse(beast::buffers_to_string(buffer.data()));
        };

        auto started = send_ws(Json{
            {"type", "document.upload.start"},
            {"traceId", "trace-ws-upload-start"},
            {"payload", {
                {"fileName", "ws-upload.docx"},
                {"totalBytes", file_bytes.size()},
                {"sessionId", "ws-upload-session"},
            }},
        });
        ASSERT_EQ(started["type"], "document.upload.started");
        const auto upload_id = started["payload"]["uploadId"].get<std::string>();

        auto ack = send_ws(Json{
            {"type", "document.upload.chunk"},
            {"traceId", "trace-ws-upload-chunk"},
            {"payload", {
                {"uploadId", upload_id},
                {"offset", 0},
                {"data", Base64Encode(file_bytes)},
            }},
        });
        ASSERT_EQ(ack["type"], "document.upload.chunk_ack");
        EXPECT_EQ(ack["payload"]["receivedBytes"], file_bytes.size());

        auto finished = send_ws(Json{
            {"type", "document.upload.finish"},
            {"traceId", "trace-ws-upload-finish"},
            {"payload", {
                {"uploadId", upload_id},
            }},
        });
        ASSERT_EQ(finished["type"], "document.upload.finished");
        ASSERT_TRUE(finished["payload"]["ok"].get<bool>());
        const auto document_id = finished["payload"]["documentId"].get<std::string>();
        EXPECT_EQ(finished["payload"]["data"]["fileName"], "ws-upload.docx");
        EXPECT_EQ(finished["payload"]["data"]["sessionId"], "ws-upload-session");

        beast::error_code ws_ec;
        ws.close(beast::websocket::close_code::normal, ws_ec);

        auto analyzed = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/document/analyze",
            Json{
                {"traceId", "trace-ws-upload-analyze"},
                {"documentId", document_id},
            },
            {{"Cookie", auth_cookie}});
        ASSERT_EQ(analyzed.result(), ::net::http::status::ok);
        auto analyzed_body = Json::parse(analyzed.body());
        EXPECT_TRUE(analyzed_body["ok"].get<bool>());
        EXPECT_EQ(analyzed_body["data"]["fileName"], "ws-upload.docx");

        server.Stop();
    }

    std::filesystem::remove(source_path, ec);
    std::filesystem::remove_all(store_root, ec);
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
}

TEST(PersonaGatewayServerTest, UploadsBinaryDocumentOverWebSocketAndAnalyzesByDocumentId) {
    const auto source_path = TempPath("agent_gateway_server_ws_binary_upload.docx");
    const auto db_path = TempPath("agent_gateway_server_ws_binary_upload.sqlite");
    const auto auth_db_path = TempPath("agent_gateway_server_ws_binary_upload_auth.sqlite");
    const auto store_root = TempPath("agent_gateway_server_ws_binary_upload_store");
    WriteMinimalDocx(source_path);
    const auto file_bytes = ReadBinaryFile(source_path);
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
    std::filesystem::remove_all(store_root, ec);

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();

    PersonaGatewayServerOptions options;
    options.http.address = "127.0.0.1";
    options.http.port = 0;
    options.http.io_threads = 1;
    options.websocket_path = "/ws/session";
    options.compute_pool.worker_count = 1;
    options.compute_pool.queue_capacity = 64;
    options.io_pool.worker_count = 1;
    options.io_pool.queue_capacity = 64;
    options.runtime.default_model = "test-model";
    options.document_store.enabled = true;
    options.document_store.root = store_root;
    options.document_store.database_path = db_path;
    options.document_store.read_connection_count = 1;
    options.document_store.write_connection_count = 1;
    options.document_store.busy_timeout_ms = 250;
    EnableGatewayTestAuth(options, auth_db_path);

    PersonaGatewayServerDependencies dependencies;
    dependencies.memory_provider = memory;
    dependencies.emotion_analyzer = emotion;
    dependencies.llm_client = llm;

    {
        PersonaGatewayServer server(std::move(options), std::move(dependencies));
        ASSERT_TRUE(server.Start().ok());
        const auto auth_cookie = RegisterDevAuthCookie(server.port(), "ws-binary-doc-user-001");

        asio::io_context io;
        tcp::resolver resolver(io);
        beast::websocket::stream<tcp::socket> ws(io);
        auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
        asio::connect(ws.next_layer(), endpoints);
        ws.set_option(beast::websocket::stream_base::decorator([&auth_cookie](beast::websocket::request_type& req) {
            req.set(::net::http::field::cookie, auth_cookie);
        }));
        ws.handshake("127.0.0.1", "/ws/session");

        auto read_json = [&]() {
            beast::flat_buffer buffer;
            ws.read(buffer);
            EXPECT_TRUE(ws.got_text());
            return Json::parse(beast::buffers_to_string(buffer.data()));
        };

        ws.text(true);
        ws.write(asio::buffer(Json{
            {"type", "document.upload.start"},
            {"traceId", "trace-ws-binary-start"},
            {"payload", {
                {"fileName", "ws-binary-upload.docx"},
                {"totalBytes", file_bytes.size()},
                {"mode", "binary"},
            }},
        }.dump()));
        auto started = read_json();
        ASSERT_EQ(started["type"], "document.upload.started");
        ASSERT_EQ(started["payload"]["mode"], "binary");
        const auto upload_id = started["payload"]["uploadId"].get<std::string>();

        ws.binary(true);
        ws.write(asio::buffer(file_bytes));
        auto ack = read_json();
        ASSERT_EQ(ack["type"], "document.upload.chunk_ack");
        EXPECT_EQ(ack["payload"]["mode"], "binary");
        EXPECT_EQ(ack["payload"]["uploadId"], upload_id);
        EXPECT_EQ(ack["payload"]["receivedBytes"], file_bytes.size());

        ws.text(true);
        ws.write(asio::buffer(Json{
            {"type", "document.upload.finish"},
            {"traceId", "trace-ws-binary-finish"},
            {"payload", {
                {"uploadId", upload_id},
            }},
        }.dump()));
        auto finished = read_json();
        ASSERT_EQ(finished["type"], "document.upload.finished");
        ASSERT_TRUE(finished["payload"]["ok"].get<bool>());
        const auto document_id = finished["payload"]["documentId"].get<std::string>();
        EXPECT_EQ(finished["payload"]["data"]["fileName"], "ws-binary-upload.docx");

        beast::error_code ws_ec;
        ws.close(beast::websocket::close_code::normal, ws_ec);

        auto analyzed = SendJsonRequest(
            server.port(),
            ::net::http::verb::post,
            "/api/document/analyze",
            Json{
                {"traceId", "trace-ws-binary-analyze"},
                {"documentId", document_id},
            },
            {{"Cookie", auth_cookie}});
        ASSERT_EQ(analyzed.result(), ::net::http::status::ok);
        auto analyzed_body = Json::parse(analyzed.body());
        EXPECT_TRUE(analyzed_body["ok"].get<bool>());
        EXPECT_EQ(analyzed_body["data"]["fileName"], "ws-binary-upload.docx");

        server.Stop();
    }

    std::filesystem::remove(source_path, ec);
    std::filesystem::remove_all(store_root, ec);
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
}

TEST(PersonaGatewayServerTest, CleansUnfinishedBinaryDocumentUploadWhenWebSocketCloses) {
    const auto source_path = TempPath("agent_gateway_server_ws_binary_upload_cleanup.docx");
    const auto db_path = TempPath("agent_gateway_server_ws_binary_upload_cleanup.sqlite");
    const auto auth_db_path = TempPath("agent_gateway_server_ws_binary_upload_cleanup_auth.sqlite");
    const auto store_root = TempPath("agent_gateway_server_ws_binary_upload_cleanup_store");
    WriteMinimalDocx(source_path);
    const auto file_bytes = ReadBinaryFile(source_path);
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
    std::filesystem::remove_all(store_root, ec);

    auto cache = std::make_shared<FakeSemanticCache>();
    auto memory = std::make_shared<SemanticMemoryContextProvider>(cache);
    auto emotion = std::make_shared<NeutralEmotionAnalyzer>();
    auto llm = std::make_shared<FakeLlmClient>();

    PersonaGatewayServerOptions options;
    options.http.address = "127.0.0.1";
    options.http.port = 0;
    options.http.io_threads = 1;
    options.websocket_path = "/ws/session";
    options.compute_pool.worker_count = 1;
    options.compute_pool.queue_capacity = 64;
    options.io_pool.worker_count = 1;
    options.io_pool.queue_capacity = 64;
    options.runtime.default_model = "test-model";
    options.document_store.enabled = true;
    options.document_store.root = store_root;
    options.document_store.database_path = db_path;
    options.document_store.read_connection_count = 1;
    options.document_store.write_connection_count = 1;
    options.document_store.busy_timeout_ms = 250;
    EnableGatewayTestAuth(options, auth_db_path);

    PersonaGatewayServerDependencies dependencies;
    dependencies.memory_provider = memory;
    dependencies.emotion_analyzer = emotion;
    dependencies.llm_client = llm;

    {
        PersonaGatewayServer server(std::move(options), std::move(dependencies));
        ASSERT_TRUE(server.Start().ok());
        const auto auth_cookie = RegisterDevAuthCookie(server.port(), "ws-binary-cleanup-user-001");

        asio::io_context io;
        tcp::resolver resolver(io);
        beast::websocket::stream<tcp::socket> ws(io);
        auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()));
        asio::connect(ws.next_layer(), endpoints);
        ws.set_option(beast::websocket::stream_base::decorator([&auth_cookie](beast::websocket::request_type& req) {
            req.set(::net::http::field::cookie, auth_cookie);
        }));
        ws.handshake("127.0.0.1", "/ws/session");

        auto read_json = [&]() {
            beast::flat_buffer buffer;
            ws.read(buffer);
            EXPECT_TRUE(ws.got_text());
            return Json::parse(beast::buffers_to_string(buffer.data()));
        };

        ws.text(true);
        ws.write(asio::buffer(Json{
                {"type", "document.upload.start"},
                {"traceId", "trace-ws-binary-cleanup-start"},
                {"payload", {
                    {"fileName", "ws-binary-cleanup.docx"},
                    {"totalBytes", file_bytes.size()},
                    {"mode", "binary"},
                }},
        }.dump()));
        auto started = read_json();
        ASSERT_EQ(started["type"], "document.upload.started");
        const auto upload_id = started["payload"]["uploadId"].get<std::string>();
        const auto temp_path = UploadTempPathForTest(upload_id);

        ws.binary(true);
        ws.write(asio::buffer(file_bytes));
        auto ack = read_json();
        ASSERT_EQ(ack["type"], "document.upload.chunk_ack");
        ASSERT_TRUE(FileExistsForTest(temp_path));

        beast::error_code ws_ec;
        auto& socket = beast::get_lowest_layer(ws);
        socket.shutdown(tcp::socket::shutdown_both, ws_ec);
        socket.close(ws_ec);

        for (int attempt = 0; attempt < 20 && FileExistsForTest(temp_path); ++attempt) {
            std::this_thread::sleep_for(10ms);
        }
        EXPECT_FALSE(FileExistsForTest(temp_path));

        server.Stop();
    }

    std::filesystem::remove(source_path, ec);
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);
    std::filesystem::remove(auth_db_path, ec);
    std::filesystem::remove(auth_db_path.string() + "-wal", ec);
    std::filesystem::remove(auth_db_path.string() + "-shm", ec);
    std::filesystem::remove_all(store_root, ec);
}

TEST(PersonaMetadataStoreTest, RedisCacheReadsThroughFromSqliteWhenAvailable) {
    agent::semantic_cache::RedisPoolOptions options;
    options.host = "127.0.0.1";
    options.port = "5000";
    options.pool_size = 1;
    options.command_timeout = std::chrono::milliseconds(1000);
    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(options);
    auto start = redis->Start();
    if (!start.ok()) {
        GTEST_SKIP() << "redis connection failed: " << start.message();
    }

    const auto path = std::filesystem::temp_directory_path() / "agent_gateway_persona_metadata_cache_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string prefix = "agent:test:gateway:persona:" + std::to_string(nonce);
    auto keys = redis->Scan(prefix + "*");
    if (keys.ok() && !keys.value().empty()) {
        static_cast<void>(redis->Del(keys.value()));
    }

    auto primary = std::make_shared<SqlitePersonaMetadataStore>(path.string());
    auto cache = std::make_shared<RedisPersonaMetadataCache>(redis, prefix);
    CachedPersonaMetadataStore store(primary, cache);
    ASSERT_TRUE(store.EnsureSchema().ok());

    auto req = MakePersonaMetadataRequest("persona-cache-owner", "dazhi", "cached sqlite persona");
    agent::service::gateway::PersonaMetadataRecord record{
        req.tenant_id,
        req.user_uuid,
        req.persona_id,
        req.personality,
        req.emotion_prompt_config,
        req.emotion_state_config,
    };
    ASSERT_TRUE(primary->Upsert(record).ok());

    auto loaded = store.Get("default", "persona-cache-owner", "dazhi");
    ASSERT_TRUE(loaded.ok()) << loaded.status().message();
    EXPECT_EQ(loaded.value().personality.description, "cached sqlite persona");

    auto cached = cache->Get("default", "persona-cache-owner", "dazhi");
    ASSERT_TRUE(cached.ok()) << cached.status().message();
    EXPECT_EQ(cached.value().personality.description, "cached sqlite persona");

    keys = redis->Scan(prefix + "*");
    if (keys.ok() && !keys.value().empty()) {
        static_cast<void>(redis->Del(keys.value()));
    }
    redis->Shutdown();
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

} // namespace

