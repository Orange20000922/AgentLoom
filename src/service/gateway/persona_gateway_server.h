#pragma once

#include "persona_gateway_http_adapter.h"
#include "gateway_lifecycle.h"
#include "persona_gateway_service.h"
#include "persona_runtime.h"
#include "runtime_maintenance_service.h"
#include "document_analysis_service.h"
#include "http_server.h"
#include "static_file_handler.h"
#include "thread_pool.h"
#include "../../skill/skill_registry.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent::semantic_cache {
class L0MemoryCacheAdapter;
class RedisConnectionPool;
}

namespace storage::sqlite {
class SqliteConnectionPool;
}

namespace agent::service::gateway {

struct GatewayDocumentStoreOptions {
    bool enabled = false;
    std::filesystem::path root;
    std::filesystem::path database_path;
    std::size_t read_connection_count = 2;
    std::size_t write_connection_count = 1;
    int busy_timeout_ms = 5000;
    int retention_hours = 24 * 7;
    int cleanup_interval_seconds = 60;
    bool enable_path_register_test_endpoint = false;
    bool enable_path_analyze_test_endpoint = false;
};

struct GatewayThreadPoolConcurrencyOptions {
    std::string scheduler = "default_fifo";
    std::size_t max_active_keys = 1024;
    std::size_t max_outstanding_per_key = 8;
    std::size_t max_outstanding_per_fairness_key = 32;
    std::size_t max_outstanding_per_tenant = 256;
};

struct GatewayLlmPoolOptions {
    core::ThreadPoolOptions pool;
    GatewayThreadPoolConcurrencyOptions concurrency{
        .scheduler = "session_affinity",
    };
};

struct GatewayEmbeddingBatchOptions {
    bool enabled = true;
    std::size_t max_pending_requests = 1024;
    std::size_t max_batch_size = 16;
    std::chrono::milliseconds max_batch_wait{2};
    std::size_t max_inflight_batches = 1;
};

struct PersonaGatewayServerOptions {
    using LlmPoolOptions = GatewayLlmPoolOptions;

    ::net::HttpServerOptions http;
    GatewayAuthOptions auth;
    core::ThreadPoolOptions compute_pool;
    GatewayThreadPoolConcurrencyOptions compute_pool_concurrency;
    core::ThreadPoolOptions io_pool;
    GatewayThreadPoolConcurrencyOptions io_pool_concurrency;
    /// 未配置时兼容沿用 IO pool 参数；显式配置后独立控制长时 Persona/LLM Turn 的并发与排队容量。
    std::optional<GatewayLlmPoolOptions> llm_pool;
    GatewayEmbeddingBatchOptions embedding_batch;
    persona::SessionOptions session;
    persona::PersonaRuntimeOptions runtime;
    GatewayStreamingOptions streaming;
    std::optional<::net::StaticFileOptions> static_files;
    GatewayDocumentStoreOptions document_store;
    std::vector<PersonaMetadataRecord> default_personas;
    std::string websocket_path = "/ws/session";
};

struct PersonaGatewayServerDependencies {
    /// 以下 shared_ptr 均由 Server 共享持有；未标为可选的主链路 provider 应在 Start 前配置。
    std::shared_ptr<persona::IMemoryContextProvider> memory_provider;
    std::shared_ptr<persona::IEmotionAnalyzer> emotion_analyzer;
    std::shared_ptr<persona::IToolMemoryProvider> tool_memory_provider;
    std::shared_ptr<persona::ISkillSessionManager> skill_session_manager;
    std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router;
    /// 启动期校验并注册的 Skill manifest；Server 共享其生命周期供下游装配使用。
    std::shared_ptr<agent::skill::ISkillRegistry> skill_registry;
    std::shared_ptr<llm::ILlmClient> llm_client;
    /// 可选的真异步 Persona 主对话客户端；为空时保持同步 ILlmClient 兼容路径。
    std::shared_ptr<llm::IAsyncLlmClient> async_llm_client;
    std::shared_ptr<document::IDocumentEmbeddingProvider> document_embedding_provider;
    std::shared_ptr<document::IDocumentLlmChunkCache> document_llm_chunk_cache;
    std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache;
    std::shared_ptr<semantic_cache::L0MemoryCacheAdapter> l0_memory_adapter;
    std::shared_ptr<IPersonaMetadataStore> persona_metadata_store;
    std::shared_ptr<IReportEvaluator> report_evaluator;
    std::vector<std::shared_ptr<IRuntimeMaintenanceTask>> maintenance_tasks;
    /// 下游注册业务执行器；未提供时使用空工厂，未注册调用回填结构化错误。
    std::shared_ptr<const agent::skill::ISkillExecutorFactory> skill_executor_factory;
    /// 可选的自定义协调器，优先于 registry/factory/session 的默认装配。
    std::shared_ptr<agent::skill::ISkillToolCallCoordinator> skill_tool_coordinator;
};

class PersonaGatewayServer final {
public:
    /// 创建 Gateway 及其线程池、Session Runtime 和协议适配器。
    /// @param options HTTP、认证、线程池、session 和文档存储配置。
    /// @param dependencies 外部 provider 与维护任务，Server 共享持有其所有权。
    /// @param logger Gateway 生命周期和业务失败日志适配器。
    PersonaGatewayServer(PersonaGatewayServerOptions options,
                         PersonaGatewayServerDependencies dependencies,
                         core::LoggerAdapter logger = core::LoggerAdapter::ForModule("gateway"));
    ~PersonaGatewayServer();

