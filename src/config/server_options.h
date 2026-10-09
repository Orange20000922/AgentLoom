#pragma once

#include <chrono>
#include "http_server.h"
#include "request_options.h"
#include "server_common.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include "../skill/skill_manifest.h"
#include <unordered_map>
#include <vector>

struct VramGuardOptions {
    int monitor_interval_seconds = 10;
    size_t warning_free_bytes = 1024 * 1024 * 1024;
    size_t unload_free_bytes = 512 * 1024 * 1024;
    size_t min_free_before_load_bytes = 0;
    bool reload_after_unload = false;
    bool unload_on_oom_error = true;
};

struct EmbeddingModelOptions {
    std::string tokenizer_path;
    std::string onnx_model_path;
    std::string execution_provider = "auto";
    bool allow_cpu_fallback = true;
    int cuda_device_id = 0;
    int intra_op_num_threads = 0;
    int inter_op_num_threads = 0;
    std::string pooling_strategy = "mean";
    bool normalize = true;
    int expected_dimension = 0;
    bool require_token_type_ids = false;
    bool batch_enabled = true;
    std::size_t batch_max_pending_requests = 1024;
    std::size_t batch_max_size = 16;
    int batch_max_wait_ms = 2;
    std::size_t batch_max_inflight = 1;
};

struct BertRuntimeConfigOptions {
    std::string execution_provider = "auto";
    bool allow_cpu_fallback = true;
    int cuda_device_id = 0;
    int intra_op_num_threads = 0;
    int inter_op_num_threads = 0;
    bool enable_cpu_mem_arena = true;
    bool enable_mem_pattern = true;
};

struct VlmCacheConfigOptions {
    bool enabled = false;
    std::string reuse_policy = "result_vector";
    bool persist = false;
    std::filesystem::path cache_dir = "cache/vlm";
    std::size_t max_entries = 512;
    std::size_t max_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::int64_t ttl_seconds = 3600;
    bool store_images = true;
    bool store_prompts = true;
    bool allow_stale_on_failure = true;
    bool default_allow_cache = true;
};

struct VlmCacheVectorOptions {
    bool enabled = false;
    float sim_threshold_high = 0.97f;
    float sim_threshold_mid = 0.93f;
    float max_saliency_for_mid = 0.15f;
    std::size_t max_entries_per_bucket = 256;
    std::int64_t ttl_seconds = 3600;
    bool persist = false;
    std::filesystem::path vector_dir = "cache/vlm/vectors";
};

struct VlmPromptKvCacheOptions {
    bool enabled = false;
    std::string backend = "redis";
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_password;
    std::size_t redis_pool_size = 4;
    int redis_command_timeout_ms = 5000;
    std::string key_prefix = "agent:vlm:prompt_kv";
    std::int64_t ttl_seconds = 3600;
    std::size_t max_bytes = 512ULL * 1024ULL * 1024ULL;
    bool near_embedding_enabled = false;
    float near_same_session_min_cosine = 0.99f;
    float near_cross_session_min_cosine = 0.995f;
    float near_same_session_min_mean_token_cosine = 0.99f;
    float near_cross_session_min_mean_token_cosine = 0.995f;
    float near_same_session_min_p05_token_cosine = 0.95f;
    float near_cross_session_min_p05_token_cosine = 0.98f;
    float near_same_session_max_relative_l2 = 0.15f;
    float near_cross_session_max_relative_l2 = 0.10f;
};

