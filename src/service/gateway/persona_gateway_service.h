#pragma once
#include "chat_sse_registry.h"

#include "classroom_scheduler.h"
#include "gateway_models.h"
#include "logger_adapter.h"
#include "persona_runtime.h"
#include "persona_interaction.h"
#include "report_evaluator.h"
#include "session_manager.h"
#include "sqlite/sqlite_migration.h"

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

namespace storage::sqlite {
class SqliteConnectionPool;
}

namespace agent::service::gateway {

class IPersonaMetadataStore {
public:
    virtual ~IPersonaMetadataStore() = default;
    virtual core::Status EnsureSchema() = 0;
    virtual core::Status Upsert(PersonaMetadataRecord record) = 0;
    virtual core::Result<PersonaMetadataRecord> Get(std::string_view tenant_id,
                                                    std::string_view user_uuid,
                                                    std::string_view persona_id) const = 0;
    virtual core::Result<std::vector<PersonaMetadataRecord>> ListByAccount(std::string_view tenant_id,
                                                                           std::string_view user_uuid) const = 0;
};

class InMemoryPersonaMetadataStore final : public IPersonaMetadataStore {
public:
    core::Status EnsureSchema() override;
    core::Status Upsert(PersonaMetadataRecord record) override;
    core::Result<PersonaMetadataRecord> Get(std::string_view tenant_id,
                                            std::string_view user_uuid,
                                            std::string_view persona_id) const override;
    core::Result<std::vector<PersonaMetadataRecord>> ListByAccount(std::string_view tenant_id,
                                                                   std::string_view user_uuid) const override;

private:
    static std::string Key(std::string_view tenant_id, std::string_view user_uuid, std::string_view persona_id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PersonaMetadataRecord> records_;
};

class ServerDefaultPersonaMetadataStore final : public IPersonaMetadataStore {
public:
    explicit ServerDefaultPersonaMetadataStore(std::vector<PersonaMetadataRecord> records = {});

    core::Status EnsureSchema() override;
    core::Status Upsert(PersonaMetadataRecord record) override;
    core::Result<PersonaMetadataRecord> Get(std::string_view tenant_id,
                                            std::string_view user_uuid,
                                            std::string_view persona_id) const override;
    core::Result<std::vector<PersonaMetadataRecord>> ListByAccount(std::string_view tenant_id,
                                                                   std::string_view user_uuid) const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PersonaMetadataRecord> records_;
};

class OverlayPersonaMetadataStore final : public IPersonaMetadataStore {
public:
    OverlayPersonaMetadataStore(std::shared_ptr<IPersonaMetadataStore> defaults,
                                std::shared_ptr<IPersonaMetadataStore> account);

    core::Status EnsureSchema() override;
    core::Status Upsert(PersonaMetadataRecord record) override;
    core::Result<PersonaMetadataRecord> Get(std::string_view tenant_id,
                                            std::string_view user_uuid,
                                            std::string_view persona_id) const override;
    core::Result<std::vector<PersonaMetadataRecord>> ListByAccount(std::string_view tenant_id,
                                                                   std::string_view user_uuid) const override;

private:
    std::shared_ptr<IPersonaMetadataStore> defaults_;
    std::shared_ptr<IPersonaMetadataStore> account_;
};

class SqlitePersonaMetadataStore final
    : public IPersonaMetadataStore,
      public storage::sqlite::ISqliteMigrationSource {
public:
    explicit SqlitePersonaMetadataStore(std::string database_path);
    explicit SqlitePersonaMetadataStore(std::shared_ptr<storage::sqlite::SqliteConnectionPool> pool);

    core::Status EnsureSchema() override;
    core::Status Upsert(PersonaMetadataRecord record) override;
    core::Result<PersonaMetadataRecord> Get(std::string_view tenant_id,
                                            std::string_view user_uuid,
                                            std::string_view persona_id) const override;
    core::Result<std::vector<PersonaMetadataRecord>> ListByAccount(std::string_view tenant_id,
                                                                   std::string_view user_uuid) const override;

    std::string_view MigrationNamespace() const noexcept override;
    std::span<const storage::sqlite::SqliteMigrationStep>
    MigrationSteps() const noexcept override;

private:
    std::shared_ptr<storage::sqlite::SqliteConnectionPool> pool_;
};

class RedisPersonaMetadataCache final : public IPersonaMetadataStore {
public:
    RedisPersonaMetadataCache(std::shared_ptr<semantic_cache::RedisConnectionPool> redis,
                              std::string key_prefix = "agent:gateway:persona");

    core::Status EnsureSchema() override;
    core::Status Upsert(PersonaMetadataRecord record) override;
    core::Result<PersonaMetadataRecord> Get(std::string_view tenant_id,
                                            std::string_view user_uuid,
                                            std::string_view persona_id) const override;
    core::Result<std::vector<PersonaMetadataRecord>> ListByAccount(std::string_view tenant_id,
                                                                   std::string_view user_uuid) const override;

private:
    std::string Key(std::string_view tenant_id, std::string_view user_uuid, std::string_view persona_id) const;

    std::shared_ptr<semantic_cache::RedisConnectionPool> redis_;
    std::string key_prefix_;
};

class CachedPersonaMetadataStore final : public IPersonaMetadataStore {
public:
    CachedPersonaMetadataStore(std::shared_ptr<IPersonaMetadataStore> primary,
                               std::shared_ptr<IPersonaMetadataStore> cache);