    PersonaGatewayServer(const PersonaGatewayServer&) = delete;
    PersonaGatewayServer& operator=(const PersonaGatewayServer&) = delete;

    /// 启动依赖校验、存储、维护任务和 HTTP/WebSocket listener。
    core::Status Start();
    /// 幂等停止 listener、维护任务和文档存储。
    void Stop();
    /// 注册随 Server 启停的维护任务；Server 已启动时按实现规则立即纳入调度。
    /// @param task 共享持有的维护任务，不得为空。
    core::Status RegisterMaintenanceTask(std::shared_ptr<IRuntimeMaintenanceTask> task);

    bool running() const noexcept;
    std::uint16_t port() const noexcept;

    persona::SessionManager& sessions() noexcept;
    PersonaGatewayService& service() noexcept;
    ::net::HttpServer& http_server() noexcept;

private:
    void HandleHttp(std::shared_ptr<::net::IHttpRequest> request);
    void HandleWebSocket(std::shared_ptr<::net::IWebSocketStreamRequest> request);
    void HandleWebSocketClose(const ::net::ConnectionCloseInfo& close_info);
    core::Status ValidateDependencies() const;
    core::Status EnsureGatewayMetadataPool();
    core::Status EnsureAuthSessionStore();
    core::Status EnsurePersonaMetadataStore();
    core::Status EnsureDocumentStore();
    core::Status ConfigureLifecycle();
    void ShutdownDocumentStore();

    PersonaGatewayServerOptions options_;
    PersonaGatewayServerDependencies dependencies_;
    core::LoggerAdapter logger_;
    core::ThreadPool compute_pool_;
    core::ThreadPool io_pool_;
    // 与通用 IO pool 隔离的 Persona/LLM 等待池；其 scheduler 仍复用 Session affinity 配额语义。
    core::ThreadPool llm_pool_;
    persona::SessionManager sessions_;
    persona::PersonaRuntime runtime_;
    ClassroomScheduler classroom_scheduler_;
    /// Auth 与 Persona metadata 共享同一 SQLite pool，保证同一数据库内严格单写。
    std::shared_ptr<storage::sqlite::SqliteConnectionPool> gateway_metadata_pool_;
    std::shared_ptr<agent::semantic_cache::RedisConnectionPool> auth_redis_;
    std::shared_ptr<IAuthSessionStore> auth_session_store_;
    std::shared_ptr<IPersonaMetadataStore> persona_metadata_store_;
    PersonaGatewayService service_;
    std::shared_ptr<document::DocumentAnalysisService> document_service_;
    std::shared_ptr<storage::sqlite::SqliteConnectionPool> document_repository_pool_;
    std::shared_ptr<IGatewayAuthenticator> authenticator_;
    std::shared_ptr<IAuthRegistrationService> auth_registration_;
    PersonaGatewayHttpAdapter adapter_;
    ::net::HttpServer http_server_;
    RuntimeMaintenanceService maintenance_;
    GatewayLifecycleCoordinator lifecycle_;
    std::shared_ptr<::net::StaticFileHandler> static_files_;
    core::Status lifecycle_configuration_status_ = core::Status::Ok();
};

} // namespace agent::service::gateway