struct LlmOptions {
    bool enabled = true;
    std::string base_url;
    std::string api_key_env = "AGENT_LLM_API_KEY";
    std::string api_key_file;
    /// 在配置校验阶段解析，不直接写入配置文件。
    std::string api_key;
    std::string model = "deepseek-chat";
    int timeout_ms = 30000;
    int max_retries = 2;
    /// 异步出站 HTTP runtime 的 IO 线程数；与 Persona 业务 worker 分离。
    std::size_t async_http_io_threads = 1;
    /// 仅控制已完成请求的 HTTP/1.1 空闲连接复用，不限制同时在途的 Provider 请求数。
    bool http_keep_alive = true;
    /// 所有 origin 合计和单 origin 的空闲连接缓存上限，不是并发连接配额。
    std::size_t http_max_idle_connections = 64;
    std::size_t http_max_idle_connections_per_origin = 64;
    int http_idle_timeout_ms = 30000;
    bool require_api_key = true;
    bool allow_placeholder = false;
    /// 仅供紧急诊断或测试；生产 HTTPS 必须保持 false。
    bool disable_tls_verify_on_windows = false;
    /// HTTPS 校验使用的 PEM CA bundle，相对配置文件解析。
    /// Windows OpenSSL client 需要显式配置；Linux 留空时使用系统信任库。
    std::string ca_bundle_path;
    std::string response_token_count_mode = "off";
    std::filesystem::path response_tokenizer_path;
    std::string response_tokenizer_model;
    std::size_t response_max_token_difference = 2;
    std::unordered_map<std::string, std::filesystem::path> prompts;
};

struct LoggingConfigOptions {
    std::filesystem::path log_dir = "../logs/persona_gateway_e2e";
    std::string logger_name = "agent_gateway_server";
    std::string file_name = "agent_gateway_server.log";
    bool enable_console = true;
    bool use_daily_rotation = false;
    std::size_t max_file_size_bytes = 10 * 1024 * 1024;
    std::size_t max_files = 5;
};

struct LocalLlmConfigOptions {
    bool enabled = false;
    std::string target = "127.0.0.1:50051";
    int deadline_ms = 30000;
    std::string auth_token;
    std::string auth_metadata_key = "authorization";
};

struct EmotionAnalyzerConfigOptions {
    bool enabled = false;
    std::string backend = "neutral";
    std::string target = "127.0.0.1:50051";
    std::filesystem::path tokenizer_path;
    int deadline_ms = 3000;
    std::string auth_token;
    std::string auth_metadata_key = "authorization";
    std::size_t max_length = 128;
    bool truncation = true;
    bool padding = true;
    bool add_special_tokens = true;
};

struct EmotionKeywordRuleConfigOptions {
    std::string label;
    std::string pattern;
    float score = 1.0f;
};

struct EmotionFusionConfigOptions {
    bool enabled = true;
    float bert_weight = 1.0f;
    float default_reliability = 0.7f;
    float accept_confidence = 0.55f;
    float ambiguity_margin = 0.12f;
    float head_bias = 0.0f;
    float bert_signal_weight = 2.0f;
    float keyword_signal_weight = 2.2f;
    float vector_signal_weight = 2.2f;
    float llm_signal_weight = 0.0f;
    float margin_signal_weight = 0.5f;
    float llm_gate_confidence = 0.0f;
    float llm_gate_min_delta = 0.0f;
    std::unordered_map<std::string, double> source_weights;
    std::unordered_map<std::string, double> label_reliability;
    std::vector<EmotionKeywordRuleConfigOptions> keyword_rules;
};

struct EmotionGenerationConfigOptions {
    int max_tokens = 2048;
    int min_tokens = 100;
    double max_token_ratio = 1.25;
    double default_token_weight = 1.0;
    double high_intensity_threshold = 0.7;
    double high_intensity_multiplier = 1.1;
    std::unordered_map<std::string, double> token_weights{
        {"neutral", 1.0}, {"joy", 1.0}, {"excitement", 1.05},
        {"sadness", 1.1}, {"fear", 1.1}, {"anger", 1.05},
        {"disgust", 1.0}, {"surprise", 1.05}, {"tenderness", 1.05},
        {"curiosity", 1.15},
    };
};

struct L0MemoryConfigOptions {
    bool enabled = true;
    std::string redis_host = "127.0.0.1";
    int redis_port = 5000;
    std::filesystem::path sqlite_path = "../data/persona_gateway_e2e/l0_memory.db";
    std::size_t max_cached_records = 1000;
    std::size_t top_k = 5;
    std::size_t candidate_multiplier = 4;
    std::size_t neighbors_per_hit = 1;
    float similarity_floor = 0.78f;
    int64_t warm_window_seconds = 3600;
    int64_t half_life_seconds = 172800;
    int64_t max_age_seconds = 604800;
    std::string user_uuid = "e2e-l0";
};

