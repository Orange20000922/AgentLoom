#include "persona_gateway_server.h"
#include "auth_session_maintenance_task.h"

#include "document_file_store.h"
#include "gateway_session_affinity_scheduler.h"
#include "http_types.h"
#include "l0_memory_cache_adapter.h"
#include "redis_connection_pool.h"
#include "sqlite/sqlite_connection_pool.h"

#include <utility>

namespace agent::service::gateway {

namespace {

PersonaGatewayServerDependencies ResolveSkillToolCallingDependencies(
    PersonaGatewayServerDependencies dependencies) {
    // 共享已装配的 Registry 和 Skill Session，避免工具调用与 HTTP Skill 生命周期分裂。
    if (!dependencies.skill_tool_coordinator && dependencies.skill_registry &&
        dependencies.skill_session_manager) {
        if (!dependencies.skill_executor_factory) {
            dependencies.skill_executor_factory =
                std::make_shared<agent::skill::InMemorySkillExecutorFactory>();
        }
        auto invocation = std::make_shared<agent::skill::SkillInvocationService>(
            dependencies.skill_registry,
            dependencies.skill_executor_factory,
            dependencies.skill_session_manager);
        dependencies.skill_tool_coordinator =
            std::make_shared<agent::skill::SkillToolCallCoordinator>(
                dependencies.skill_registry, std::move(invocation));
    }
    return dependencies;
}

core::ThreadPoolOptions WithDefaultPoolName(core::ThreadPoolOptions options, std::string name) {
    if (options.name.empty() || options.name == "core-thread-pool") {
        options.name = std::move(name);
    }
    return options;
}

core::ThreadPoolOptions ResolvePoolOptions(
    core::ThreadPoolOptions options,
    const GatewayThreadPoolConcurrencyOptions& concurrency,
    std::string name) {
    options = WithDefaultPoolName(std::move(options), std::move(name));
    if (!options.scheduler && concurrency.scheduler == "session_affinity") {
        options.scheduler = std::make_shared<persona::GatewaySessionAffinityScheduler>(
            persona::GatewaySessionAffinitySchedulerOptions{
                .max_active_keys = concurrency.max_active_keys,
                .max_outstanding_per_key = concurrency.max_outstanding_per_key,
                .max_outstanding_per_fairness_key = concurrency.max_outstanding_per_fairness_key,
                .max_outstanding_per_tenant = concurrency.max_outstanding_per_tenant,
            });
    }
    return options;
}

core::ThreadPoolOptions ResolveLlmPoolOptions(
    const PersonaGatewayServerOptions& gateway) {
    auto options = gateway.llm_pool
        ? gateway.llm_pool->pool
        : gateway.io_pool;
    const auto& concurrency = gateway.llm_pool
        ? gateway.llm_pool->concurrency
        : gateway.io_pool_concurrency;
    options.name = "gateway-llm-pool";
    // LLM lane 必须在调度器内按 Session 排队，不能让后续同 Session Turn 占用 worker 等 mutex。
    options.scheduler = std::make_shared<persona::GatewaySessionAffinityScheduler>(
        persona::GatewaySessionAffinitySchedulerOptions{
            .max_active_keys = concurrency.max_active_keys,
            .max_outstanding_per_key = concurrency.max_outstanding_per_key,
            .max_outstanding_per_fairness_key = concurrency.max_outstanding_per_fairness_key,
            .max_outstanding_per_tenant = concurrency.max_outstanding_per_tenant,
        });
    return options;
}

::net::HttpServerOptions ResolveHttpOptions(const PersonaGatewayServerOptions& options) {
    auto http = options.http;
    if (!http.request_filter.enabled) {
        http.request_filter.enabled = true;
        http.request_filter.reject_control_chars = true;
        http.request_filter.reject_suspicious_patterns = true;
    }
    if (options.static_files) {
        http.static_files = std::nullopt;
    }
    return http;
}

std::shared_ptr<IAuthSessionStore> MakeAuthSessionStore(
    const GatewayAuthOptions& options,
    std::shared_ptr<agent::semantic_cache::RedisConnectionPool>& auth_redis,
    const std::shared_ptr<storage::sqlite::SqliteConnectionPool>& metadata_pool) {
    if (options.session_store_backend == "redis") {
        agent::semantic_cache::RedisPoolOptions redis_options;
        redis_options.host = options.redis_host;
        redis_options.port = options.redis_port;
        redis_options.password = options.redis_password;
        redis_options.pool_size = options.redis_pool_size;
        redis_options.command_timeout = options.redis_command_timeout;
        auth_redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(redis_options);
        return std::make_shared<RedisAuthSessionStore>(auth_redis, options.redis_key_prefix);
    }
    if (!metadata_pool) {
        return nullptr;
    }
    return std::make_shared<SqliteAuthSessionStore>(metadata_pool);
}

std::shared_ptr<storage::sqlite::SqliteConnectionPool> MakeGatewayMetadataPool(
    const GatewayAuthOptions& options) {
    if (options.session_database_path.empty()) {
        return nullptr;
    }
    storage::sqlite::SqliteConnectionPoolOptions pool_options;
    pool_options.path = options.session_database_path;
    pool_options.read_connection_count = 4;
    pool_options.write_connection_count = 1;
    pool_options.busy_timeout_ms = 5000;
    pool_options.enable_wal = true;
    return std::make_shared<storage::sqlite::SqliteConnectionPool>(std::move(pool_options));
}

std::shared_ptr<IPersonaMetadataStore> MakePersonaMetadataStore(
    const GatewayAuthOptions& options,
    const std::vector<PersonaMetadataRecord>& default_personas,
    const PersonaGatewayServerDependencies& dependencies,
    const std::shared_ptr<agent::semantic_cache::RedisConnectionPool>& auth_redis,
    const std::shared_ptr<storage::sqlite::SqliteConnectionPool>& metadata_pool) {
    if (dependencies.persona_metadata_store) {
        if (default_personas.empty()) {
            return dependencies.persona_metadata_store;
        }
        return std::make_shared<OverlayPersonaMetadataStore>(
            std::make_shared<ServerDefaultPersonaMetadataStore>(default_personas),
            dependencies.persona_metadata_store);
    }
    std::shared_ptr<IPersonaMetadataStore> account_store;
    if (!metadata_pool) {
        account_store = std::make_shared<InMemoryPersonaMetadataStore>();
    } else {
        auto primary = std::make_shared<SqlitePersonaMetadataStore>(metadata_pool);
        if (!auth_redis) {
            account_store = std::move(primary);
        } else {
            auto cache = std::make_shared<RedisPersonaMetadataCache>(
                auth_redis,
                options.redis_key_prefix.empty() ? "agent:gateway:persona" : options.redis_key_prefix + ":persona");
            account_store = std::make_shared<CachedPersonaMetadataStore>(std::move(primary), std::move(cache));
        }
    }
    if (default_personas.empty()) {
        return account_store;
    }
    return std::make_shared<OverlayPersonaMetadataStore>(
        std::make_shared<ServerDefaultPersonaMetadataStore>(default_personas),
        std::move(account_store));
}

} // namespace

PersonaGatewayServer::PersonaGatewayServer(PersonaGatewayServerOptions options,
                                           PersonaGatewayServerDependencies dependencies,
                                           core::LoggerAdapter logger)
    : options_(std::move(options)),
      dependencies_(ResolveSkillToolCallingDependencies(std::move(dependencies))),
      logger_(std::move(logger)),
      compute_pool_(ResolvePoolOptions(
          options_.compute_pool,
          options_.compute_pool_concurrency,
          "gateway-compute-pool")),
      io_pool_(ResolvePoolOptions(
          options_.io_pool,
          options_.io_pool_concurrency,
          "gateway-io-pool")),
      llm_pool_(ResolveLlmPoolOptions(options_)),
      sessions_(compute_pool_, io_pool_, options_.session, logger_, &llm_pool_),
      runtime_(sessions_,
               dependencies_.memory_provider,
               dependencies_.emotion_analyzer,
               dependencies_.llm_client,
               options_.runtime,
               nullptr,
               dependencies_.tool_memory_provider,
               dependencies_.skill_session_manager,
               logger_,
               nullptr,
               dependencies_.async_llm_client,
               &llm_pool_,
               dependencies_.skill_tool_coordinator,
               dependencies_.stateful_skill_router),
      classroom_scheduler_({}, core::LoggerAdapter::ForModule("classroom")),
      gateway_metadata_pool_(MakeGatewayMetadataPool(options_.auth)),
      auth_session_store_(MakeAuthSessionStore(options_.auth, auth_redis_, gateway_metadata_pool_)),
      persona_metadata_store_(MakePersonaMetadataStore(
          options_.auth,
          options_.default_personas,
          dependencies_,
          auth_redis_,
          gateway_metadata_pool_)),
      service_(sessions_,
               runtime_,
               &classroom_scheduler_,
               dependencies_.report_evaluator,
               persona_metadata_store_,
               logger_),
      document_service_(std::make_shared<document::DocumentAnalysisService>(
          compute_pool_,
          io_pool_,
          core::LoggerAdapter::ForModule("document"))),
      authenticator_(std::make_shared<JwtCookieAuthenticator>(options_.auth, auth_session_store_)),
      auth_registration_(std::make_shared<JwtAuthRegistrationService>(options_.auth, auth_session_store_)),
      adapter_(service_,
               authenticator_,
               auth_registration_,
               document_service_,
               dependencies_.llm_client,
               dependencies_.document_embedding_provider,
               dependencies_.document_llm_chunk_cache,
               dependencies_.document_semantic_cache,
               dependencies_.skill_session_manager,
               PersonaGatewayHttpAdapterOptions{
                   .enable_dev_registration = options_.auth.enable_dev_registration,
                   .enable_path_register_test_endpoint =
                       options_.document_store.enable_path_register_test_endpoint,
                   .enable_path_analyze_test_endpoint =
                       options_.document_store.enable_path_analyze_test_endpoint},
               dependencies_.stateful_skill_router),
      http_server_(ResolveHttpOptions(options_)),
      maintenance_(core::LoggerAdapter::ForModule("gateway")),
      lifecycle_(core::LoggerAdapter::ForModule("gateway")) {
    if (dependencies_.l0_memory_adapter) {
        auto l0 = dependencies_.l0_memory_adapter;
        sessions_.SetSessionClosedCallback([l0 = std::move(l0)](const persona::SessionSnapshot& snapshot) {
            l0->ReleaseSession({snapshot.tenant_id, snapshot.user_uuid, snapshot.session_id});
        });
        if (options_.embedding_batch.enabled) {
            ::vector::EmbeddingBatchCoordinatorOptions batch_options;
            batch_options.max_pending_requests =
                options_.embedding_batch.max_pending_requests;
            batch_options.max_batch_size = options_.embedding_batch.max_batch_size;
            batch_options.max_batch_wait = options_.embedding_batch.max_batch_wait;
            batch_options.max_inflight_batches =
                options_.embedding_batch.max_inflight_batches;
            lifecycle_configuration_status_ = dependencies_.l0_memory_adapter->ConfigureBatching(
                compute_pool_, io_pool_, batch_options);
        }
    }
    if (options_.static_files) {
        static_files_ = std::make_shared<::net::StaticFileHandler>(*options_.static_files);
    }

    static_cast<void>(maintenance_.RegisterTask(std::make_shared<SessionMaintenanceTask>(
        sessions_,
        std::chrono::seconds(30),
        core::LoggerAdapter::ForModule("gateway"))));
    if (auth_session_store_) {
        static_cast<void>(maintenance_.RegisterTask(std::make_shared<AuthSessionMaintenanceTask>(
            auth_session_store_,
            std::chrono::seconds(60),
            256,
            core::LoggerAdapter::ForModule("gateway"))));
    }
    for (const auto& task : dependencies_.maintenance_tasks) {
        static_cast<void>(maintenance_.RegisterTask(task));
    }
    if (options_.document_store.enabled) {
        static_cast<void>(maintenance_.RegisterTask(std::make_shared<DocumentRetentionMaintenanceTask>(
            document_service_,
            std::chrono::seconds(options_.document_store.cleanup_interval_seconds))));
    }

    http_server_.SetHttpRequestHandler([this](std::shared_ptr<::net::IHttpRequest> request) {
        HandleHttp(std::move(request));
    });
    http_server_.SetWebSocketStreamHandler(options_.websocket_path, [this](std::shared_ptr<::net::IWebSocketStreamRequest> request) {
        HandleWebSocket(std::move(request));
    });
    http_server_.SetWebSocketCloseHandler([this](const ::net::ConnectionCloseInfo& close_info) {
        HandleWebSocketClose(close_info);
    });
    if (lifecycle_configuration_status_.ok()) {
        lifecycle_configuration_status_ = ConfigureLifecycle();
    }
}

PersonaGatewayServer::~PersonaGatewayServer() {
    Stop();
}

core::Status PersonaGatewayServer::RegisterMaintenanceTask(std::shared_ptr<IRuntimeMaintenanceTask> task) {
    if (lifecycle_.state() != GatewayLifecycleState::Stopped || maintenance_.running()) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "maintenance tasks must be registered before server start");
    }
    return maintenance_.RegisterTask(std::move(task));
}

