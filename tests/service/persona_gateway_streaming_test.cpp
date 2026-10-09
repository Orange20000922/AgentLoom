#include "persona_gateway_server.h"
#include "async_beast_http_client.h"
#include "beast_http_client.h"
#include "openai_llm_client.h"
#include "sse.h"
#include "trace_context.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <future>

namespace {
namespace gateway = agent::service::gateway;
namespace persona = agent::service::persona;
namespace llm = agent::llm;
namespace outbound = agent::net;
using Json = nlohmann::json;
using namespace std::chrono_literals;

class TestMemory final : public persona::IMemoryContextProvider {
public:
    core::Result<persona::RecalledContext> BuildContext(const persona::MemoryContextRequest&) override { return persona::RecalledContext{}; }
    core::Status AdmitTurn(std::string_view, std::string_view, std::string_view,
                           const persona::ConversationTurn&, std::string_view) override {
        ++admissions;
        return fail_admission ? core::Status::Error(core::ErrorCode::Unavailable, "test admission failure") : core::Status::Ok();
    }
    std::atomic<int> admissions{0};
    bool fail_admission = false;
};

class TestEmotion final : public persona::IEmotionAnalyzer {
public:
    core::Result<persona::EmotionAnalysis> Analyze(std::string_view text, std::string_view,
                                                  std::shared_ptr<const persona::PersonalityConfig> personality) override {
        if (fail_ai && text.starts_with("first"))
            return core::Status::Error(core::ErrorCode::Unavailable, "test AI emotion failure");
        return neutral.Analyze(text, {}, std::move(personality));
    }
    bool fail_ai = false;
    persona::NeutralEmotionAnalyzer neutral;
};

class ControlledProvider {
public:
    ControlledProvider() : server_(Options()) {
        server_.SetHttpRequestHandler([this](auto request) {
            auto opened = request->BeginEventStream({}, [this](auto status) {
                if (!status.ok()) ++disconnects;
                changed_.notify_all();
            });
            EXPECT_TRUE(opened.ok());
            if (!opened.ok()) return;
            std::size_t index;
            {
                std::lock_guard lock(mutex_);
                index = streams_.size();
                streams_.push_back(opened.value());
            }
            EXPECT_TRUE(opened.value()->SendEvent({{}, Delta(index, "first"), {}}).ok());
            changed_.notify_all();
        });
        EXPECT_TRUE(server_.Start().ok());
    }
    ~ControlledProvider() { server_.Stop(); }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(server_.port()); }
    bool Wait(std::size_t count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return streams_.size() >= count; });
    }
    bool WaitDisconnected() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return disconnects.load() > 0; });
    }
    void Finish(std::size_t index) {
        std::shared_ptr<::net::IServerEventStream> stream;
        { std::lock_guard lock(mutex_); stream = streams_.at(index); }
        EXPECT_TRUE(stream->SendEvent({{}, Delta(index, " last", "stop"), {}}).ok());
        EXPECT_TRUE(stream->SendEvent({{}, R"({"choices":[],"usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})", {}}).ok());
        EXPECT_TRUE(stream->SendEvent({{}, "[DONE]", {}}).ok());
        stream->FinishEvents();
    }
    std::atomic<int> disconnects{0};
private:
    static ::net::HttpServerOptions Options() {
        ::net::HttpServerOptions options;
        options.address = "127.0.0.1"; options.port = 0; options.io_threads = 2;
        return options;
    }
    static std::string Delta(std::size_t index, std::string text, Json finish = nullptr) {
        return Json{{"id", "provider-" + std::to_string(index)}, {"model", "fake"},
            {"choices", Json::array({{{"index", 0}, {"delta", {{"content", text}}}, {"finish_reason", finish}}})}}.dump();
    }
    ::net::HttpServer server_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::shared_ptr<::net::IServerEventStream>> streams_;
};