struct DocumentLlmChunkCacheConfigOptions {
    bool enabled = false;
    std::string redis_host = "127.0.0.1";
    int redis_port = 5000;
    std::size_t redis_pool_size = 4;
    std::string key_prefix = "agent:gateway:document:llm_chunk";
    int ttl_seconds = 7 * 24 * 60 * 60;
};

struct DocumentSemanticCacheConfigOptions {
    bool enabled = false;
    std::string redis_host = "127.0.0.1";
    int redis_port = 5000;
    std::filesystem::path sqlite_path = "../data/agent_gateway/document_semantic_cache.db";
    std::string user_uuid = "document-semantic-cache";
    std::size_t max_cached_records = 10000;
    std::size_t top_k = 6;
    float similarity_floor = 0.94f;
};

struct L3MemoryConfigOptions {
    bool enabled = false;
    std::filesystem::path sqlite_path = "../data/agent_gateway/l3_memory.db";
    std::filesystem::path registry_sqlite_path = "../data/agent_gateway/l3_registry.db";
    std::string collection_name = "l3_memory";
    std::string embedding_fingerprint = "minilm-l6-v2";
    std::string tokenizer_fingerprint = "minilm-l6-v2";
    std::string corpus_version = "v1";
    std::string policy_version = "v1";
    std::string index_backend = "exact";
    std::size_t max_resident_partitions = 16;
    int max_records_per_batch = 1000;
    int compression_max_tokens = 800;
    float compression_temperature = 0.1f;
    std::vector<std::string> user_uuids;
};

struct L3FlushSchedulerConfigOptions {
    bool enabled = false;
    int check_interval_seconds = 300;
    int flush_hour = 3;
    int flush_minute = 0;
    int flush_date_offset_days = 0;
    bool defer_when_sessions_active = true;
};

struct GatewayAuthConfigOptions {
    bool enabled = false;
    bool allow_dev_identity = true;
    bool require_auth_for_api = false;
    std::string cookie_name = "agent_auth";
    std::string public_key_pem;
    std::string public_key_file;
    std::string private_key_pem;
    std::string private_key_file;
    std::string issuer;
    std::string audience;
    int clock_skew_seconds = 60;
    int token_ttl_seconds = 28800;
    bool cookie_http_only = true;
    bool cookie_secure = false;
    std::string cookie_same_site = "Lax";
    bool require_session_record = false;
    bool auto_provision_session = true;
    bool enable_dev_registration = false;
    std::string session_store_backend = "sqlite";
    std::string session_database_path;
    std::string redis_host = "127.0.0.1";
    std::string redis_port = "6379";
    std::string redis_password;
    int redis_pool_size = 8;
    int redis_command_timeout_ms = 5000;
    std::string redis_key_prefix = "agent:gateway:auth";
    bool generate_dev_keys = false;
};

struct GatewayStaticFilesConfigOptions {
    bool enabled = false;
    std::filesystem::path root;
    std::string index_file = "index.html";
    bool spa_fallback = true;
};

struct GatewayThreadPoolConfigOptions {
    std::size_t worker_count = 0;
    std::size_t queue_capacity = 0;
    std::string scheduler = "default_fifo";
    std::size_t max_active_keys = 1024;
    std::size_t max_outstanding_per_key = 8;
    std::size_t max_outstanding_per_fairness_key = 32;
    std::size_t max_outstanding_per_tenant = 256;
};