core::Status PersonaGatewayServer::Start() {
    if (!lifecycle_configuration_status_.ok()) {
        return lifecycle_configuration_status_;
    }
    auto status = lifecycle_.Start();
    if (!status.ok()) {
        return status;
    }
    logger_.info("[gateway] started http_port={} ws_path={}", http_server_.port(), options_.websocket_path);
    return core::Status::Ok();
}

core::Status PersonaGatewayServer::ConfigureLifecycle() {
    auto register_component = [this](std::string name,
                                     CallbackGatewayLifecycleComponent::StartCallback start,
                                     CallbackGatewayLifecycleComponent::StopCallback stop) {
        return lifecycle_.Register(std::make_shared<CallbackGatewayLifecycleComponent>(
            std::move(name), std::move(start), std::move(stop)));
    };

    auto status = register_component(
        "reference-storage",
        [this]() {
            if (auto value = ValidateDependencies(); !value.ok()) {
                return value;
            }
            if (auto value = EnsureGatewayMetadataPool(); !value.ok()) {
                return value;
            }
            if (auto value = EnsureAuthSessionStore(); !value.ok()) {
                return value;
            }
            if (auto value = EnsurePersonaMetadataStore(); !value.ok()) {
                return value;
            }
            return EnsureDocumentStore();
        },
        [this](std::chrono::steady_clock::time_point) {
            if (auth_redis_) {
                auth_redis_->Shutdown();
            }
            ShutdownDocumentStore();
            if (gateway_metadata_pool_) {
                gateway_metadata_pool_->Close();
            }
            return core::Status::Ok();
        });
    if (!status.ok()) {
        return status;
    }
    status = register_component(
        "compute-pool",
        [this]() { return compute_pool_.Start(); },
        [this](std::chrono::steady_clock::time_point) {
            compute_pool_.Shutdown(true);
            return core::Status::Ok();
        });
    if (!status.ok()) {
        return status;
    }
    status = register_component(
        "io-pool",
        [this]() { return io_pool_.Start(); },
        [this](std::chrono::steady_clock::time_point) {
            io_pool_.Shutdown(true);
            return core::Status::Ok();
        });
    if (!status.ok()) {
        return status;
    }
    if (dependencies_.l0_memory_adapter && options_.embedding_batch.enabled) {
        status = register_component(
            "embedding-batch",
            [this]() { return dependencies_.l0_memory_adapter->StartBatching(); },
            [this](std::chrono::steady_clock::time_point) {
                dependencies_.l0_memory_adapter->ShutdownBatching();
                return core::Status::Ok();
            });
        if (!status.ok()) {
            return status;
        }
    }
    status = register_component(
        "llm-pool",
        [this]() { return llm_pool_.Start(); },
        [this](std::chrono::steady_clock::time_point) {
            llm_pool_.Shutdown(true);
            return core::Status::Ok();
        });
    if (!status.ok()) {
        return status;
    }
    status = register_component(
        "sessions",
        []() { return core::Status::Ok(); },
        [this](std::chrono::steady_clock::time_point) {
            sessions_.Shutdown();
            runtime_.Shutdown();
            return core::Status::Ok();
        });
    if (!status.ok()) {
        return status;
    }
    status = register_component(
        "maintenance",
        [this]() { return maintenance_.Start(); },
        [this](std::chrono::steady_clock::time_point) {
            maintenance_.Stop();
            return core::Status::Ok();
        });
    if (!status.ok()) {
        return status;
    }
    return register_component(
        "http-ingress",
        [this]() { return http_server_.Start(); },
        [this](std::chrono::steady_clock::time_point) {
            http_server_.Stop();
            return core::Status::Ok();
        });
}