struct Reader : std::enable_shared_from_this<Reader> {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<outbound::SseEvent> events;
    std::optional<outbound::SseDecoder> decoder;
    std::promise<core::Status> completion;
    std::future<core::Status> done{completion.get_future()};
    std::shared_ptr<outbound::IAsyncHttpOperation> operation;
    bool Wait(std::string_view name) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return std::any_of(events.begin(), events.end(),
            [name](const auto& event) { return event.event == name; }); });
    }
    std::vector<outbound::SseEvent> Events() { std::lock_guard lock(mutex); return events; }
};

class GatewayStreamingTest : public testing::Test {
protected:
    void SetUp() override {
        auto sync = outbound::BeastHttpClient::Create({});
        ASSERT_TRUE(sync.ok());
        sync_http_ = std::move(sync).value();
        outbound::AsyncBeastHttpClientOptions http_options;
        http_options.io_thread_count = 3;
        auto async = outbound::AsyncBeastHttpClient::Create(http_options);
        ASSERT_TRUE(async.ok());
        async_http_ = std::move(async).value();
        llm::OpenAiLlmClientOptions llm_options;
        llm_options.base_url = provider_.Url(); llm_options.require_api_key = false; llm_options.default_model = "fake";
        auto sync_llm = llm::OpenAiLlmClient::Create(llm_options, *sync_http_);
        auto async_llm = llm::OpenAiAsyncLlmClient::Create(llm_options, *async_http_);
        ASSERT_TRUE(sync_llm.ok()); ASSERT_TRUE(async_llm.ok());
        gateway::PersonaGatewayServerDependencies dependencies;
        dependencies.llm_client = std::move(sync_llm).value();
        dependencies.async_llm_client = std::move(async_llm).value();
        dependencies.memory_provider = memory_;
        dependencies.emotion_analyzer = emotion_;
        gateway::PersonaGatewayServerOptions options;
        options.http.address = "127.0.0.1"; options.http.port = 0; options.http.io_threads = 2;
        options.compute_pool.worker_count = 2; options.compute_pool.queue_capacity = 32;
        options.io_pool.worker_count = 2; options.io_pool.queue_capacity = 32;
        options.runtime.default_model = "fake";
        options.streaming.transport = stream_options_;
        auto keys = gateway::GenerateDevelopmentRsaKeyPair();
        ASSERT_TRUE(keys.ok());
        options.auth.enabled = true; options.auth.require_auth_for_api = true; options.auth.allow_dev_identity = false;
        options.auth.enable_dev_registration = true;
        options.auth.public_key_pem = keys.value().public_key_pem;
        options.auth.private_key_pem = keys.value().private_key_pem;
        std::filesystem::create_directories("build/stream-tests");
        database_ = std::filesystem::path("build/stream-tests") / (core::GenerateTraceId() + ".sqlite");
        options.auth.session_database_path = database_.string();
        gateway::PersonaMetadataRecord metadata;
        metadata.user_uuid = "stream-user"; metadata.persona_id = "stream-persona"; metadata.personality.name = "stream-persona";
        options.default_personas.push_back(metadata);
        server_ = std::make_unique<gateway::PersonaGatewayServer>(options, std::move(dependencies));
        ASSERT_TRUE(server_->Start().ok());
        auto auth = Post("/api/auth/register", {{"userUuid", "stream-user"}, {"ttlSeconds", 60}});
        ASSERT_TRUE(auth.ok()); ASSERT_EQ(auth.value().status, 200);
        for (const auto& header : auth.value().headers) if (header.name == "Set-Cookie") cookie_ = header.value;
        ASSERT_FALSE(cookie_.empty());
        auto created = Post("/api/session/create", {{"sessionId", "stream-session"}, {"personaId", "stream-persona"}});
        ASSERT_TRUE(created.ok()); ASSERT_EQ(created.value().status, 200) << created.value().body;
    }
    void TearDown() override {
        if (server_) { server_->Stop(); server_.reset(); }
        if (async_http_) async_http_->Shutdown();
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::error_code ignored;
            if (!database_.empty()) std::filesystem::remove(database_.string() + suffix, ignored);
        }
    }
    std::string Url(std::string_view path) const { return "http://127.0.0.1:" + std::to_string(server_->port()) + std::string(path); }
    core::Result<outbound::HttpClientResponse> Post(std::string_view path, Json body) {
        outbound::HttpClientRequest request;
        request.url = Url(path); request.body = body.dump(); request.timeout_ms = 5000;
        request.headers = {{"Content-Type", "application/json"}};
        if (!cookie_.empty()) request.headers.push_back({"Cookie", cookie_});
        return sync_http_->Execute(request);
    }
    std::shared_ptr<Reader> Read(std::string trace, std::string last_id = {}, bool resume = false) {
        auto reader = std::make_shared<Reader>();
        std::weak_ptr<Reader> weak = reader;
        reader->decoder.emplace([weak](const auto& event) {
            if (auto value = weak.lock()) {
                { std::lock_guard lock(value->mutex); value->events.push_back(event); }
                value->changed.notify_all();
            }
            return core::Status::Ok();
        });
        outbound::HttpClientRequest request;
        request.url = Url(resume ? "/api/chat/stream/" + trace : "/api/chat/message");
        request.method = resume ? "GET" : "POST";
        request.headers = {{"Cookie", cookie_}, {"Content-Type", "application/json"}, {"X-Trace-Id", trace}};
        if (!last_id.empty()) request.headers.push_back({"Last-Event-ID", last_id});
        if (!resume) request.body = Json{{"sessionId", "stream-session"}, {"message", "say hello"}, {"stream", true}}.dump();
        auto submitted = async_http_->ExecuteStreamingAsync(request, {}, {
            .on_headers = [](const auto& headers) {
                return headers.status == 200 ? core::Status::Ok()
                    : core::Status::Error(core::ErrorCode::Unavailable, "unexpected gateway HTTP status");
            },
            .on_body = [reader](auto bytes) { return reader->decoder->Feed(bytes); },
            .on_complete = [reader](auto status) { reader->completion.set_value(status); },
        });
        EXPECT_TRUE(submitted.ok());
        if (submitted.ok()) reader->operation = submitted.value();
        return reader;
    }
    ControlledProvider provider_;
    std::shared_ptr<TestMemory> memory_ = std::make_shared<TestMemory>();
    std::shared_ptr<TestEmotion> emotion_ = std::make_shared<TestEmotion>();
    std::unique_ptr<outbound::BeastHttpClient> sync_http_;
    std::unique_ptr<outbound::AsyncBeastHttpClient> async_http_;
    std::unique_ptr<gateway::PersonaGatewayServer> server_;
    std::filesystem::path database_;
    std::string cookie_;
    ::net::SseStreamOptions stream_options_;
};