    core::Status EnsureSchema() override;
    core::Status Upsert(PersonaMetadataRecord record) override;
    core::Result<PersonaMetadataRecord> Get(std::string_view tenant_id,
                                            std::string_view user_uuid,
                                            std::string_view persona_id) const override;
    core::Result<std::vector<PersonaMetadataRecord>> ListByAccount(std::string_view tenant_id,
                                                                   std::string_view user_uuid) const override;

private:
    std::shared_ptr<IPersonaMetadataStore> primary_;
    std::shared_ptr<IPersonaMetadataStore> cache_;
};

class PersonaGatewayService {
public:
    using ChatCallback = std::function<void(core::Result<ChatGatewayResponse>)>;
    using ClassroomCallback = std::function<void(core::Result<ClassroomGatewayResponse>)>;

    PersonaGatewayService(persona::SessionManager& sessions,
                          persona::PersonaRuntime& runtime,
                          IClassroomScheduler* classroom_scheduler = nullptr,
                          std::shared_ptr<IReportEvaluator> report_evaluator = nullptr,
                          std::shared_ptr<IPersonaMetadataStore> persona_metadata_store = nullptr,
                          core::LoggerAdapter logger = core::LoggerAdapter::ForModule("service"),
                          GatewayStreamingOptions streaming = {});

    core::Result<PersonaMetadataGatewayResponse> UpsertPersonaMetadata(PersonaMetadataGatewayRequest request);
    core::Result<PersonaMetadataGatewayResponse> GetPersonaMetadata(std::string_view tenant_id,
                                                                    std::string_view user_uuid,
                                                                    std::string_view persona_id,
                                                                    std::string trace_id);
    core::Result<SessionGatewayResponse> CreateSession(CreateSessionGatewayRequest request);
    core::Result<SessionGatewayResponse> GetSession(std::string_view session_id,
                                                    std::string trace_id,
                                                    std::string_view authenticated_user_uuid = {});
    core::Result<SessionGatewayResponse> CloseSession(CloseSessionGatewayRequest request);
    core::Result<ChatGatewayResponse> Chat(ChatGatewayRequest request);
    core::Result<ClassroomGatewayResponse> ClassroomMessage(ClassroomMessageGatewayRequest request);
    core::Result<ClassroomGatewayResponse> ClassroomProactive(ClassroomProactiveGatewayRequest request);
    core::Result<ClassroomGatewayResponse> ClassroomPoll(ClassroomPollGatewayRequest request);
    core::Result<TrainingReportGatewayResponse> TrainingReport(TrainingReportGatewayRequest request);
    core::Result<SystemStatsGatewayResponse> SystemStats(std::string trace_id);
    core::Result<std::vector<PersonaMetadataRecord>> ListPersonaMetadataByAccount(std::string_view tenant_id,
                                                              std::string_view user_uuid,
                                                              std::string trace_id);
    core::Status SubmitChat(ChatGatewayRequest request, ChatCallback callback);
    bool SupportsStreaming() const noexcept { return runtime_.SupportsStreaming(); }
    IChatSseRegistry& ChatStreams() noexcept { return chat_streams_; }
    const ::net::SseStreamOptions& StreamOptions() const noexcept { return streaming_.transport; }
    core::Status CancelChat(std::string session_id, std::string owner, std::string trace_id) {
        return interaction_.CancelTurn({std::move(session_id), std::move(trace_id), std::move(owner)});
    }
    core::Status SubmitClassroomMessage(ClassroomMessageGatewayRequest request, ClassroomCallback callback);
    core::Status SubmitClassroomProactive(ClassroomProactiveGatewayRequest request, ClassroomCallback callback);
    core::Status SubmitClassroomPoll(ClassroomPollGatewayRequest request, ClassroomCallback callback);

private:
    core::Result<ChatGatewayResponse> ToChatGatewayResponse(const ChatGatewayRequest& request,
                                                            const persona::ChatResponse& result,
                                                            std::chrono::steady_clock::time_point started);
    ClassroomGatewayResponse ToClassroomGatewayResponse(const ChatGatewayResponse& result,
                                                        std::string classroom_id,
                                                        bool should_speak = true) const;
    core::Result<ClassroomRouteResult> ResolveClassroomRoute(const ClassroomMessageGatewayRequest& request);
    core::Result<ClassroomRouteResult> ResolveClassroomRoute(const ClassroomProactiveGatewayRequest& request);
    ClassroomPersonaRegistration BuildClassroomRegistration(const CreateSessionGatewayRequest& request,
                                                            const persona::SessionSnapshot& snapshot) const;
    core::Result<persona::ChatResponse> SubmitChatAndWait(persona::ChatRequest request);
    std::string EnsureTrace(std::string trace_id) const;

    persona::SessionManager& sessions_;
    persona::PersonaRuntime& runtime_;
    persona::PersonaInteraction interaction_;
    IClassroomScheduler* classroom_scheduler_ = nullptr;
    std::shared_ptr<IReportEvaluator> report_evaluator_;
    std::shared_ptr<IPersonaMetadataStore> persona_metadata_store_;
    core::LoggerAdapter logger_;
    GatewayStreamingOptions streaming_;
    ChatSseRegistry chat_streams_;
};

}