core::Status PersonaGatewayServer::EnsureGatewayMetadataPool() {
    if (!gateway_metadata_pool_) {
        return core::Status::Ok();
    }
    return gateway_metadata_pool_->Start();
}

core::Status PersonaGatewayServer::EnsureAuthSessionStore() {
    if (auth_redis_ && !auth_redis_->running()) {
        auto start = auth_redis_->Start();
        if (!start.ok()) {
            return start;
        }
    }
    if (!auth_session_store_) {
        return core::Status::Ok();
    }
    return auth_session_store_->EnsureSchema();
}

core::Status PersonaGatewayServer::EnsurePersonaMetadataStore() {
    if (!persona_metadata_store_) {
        return core::Status::Ok();
    }
    return persona_metadata_store_->EnsureSchema();
}

core::Status PersonaGatewayServer::EnsureDocumentStore() {
    if (!options_.document_store.enabled) {
        return core::Status::Ok();
    }
    if (options_.document_store.root.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "gateway document store root is required");
    }
    if (options_.document_store.database_path.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "gateway document store database_path is required");
    }

    storage::sqlite::SqliteConnectionPoolOptions pool_options;
    pool_options.path = options_.document_store.database_path.string();
    pool_options.read_connection_count = options_.document_store.read_connection_count;
    pool_options.write_connection_count = options_.document_store.write_connection_count;
    pool_options.busy_timeout_ms = options_.document_store.busy_timeout_ms;
    document_repository_pool_ = std::make_shared<storage::sqlite::SqliteConnectionPool>(pool_options);
    auto start = document_repository_pool_->Start();
    if (!start.ok()) {
        return start;
    }
    auto repository_status = document_service_->SetRepository(document_repository_pool_);
    if (!repository_status.ok()) {
        document_repository_pool_->Close();
        document_repository_pool_.reset();
        return repository_status;
    }

    auto file_store = std::make_shared<document::DocumentFileStore>(
        document::DocumentFileStoreOptions{
            options_.document_store.root,
            std::chrono::hours(options_.document_store.retention_hours)});
    auto file_store_status = document_service_->SetFileStore(std::move(file_store));
    if (!file_store_status.ok()) {
        ShutdownDocumentStore();
        return file_store_status;
    }
    document_service_->SetRetentionCleanupOptions(
        std::chrono::hours(options_.document_store.retention_hours),
        std::chrono::seconds(options_.document_store.cleanup_interval_seconds));
    logger_.info("[gateway] document store enabled root={} db={}",
                 options_.document_store.root.string(),
                 options_.document_store.database_path.string());
    return core::Status::Ok();
}