struct GatewayDocumentStoreConfigOptions {
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

struct GatewayEmotionStateConfigOptions {
    double alpha = 0.75;
    double beta = 0.25;
    double gamma = 0.25;
    double delta = 0.15;
    double baseline_valence = 0.15;
    double baseline_arousal = 0.28;
    double kappa = 0.05;
    double negativity_bias = 1.3;
    double noise_sigma = 0.05;
    double injection_threshold = 0.12;
    int save_interval_turns = 5;
    bool persist_to_l4 = true;
};

struct GatewayEmotionPromptConfigOptions {
    std::unordered_map<std::string, std::string> emotion_map;
    std::unordered_map<std::string, double> emotion_reliability;
    std::unordered_map<std::string, double> confidence_thresholds;
    std::unordered_map<std::string, double> intensity_levels;
};

struct GatewayPersonaConfigOptions {
    std::string persona_id;
    std::string description;
    std::vector<std::string> traits;
    double openness = 0.5;
    double extraversion = 0.5;
    double humor_tendency = 0.5;
    double empathy_level = 0.5;
    double curiosity_level = 0.5;
    double formality = 0.5;
    std::optional<GatewayEmotionPromptConfigOptions> emotion_prompts;
    GatewayEmotionStateConfigOptions emotion_state;
};

struct GatewayStreamingConfigOptions {
    net::SseStreamOptions transport;
    std::size_t max_replay_turns = 64;
    std::size_t max_replay_events = 128;
    std::size_t max_replay_bytes = 512 * 1024;
    int replay_retention_ms = 30000;
};

struct PersonaGatewayConfigOptions {
    std::string websocket_path = "/ws/session";
    GatewayStaticFilesConfigOptions static_files;
    GatewayDocumentStoreConfigOptions document_store;
    GatewayThreadPoolConfigOptions compute_pool;
    GatewayThreadPoolConfigOptions io_pool;
    /// 缺省表示兼容沿用 io_pool；存在时作为独立的长时 Persona/LLM Turn 池配置。
    std::optional<GatewayThreadPoolConfigOptions> llm_pool;
    int session_idle_timeout_minutes = 15;
    std::size_t session_max_recent_turns = 20;
    std::size_t session_max_active_sessions = 1024;
    std::size_t runtime_recent_raw_turns = 10;
    std::string runtime_default_model;
    GatewayStreamingConfigOptions streaming;
    bool request_filter_enabled = true;
    bool reject_control_chars = true;
    bool reject_suspicious_patterns = true;
    std::vector<GatewayPersonaConfigOptions> personas;
};

struct SkillSessionConfigOptions {
    bool enabled = true;
    int startup_timeout_ms = 15000;
    int max_duration_ms = 120000;
    int idle_timeout_ms = 60000;
    int closing_timeout_ms = 10000;
    std::size_t max_recent_observations = 8;
    int cleanup_interval_seconds = 30;
};

struct SkillRegistryConfigOptions {
    bool enabled = true;
    std::filesystem::path manifest_directory;
    std::string manifest_filename_regex = R"(.*skill.*\.json$)";
    // L4 工具记忆：tool_memory_sqlite_path 为空表示跳过种子写入与 provider 构造。
    std::filesystem::path tool_memory_sqlite_path;
    std::string tool_memory_collection_name = "skill_l4";
    int tool_memory_top_k = 3;
    double tool_memory_min_score = 0.78;
};

template <typename GatewayAuthOptionsT>
GatewayAuthOptionsT ToGatewayAuthOptions(const GatewayAuthConfigOptions& config) {
    GatewayAuthOptionsT options;
    options.enabled = config.enabled;
    options.allow_dev_identity = config.allow_dev_identity;
    options.require_auth_for_api = config.require_auth_for_api;
    options.cookie_name = config.cookie_name;
    options.public_key_pem = config.public_key_pem;
    options.private_key_pem = config.private_key_pem;
    options.issuer = config.issuer;
    options.audience = config.audience;
    options.clock_skew = std::chrono::seconds(config.clock_skew_seconds);
    options.token_ttl = std::chrono::seconds(config.token_ttl_seconds);
    options.cookie_http_only = config.cookie_http_only;
    options.cookie_secure = config.cookie_secure;
    options.cookie_same_site = config.cookie_same_site;
    options.require_session_record = config.require_session_record;
    options.auto_provision_session = config.auto_provision_session;
    options.enable_dev_registration = config.enable_dev_registration;
    options.session_store_backend = config.session_store_backend;
    options.session_database_path = config.session_database_path;
    options.redis_host = config.redis_host;
    options.redis_port = config.redis_port;
    options.redis_password = config.redis_password;
    options.redis_pool_size = static_cast<std::size_t>(config.redis_pool_size);
    options.redis_command_timeout = std::chrono::milliseconds(config.redis_command_timeout_ms);
    options.redis_key_prefix = config.redis_key_prefix;
    return options;
}

struct MultimodalServerOptions {
    std::vector<agent::skill::SkillManifest> skill_manifests;
    server_common::GrpcServerOptions grpc;
    net::HttpServerOptions http;
    BertRuntimeConfigOptions bert_runtime;
    EmbeddingModelOptions embedding;
    LlmOptions llm;
    LoggingConfigOptions logging;
    LocalLlmConfigOptions local_llm;
    EmotionAnalyzerConfigOptions emotion_analyzer;
    EmotionFusionConfigOptions emotion_fusion;
    EmotionGenerationConfigOptions emotion_generation;
    L0MemoryConfigOptions l0_memory;
    DocumentLlmChunkCacheConfigOptions document_llm_chunk_cache;
    DocumentSemanticCacheConfigOptions document_semantic_cache;
    L3MemoryConfigOptions l3_memory;
    L3FlushSchedulerConfigOptions l3_flush_scheduler;
    request_validation::AuthOptions auth;
    GatewayAuthConfigOptions gateway_auth;
    PersonaGatewayConfigOptions persona_gateway;
    SkillSessionConfigOptions skill_session;
    SkillRegistryConfigOptions skills;
    request_validation::RequestLimits limits;
    VramGuardOptions vram;
    VlmCacheConfigOptions vlm_cache;
    VlmCacheVectorOptions vlm_cache_vector;
    VlmPromptKvCacheOptions vlm_prompt_kv_cache;
    std::string auth_token_file;
    std::string auth_token_env = "AGENT_BACKEND_AUTH_TOKEN";
    std::string auth_source;
    std::string bert_model;
    std::string vit_model;
    std::string llm_model;
    std::string mmproj;
    int n_gpu_layers = -1;
    std::size_t runner_pool_size = 1;
    int llama_threads = 8;
    int mmproj_threads = 8;
    std::filesystem::path config_file_path;
};

template <typename PersonaGatewayServerOptionsT, typename StaticFileOptionsT>
PersonaGatewayServerOptionsT ToPersonaGatewayServerOptions(const MultimodalServerOptions& config) {
    PersonaGatewayServerOptionsT options;
    options.http = config.http;
    options.http.request_filter.enabled = config.persona_gateway.request_filter_enabled;
    options.http.request_filter.reject_control_chars = config.persona_gateway.reject_control_chars;
    options.http.request_filter.reject_suspicious_patterns = config.persona_gateway.reject_suspicious_patterns;
    options.auth = ToGatewayAuthOptions<decltype(options.auth)>(config.gateway_auth);
    options.websocket_path = config.persona_gateway.websocket_path;
    options.compute_pool.worker_count = config.persona_gateway.compute_pool.worker_count;
    options.compute_pool.queue_capacity = config.persona_gateway.compute_pool.queue_capacity;
    options.compute_pool_concurrency.scheduler = config.persona_gateway.compute_pool.scheduler;
    options.compute_pool_concurrency.max_active_keys = config.persona_gateway.compute_pool.max_active_keys;
    options.compute_pool_concurrency.max_outstanding_per_key =
        config.persona_gateway.compute_pool.max_outstanding_per_key;
    options.compute_pool_concurrency.max_outstanding_per_fairness_key =
        config.persona_gateway.compute_pool.max_outstanding_per_fairness_key;
    options.compute_pool_concurrency.max_outstanding_per_tenant =
        config.persona_gateway.compute_pool.max_outstanding_per_tenant;
    options.io_pool.worker_count = config.persona_gateway.io_pool.worker_count;
    options.io_pool.queue_capacity = config.persona_gateway.io_pool.queue_capacity;
    options.io_pool_concurrency.scheduler = config.persona_gateway.io_pool.scheduler;
    options.io_pool_concurrency.max_active_keys = config.persona_gateway.io_pool.max_active_keys;
    options.io_pool_concurrency.max_outstanding_per_key =
        config.persona_gateway.io_pool.max_outstanding_per_key;
    options.io_pool_concurrency.max_outstanding_per_fairness_key =
        config.persona_gateway.io_pool.max_outstanding_per_fairness_key;
    options.io_pool_concurrency.max_outstanding_per_tenant =
        config.persona_gateway.io_pool.max_outstanding_per_tenant;
    if (config.persona_gateway.llm_pool) {
        typename PersonaGatewayServerOptionsT::LlmPoolOptions llm;
        llm.pool.worker_count = config.persona_gateway.llm_pool->worker_count;
        llm.pool.queue_capacity = config.persona_gateway.llm_pool->queue_capacity;
        llm.concurrency.scheduler = config.persona_gateway.llm_pool->scheduler;
        llm.concurrency.max_active_keys = config.persona_gateway.llm_pool->max_active_keys;
        llm.concurrency.max_outstanding_per_key =
            config.persona_gateway.llm_pool->max_outstanding_per_key;
        llm.concurrency.max_outstanding_per_fairness_key =
            config.persona_gateway.llm_pool->max_outstanding_per_fairness_key;
        llm.concurrency.max_outstanding_per_tenant =
            config.persona_gateway.llm_pool->max_outstanding_per_tenant;
        options.llm_pool = std::move(llm);
    }
    options.session.idle_timeout = std::chrono::minutes(config.persona_gateway.session_idle_timeout_minutes);
    options.session.max_recent_turns = config.persona_gateway.session_max_recent_turns;
    options.session.max_active_sessions = config.persona_gateway.session_max_active_sessions;
    options.runtime.recent_raw_turns = config.persona_gateway.runtime_recent_raw_turns;
    options.streaming.transport = config.persona_gateway.streaming.transport;
    options.streaming.replay.max_turns = config.persona_gateway.streaming.max_replay_turns;
    options.streaming.replay.max_events_per_turn = config.persona_gateway.streaming.max_replay_events;
    options.streaming.replay.max_bytes_per_turn = config.persona_gateway.streaming.max_replay_bytes;
    options.streaming.replay.terminal_retention = std::chrono::milliseconds(config.persona_gateway.streaming.replay_retention_ms);
    options.runtime.default_model = config.persona_gateway.runtime_default_model.empty()
        ? config.llm.model
        : config.persona_gateway.runtime_default_model;
    options.runtime.emotion_generation.default_generation.max_tokens =
        config.emotion_generation.max_tokens;
    options.runtime.emotion_generation.min_tokens = config.emotion_generation.min_tokens;
    options.runtime.emotion_generation.max_token_ratio = config.emotion_generation.max_token_ratio;
    options.runtime.emotion_generation.default_token_weight =
        config.emotion_generation.default_token_weight;
    options.runtime.emotion_generation.high_intensity_threshold =
        config.emotion_generation.high_intensity_threshold;
    options.runtime.emotion_generation.high_intensity_multiplier =
        config.emotion_generation.high_intensity_multiplier;
    options.runtime.emotion_generation.token_weights = {
        config.emotion_generation.token_weights.begin(),
        config.emotion_generation.token_weights.end()};
    if (config.persona_gateway.static_files.enabled) {
        StaticFileOptionsT static_files;
        static_files.root = config.persona_gateway.static_files.root;
        static_files.index_file = config.persona_gateway.static_files.index_file;
        static_files.spa_fallback = config.persona_gateway.static_files.spa_fallback;
        options.static_files = std::move(static_files);
    }
    options.document_store.enabled = config.persona_gateway.document_store.enabled;
    options.document_store.root = config.persona_gateway.document_store.root;
    options.document_store.database_path = config.persona_gateway.document_store.database_path;
    options.document_store.read_connection_count = config.persona_gateway.document_store.read_connection_count;
    options.document_store.write_connection_count = config.persona_gateway.document_store.write_connection_count;
    options.document_store.busy_timeout_ms = config.persona_gateway.document_store.busy_timeout_ms;
    options.document_store.retention_hours = config.persona_gateway.document_store.retention_hours;
    options.document_store.cleanup_interval_seconds = config.persona_gateway.document_store.cleanup_interval_seconds;
    options.document_store.enable_path_register_test_endpoint =
        config.persona_gateway.document_store.enable_path_register_test_endpoint;
    options.document_store.enable_path_analyze_test_endpoint =
        config.persona_gateway.document_store.enable_path_analyze_test_endpoint;
    return options;
}