TEST_F(GatewayStreamingTest, VisibleDeltaPrecedesSingleCommitAndSameSessionRemainsOrdered) {
    auto first = Read("stream-first");
    ASSERT_TRUE(provider_.Wait(1));
    ASSERT_TRUE(first->Wait("TextDelta"));
    EXPECT_EQ(memory_->admissions.load(), 0);
    auto second = Read("stream-second");
    EXPECT_FALSE(provider_.Wait(2, 50ms));
    provider_.Finish(0);
    ASSERT_EQ(first->done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(first->done.get().ok());
    ASSERT_TRUE(provider_.Wait(2));
    EXPECT_EQ(memory_->admissions.load(), 1);
    provider_.Finish(1);
    ASSERT_EQ(second->done.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(second->done.get().ok());
    EXPECT_EQ(memory_->admissions.load(), 2);
    const auto events = first->Events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().event, "TurnCompleted");
    std::uint64_t sequence = 0;
    for (const auto& event : events) {
        const auto data = Json::parse(event.data);
        EXPECT_EQ(data.at("requestId"), "stream-first");
        EXPECT_EQ(data.at("sequence").get<std::uint64_t>(), ++sequence);
    }
    EXPECT_TRUE(Json::parse(events.back().data).at("committed").get<bool>());
}

TEST_F(GatewayStreamingTest, DisconnectCancelsAndReconnectReplaysFailureWithoutNewProviderCall) {
    auto first = Read("stream-cancel");
    ASSERT_TRUE(provider_.Wait(1));
    ASSERT_TRUE(first->Wait("TextDelta"));
    const auto cursor = first->Events().back().id;
    const auto cancelled_at = std::chrono::steady_clock::now();
    first->operation->Cancel();
    ASSERT_EQ(first->done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(provider_.WaitDisconnected());
    RecordProperty("cancel_close_ms", std::to_string(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cancelled_at).count()));
    // 上游断连说明取消已传递；重连只读取已保留事件与取消终态。
    auto channel = server_->service().ChatStreams().Find("stream-cancel", "stream-user");
    ASSERT_TRUE(channel.ok());
    auto resumed = Read("stream-cancel", cursor, true);
    ASSERT_EQ(resumed->done.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(resumed->done.get().ok());
    const auto events = resumed->Events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().event, "TurnFailed");
    EXPECT_EQ(Json::parse(events.back().data).at("error").at("code"), "CANCELLED");
    EXPECT_EQ(memory_->admissions.load(), 0);
    EXPECT_FALSE(provider_.Wait(2, 50ms));
}

TEST_F(GatewayStreamingTest, AiPostprocessingFailureHasUncommittedTerminalAfterVisibleDelta) {
    emotion_->fail_ai = true;
    auto reader = Read("stream-postprocess");
    ASSERT_TRUE(provider_.Wait(1));
    ASSERT_TRUE(reader->Wait("TextDelta"));
    provider_.Finish(0);
    ASSERT_EQ(reader->done.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(reader->done.get().ok());
    auto events = reader->Events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().event, "TurnFailed");
    EXPECT_FALSE(Json::parse(events.back().data).at("committed").get<bool>());
    EXPECT_EQ(memory_->admissions.load(), 0);
}

TEST_F(GatewayStreamingTest, MemoryAdmissionFailurePreservesSelectedAsyncCommitPolicy) {
    memory_->fail_admission = true;
    auto reader = Read("stream-admission");
    ASSERT_TRUE(provider_.Wait(1));
    provider_.Finish(0);
    ASSERT_EQ(reader->done.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(reader->done.get().ok());
    const auto events = reader->Events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().event, "TurnCompleted");
    EXPECT_EQ(memory_->admissions.load(), 1);
}

class GatewayStreamingBackpressureTest : public GatewayStreamingTest {
protected:
    void SetUp() override {
        stream_options_.outbound = {1, 128};
        GatewayStreamingTest::SetUp();
    }
};

TEST_F(GatewayStreamingBackpressureTest, QueueOverflowCancelsProviderAndRetainsResourceExhaustedTerminal) {
    auto reader = Read("stream-backpressure");
    ASSERT_TRUE(provider_.Wait(1));
    ASSERT_TRUE(provider_.WaitDisconnected());
    ASSERT_EQ(reader->done.wait_for(3s), std::future_status::ready);
    // 原 socket 已关闭，终态通过有界重放缓存检查；失败不能提交到 Memory。
    EXPECT_EQ(memory_->admissions.load(), 0);
    auto channel = server_->service().ChatStreams().Find("stream-backpressure", "stream-user");
    ASSERT_TRUE(channel.ok());
    EXPECT_EQ(channel.value()->DeliveryStatus().code(), core::ErrorCode::ResourceExhausted);
    EXPECT_FALSE(provider_.Wait(2, 50ms));
}
}