void PersonaGatewayServer::ShutdownDocumentStore() {
    if (document_service_) {
        static_cast<void>(document_service_->SetFileStore(nullptr));
        static_cast<void>(document_service_->SetRepository(nullptr));
    }
    if (document_repository_pool_) {
        document_repository_pool_->Close();
        document_repository_pool_.reset();
    }
}

void PersonaGatewayServer::Stop() {
    const auto status = lifecycle_.Stop();
    if (!status.ok()) {
        logger_.error("[gateway] stop failed: {}", status.message());
    } else {
        logger_.info("[gateway] stopped");
    }
}

bool PersonaGatewayServer::running() const noexcept {
    return http_server_.running();
}

std::uint16_t PersonaGatewayServer::port() const noexcept {
    return http_server_.port();
}

persona::SessionManager& PersonaGatewayServer::sessions() noexcept {
    return sessions_;
}

PersonaGatewayService& PersonaGatewayServer::service() noexcept {
    return service_;
}

::net::HttpServer& PersonaGatewayServer::http_server() noexcept {
    return http_server_;
}

void PersonaGatewayServer::HandleHttp(std::shared_ptr<::net::IHttpRequest> request) {
    if (!request) {
        return;
    }
    if (PersonaGatewayHttpAdapter::IsApiRequest(request->message().target())) {
        adapter_.HandleHttp(std::move(request));
        return;
    }

    if (static_files_) {
        request->Respond(static_files_->Handle(request->message()));
        return;
    }

    auto response = ::net::HttpResponse::Text(::net::http::status::not_found, "not found").message;
    response.version(request->message().version());
    response.keep_alive(request->message().keep_alive());
    response.prepare_payload();
    request->Respond(std::move(response));
}

void PersonaGatewayServer::HandleWebSocket(std::shared_ptr<::net::IWebSocketStreamRequest> request) {
    adapter_.HandleWebSocket(std::move(request));
}

void PersonaGatewayServer::HandleWebSocketClose(const ::net::ConnectionCloseInfo& close_info) {
    if (close_info.target != options_.websocket_path) {
        return;
    }
    adapter_.CleanupUnfinishedDocumentUploadsForConnection(close_info.connection_id);
}

core::Status PersonaGatewayServer::ValidateDependencies() const {
    if (!dependencies_.memory_provider) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "memory context provider is required");
    }
    if (!dependencies_.emotion_analyzer) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "emotion analyzer is required");
    }
    if (!dependencies_.llm_client) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "llm client is required");
    }
    if (dependencies_.skill_executor_factory && !dependencies_.skill_tool_coordinator) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                  "skill executor factory requires a registry and session manager");
    }
    return core::Status::Ok();
}

} // namespace agent::service::gateway
