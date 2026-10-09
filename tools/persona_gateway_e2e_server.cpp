// Manual E2E server for the Persona Gateway + static frontend dist.
//
// Not part of CTest. Run manually:
//   .\build\Release\persona_gateway_e2e_server.exe tools\persona_gateway_e2e_server.json
//
// Starts one C++ HTTP server that hosts:
//   - static frontend files from ./dist
//   - /api/* gateway routes
//   - /ws/session websocket route
//
// The tool intentionally follows the l3_compression_e2e_test style:
// config is read from JSON, LLM API key is resolved from env/file, and
// paths are resolved relative to the config file directory.

#include "persona_gateway_server.h"
#include "grpc_emotion_analyzer.h"
#include "emotion_fusion_analyzer.h"
#include "semantic_cache_types.h"
#include "isemantic_cache.h"
#include "openai_llm_client.h"
#include "local_llm_client.h"
#include "async_beast_http_client.h"
#include "beast_http_client.h"
#include "tls_context.h"
#include "crash_dump.h"
#include "logger.h"
#include "redis_connection_pool.h"
#include "semantic_cache_pipeline.h"
#include "l0_memory_cache_adapter.h"
#include "sqlite/sqlite_connection.h"
#include "embedding_pipeline.h"
#include "hf_tokenizer.h"
#include "onnx_text_embedding_model.h"

#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::atomic_bool g_stop_requested{false};

int Fail(const std::string& msg) {
    std::cerr << "[gateway-e2e] FAIL: " << msg << std::endl;
    return 1;
}

void OnSignal(int) {
    g_stop_requested.store(true);
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string ReadFirstLine(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    std::string line;
    std::getline(file, line);
    return line;
}

fs::path ResolvePath(const fs::path& config_path, const fs::path& path) {
    if (path.empty() || path.is_absolute()) {
        return path;
    }
    return config_path.parent_path() / path;
}

fs::path FindRepoRoot(const fs::path& start) {
    auto current = fs::absolute(start);
    if (fs::is_regular_file(current)) {
        current = current.parent_path();
    }
    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        if (fs::exists(current / "CMakeLists.txt") && fs::exists(current / "src") && fs::exists(current / "tools")) {
            return current;
        }
        current = current.parent_path();
    }
    return fs::current_path();
}

std::optional<std::string> ReadEnv(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
    free(value);
#else
    const char* value = std::getenv(name.c_str());
    if (!value) {
        return std::nullopt;
    }
    std::string out(value);
#endif
    return out.empty() ? std::nullopt : std::optional<std::string>(std::move(out));
}

std::string GetString(const Json& object, std::string_view name, std::string fallback = {}) {
    auto it = object.find(std::string(name));
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}

int GetInt(const Json& object, std::string_view name, int fallback) {
    auto it = object.find(std::string(name));
    return it != object.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

bool GetBool(const Json& object, std::string_view name, bool fallback) {
    auto it = object.find(std::string(name));
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::size_t GetSize(const Json& object, std::string_view name, std::size_t fallback) {
    auto it = object.find(std::string(name));
    return it != object.end() && it->is_number_integer() ? it->get<std::size_t>() : fallback;
}

float GetFloat(const Json& object, std::string_view name, float fallback) {
    auto it = object.find(std::string(name));
    return it != object.end() && it->is_number() ? it->get<float>() : fallback;
}

struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

struct PKeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

struct GeneratedKeyPair {
    std::string private_key_pem;
    std::string public_key_pem;
};

GeneratedKeyPair GenerateRsaKeyPair() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
        EVP_PKEY_CTX_free);
    if (!ctx ||
        EVP_PKEY_keygen_init(ctx.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) != 1) {
        throw std::runtime_error("failed to initialize RSA key generator");
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw_key) != 1 || raw_key == nullptr) {
        throw std::runtime_error("failed to generate RSA key pair");
    }
    std::unique_ptr<EVP_PKEY, PKeyDeleter> key(raw_key);

    std::unique_ptr<BIO, BioDeleter> private_bio(BIO_new(BIO_s_mem()));
    std::unique_ptr<BIO, BioDeleter> public_bio(BIO_new(BIO_s_mem()));
    if (!private_bio || !public_bio) {
        throw std::runtime_error("failed to allocate OpenSSL BIO");
    }
    if (PEM_write_bio_PrivateKey(private_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        throw std::runtime_error("failed to write private key PEM");
    }
    if (PEM_write_bio_PUBKEY(public_bio.get(), key.get()) != 1) {
        throw std::runtime_error("failed to write public key PEM");
    }

    BUF_MEM* private_mem = nullptr;
    BUF_MEM* public_mem = nullptr;
    BIO_get_mem_ptr(private_bio.get(), &private_mem);
    BIO_get_mem_ptr(public_bio.get(), &public_mem);
    if (!private_mem || !public_mem) {
        throw std::runtime_error("failed to read generated key PEM");
    }
    return {
        std::string(private_mem->data, private_mem->length),
        std::string(public_mem->data, public_mem->length),
    };
}

class NoopSemanticCache final : public agent::semantic_cache::ISemanticCache {
public:
    core::Result<agent::semantic_cache::CacheLookupResult> Lookup(
        const agent::semantic_cache::CacheLookupRequest&) override {
        agent::semantic_cache::CacheLookupResult result;
        result.hit = false;
        return result;
    }

    core::Status Store(const agent::semantic_cache::CacheStoreRequest&) override {
        return core::Status::Ok();
    }
};

class PlaceholderLlmClient final : public agent::llm::ILlmClient {
public:
    core::Result<agent::llm::ChatCompletionResponse> Complete(
        const agent::llm::ChatCompletionRequest& req) override {
        agent::llm::ChatCompletionResponse response;
        response.id = "placeholder-e2e";
        response.model = req.model.empty() ? "placeholder-e2e" : req.model;
        response.content =
            "This is a placeholder E2E response from persona_gateway_e2e_server. "
            "Configure llm.base_url and an API key to measure the real maximum-latency chain.";
        response.completion_tokens = 32;
        response.total_tokens = 32;
        return response;
    }
};

struct ToolConfig {
    fs::path config_path;
    fs::path repo_root;
    fs::path dump_dir;
    Json root;
    agent::service::gateway::PersonaGatewayServerOptions gateway;
    logging::LoggerOptions logging;
    agent::llm::OpenAiLlmClientOptions cloud_llm;
    bool cloud_llm_enabled = false;
    std::size_t async_http_io_threads = 1;
    bool async_http_keep_alive = true;
    std::size_t async_http_max_idle_connections = 64;
    std::size_t async_http_max_idle_connections_per_origin = 64;
    std::chrono::milliseconds async_http_idle_timeout{30000};
    bool local_llm_enabled = false;
    bool allow_placeholder_llm = false;
    bool l0_enabled = false;
    fs::path tokenizer_path;
    fs::path embedding_model_path;
    fs::path emotion_tokenizer_path;
    std::string embedding_provider = "auto";
    int embedding_dimension = 384;
    std::string l0_redis_host = "127.0.0.1";
    int l0_redis_port = 5000;
    fs::path l0_sqlite_path;
    std::size_t l0_max_cached_records = 1000;
    std::size_t l0_top_k = 5;
    std::size_t l0_neighbors_per_hit = 1;
    float l0_similarity_floor = 0.78f;
    std::string l0_user_uuid = "e2e-l0";
    agent::llm::GrpcLocalLlmClientOptions local_llm;
    bool disable_tls_verify_on_windows = true;
    bool grpc_emotion_enabled = false;
    agent::service::persona::GrpcEmotionAnalyzerOptions grpc_emotion;
    agent::service::persona::EmotionFusionAnalyzerOptions emotion_fusion;
    bool emotion_fusion_enabled = true;
    std::vector<agent::service::persona::EmotionKeywordRule> emotion_keyword_rules;
    bool emotion_vector_enabled = false;
    bool skill_session_enabled = true;
    agent::service::persona::SkillSessionOptions skill_session_options;
    int skill_session_cleanup_interval_seconds = 30;
};

struct L0MemoryCacheBundle {
    std::shared_ptr<agent::semantic_cache::ISemanticCache> cache;
    std::shared_ptr<agent::semantic_cache::RedisConnectionPool> redis_pool;
    std::shared_ptr<agent::semantic_cache::L0MemoryCacheAdapter> adapter;
};

std::string ResolveApiKey(const fs::path& config_path, const Json& llm) {
    const auto env_name = GetString(llm, "api_key_env", "AGENT_LLM_API_KEY");
    if (auto key = ReadEnv(env_name)) {
        return *key;
    }

    const auto api_key_file = GetString(llm, "api_key_file");
    if (!api_key_file.empty()) {
        auto key_path = ResolvePath(config_path, api_key_file);
        auto key = ReadFirstLine(key_path);
        if (key.empty()) {
            throw std::runtime_error("API key file is empty: " + key_path.string());
        }
        return key;
    }
    return {};
}

std::vector<agent::service::persona::EmotionKeywordRule> ParseEmotionKeywordRules(const Json& emotion_fusion) {
    auto rules = agent::service::persona::DefaultEmotionKeywordRules();
    auto it = emotion_fusion.find("keyword_rules");
    if (it == emotion_fusion.end() || !it->is_object()) {
        return rules;
    }
    for (auto label = it->begin(); label != it->end(); ++label) {
        if (!label.value().is_array()) {
            continue;
        }
        for (const auto& item : label.value()) {
            if (item.is_string()) {
                rules.push_back(agent::service::persona::EmotionKeywordRule{
                    label.key(),
                    item.get<std::string>(),
                    1.0,
                });
            } else if (item.is_object()) {
                const auto pattern = GetString(item, "pattern");
                if (!pattern.empty()) {
                    rules.push_back(agent::service::persona::EmotionKeywordRule{
                        label.key(),
                        pattern,
                        GetFloat(item, "score", 1.0f),
                    });
                }
            }
        }
    }
    return rules;
}

void ValidateThreadPoolConcurrency(
    std::string_view pool_name,
    const agent::service::gateway::GatewayThreadPoolConcurrencyOptions& options) {
    if (options.scheduler != "default_fifo" && options.scheduler != "session_affinity") {
        throw std::runtime_error(std::string(pool_name) +
                                 ".scheduler must be default_fifo or session_affinity");
    }
    if (options.max_active_keys == 0 ||
        options.max_outstanding_per_key == 0 ||
        options.max_outstanding_per_fairness_key == 0 ||
        options.max_outstanding_per_tenant == 0) {
        throw std::runtime_error(std::string(pool_name) + " scheduler limits must be positive");
    }
}

ToolConfig LoadConfig(const fs::path& config_path) {
    std::ifstream file(config_path);
    if (!file) {
        throw std::runtime_error("cannot open config file: " + config_path.string());
    }

    ToolConfig config;
    config.config_path = config_path;
    config.repo_root = FindRepoRoot(config_path);
    config.dump_dir = config.repo_root / "dumps";
    config.root = Json::parse(file);

    const auto logging = config.root.value("logging", Json::object());
    config.logging.log_dir = ResolvePath(config_path, GetString(logging, "log_dir", "../logs/persona_gateway_e2e"));
    config.logging.logger_name = GetString(logging, "logger_name", "persona_gateway_e2e_server");
    config.logging.file_name = GetString(logging, "file_name", "persona_gateway_e2e_server.log");
    config.logging.enable_console = GetBool(logging, "enable_console", true);
    config.logging.use_daily_rotation = GetBool(logging, "use_daily_rotation", false);
    config.logging.max_file_size_bytes = static_cast<std::size_t>(GetInt(logging, "max_file_size_bytes", 10 * 1024 * 1024));
    config.logging.max_files = static_cast<std::size_t>(GetInt(logging, "max_files", 5));
    config.logging.module_names = {"gateway", "service", "classroom", "gateway-auth", "net", "gateway-sse",
                                    "async-http-client", "async-llm-client", "llm-stream"};

    const auto gateway = config.root.value("persona_gateway", Json::object());
    config.gateway.http.address = GetString(gateway, "address", "127.0.0.1");
    config.gateway.http.port = static_cast<std::uint16_t>(GetInt(gateway, "port", 18080));
    config.gateway.http.io_threads = GetInt(gateway, "http_io_threads", 1);
    config.gateway.websocket_path = GetString(gateway, "websocket_path", "/ws/session");
    const auto streaming = gateway.value("streaming", Json::object());
    auto& transport = config.gateway.streaming.transport;
    auto& replay = config.gateway.streaming.replay;
    transport.outbound.max_items = GetSize(streaming, "max_pending_events", transport.outbound.max_items);
    transport.outbound.max_bytes = GetSize(streaming, "max_pending_bytes", transport.outbound.max_bytes);
    transport.max_event_bytes = GetSize(streaming, "max_event_bytes", transport.max_event_bytes);
    transport.heartbeat_interval = std::chrono::milliseconds(GetInt(streaming, "heartbeat_interval_ms", 15000));
    transport.idle_timeout = std::chrono::milliseconds(GetInt(streaming, "idle_timeout_ms", 60000));
    transport.max_duration = std::chrono::milliseconds(GetInt(streaming, "max_duration_ms", 300000));
    transport.require_pong = GetBool(streaming, "require_pong", false);
    replay.max_turns = GetSize(streaming, "max_replay_turns", replay.max_turns);
    replay.max_events_per_turn = GetSize(streaming, "max_replay_events", replay.max_events_per_turn);
    replay.max_bytes_per_turn = GetSize(streaming, "max_replay_bytes", replay.max_bytes_per_turn);
    replay.terminal_retention = std::chrono::milliseconds(GetInt(streaming, "replay_retention_ms", 30000));

    const auto compute_pool = gateway.value("compute_pool", Json::object());
    config.gateway.compute_pool.worker_count = GetSize(compute_pool, "worker_count", 2);
    config.gateway.compute_pool.queue_capacity = GetSize(compute_pool, "queue_capacity", 256);
    config.gateway.compute_pool_concurrency.scheduler = GetString(compute_pool, "scheduler", "default_fifo");
    config.gateway.compute_pool_concurrency.max_active_keys = GetSize(compute_pool, "max_active_keys", 1024);
    config.gateway.compute_pool_concurrency.max_outstanding_per_key =
        GetSize(compute_pool, "max_outstanding_per_key", 8);
    config.gateway.compute_pool_concurrency.max_outstanding_per_fairness_key =
        GetSize(compute_pool, "max_outstanding_per_fairness_key", 32);
    config.gateway.compute_pool_concurrency.max_outstanding_per_tenant =
        GetSize(compute_pool, "max_outstanding_per_tenant", 256);
    ValidateThreadPoolConcurrency("persona_gateway.compute_pool",
                                  config.gateway.compute_pool_concurrency);

    const auto io_pool = gateway.value("io_pool", Json::object());
    config.gateway.io_pool.worker_count = GetSize(io_pool, "worker_count", 2);
    config.gateway.io_pool.queue_capacity = GetSize(io_pool, "queue_capacity", 256);
    config.gateway.io_pool_concurrency.scheduler = GetString(io_pool, "scheduler", "default_fifo");
    config.gateway.io_pool_concurrency.max_active_keys = GetSize(io_pool, "max_active_keys", 1024);
    config.gateway.io_pool_concurrency.max_outstanding_per_key =
        GetSize(io_pool, "max_outstanding_per_key", 8);
    config.gateway.io_pool_concurrency.max_outstanding_per_fairness_key =
        GetSize(io_pool, "max_outstanding_per_fairness_key", 32);
    config.gateway.io_pool_concurrency.max_outstanding_per_tenant =
        GetSize(io_pool, "max_outstanding_per_tenant", 256);
    ValidateThreadPoolConcurrency("persona_gateway.io_pool",
                                  config.gateway.io_pool_concurrency);

    if (const auto llm_pool = gateway.find("llm_pool");
        llm_pool != gateway.end() && llm_pool->is_object()) {
        agent::service::gateway::GatewayLlmPoolOptions options;
        options.pool.worker_count = GetSize(*llm_pool, "worker_count", 4);
        options.pool.queue_capacity = GetSize(*llm_pool, "queue_capacity", 1024);
        options.concurrency.scheduler = GetString(
            *llm_pool, "scheduler", "session_affinity");
        options.concurrency.max_active_keys = GetSize(
            *llm_pool, "max_active_keys", 1024);
        options.concurrency.max_outstanding_per_key = GetSize(
            *llm_pool, "max_outstanding_per_key", 8);
        options.concurrency.max_outstanding_per_fairness_key = GetSize(
            *llm_pool, "max_outstanding_per_fairness_key", 1024);
        options.concurrency.max_outstanding_per_tenant = GetSize(
            *llm_pool, "max_outstanding_per_tenant", 1024);
        ValidateThreadPoolConcurrency(
            "persona_gateway.llm_pool", options.concurrency);
        config.gateway.llm_pool = std::move(options);
    }

    config.gateway.session.idle_timeout = std::chrono::minutes(GetInt(gateway, "session_idle_timeout_minutes", 15));
    config.gateway.session.max_recent_turns = GetSize(gateway, "session_max_recent_turns", 20);
    config.gateway.runtime.recent_raw_turns = GetSize(gateway, "runtime_recent_raw_turns", 10);
    config.gateway.runtime.default_model = GetString(gateway, "runtime_default_model");

    // E2E 仅暴露生产 Gateway 已有的合批开关，保证 A/B 测试只改变调度策略。
    const auto embedding_batch = config.root.value("embedding_batch", Json::object());
    config.gateway.embedding_batch.enabled = GetBool(embedding_batch, "enabled", true);
    config.gateway.embedding_batch.max_pending_requests = GetSize(
        embedding_batch, "max_pending_requests", 1024);
    config.gateway.embedding_batch.max_batch_size = GetSize(
        embedding_batch, "max_batch_size", 16);
    config.gateway.embedding_batch.max_batch_wait = std::chrono::milliseconds(
        GetInt(embedding_batch, "max_batch_wait_ms", 2));
    config.gateway.embedding_batch.max_inflight_batches = GetSize(
        embedding_batch, "max_inflight_batches", 1);

    // 压测与手工 E2E 可从配置提供只读默认人格，避免每个 Session 重复写入元数据后端。
    const auto personas = gateway.value("personas", Json::array());
    if (!personas.is_array()) {
        throw std::runtime_error("persona_gateway.personas must be an array");
    }
    for (const auto& persona : personas) {
        if (!persona.is_object()) {
            throw std::runtime_error("persona_gateway.personas[] must be an object");
        }
        agent::service::gateway::PersonaMetadataRecord record;
        record.tenant_id = "server";
        record.user_uuid = "server";
        record.persona_id = GetString(
            persona, "personaId", GetString(persona, "persona_id"));
        if (record.persona_id.empty()) {
            throw std::runtime_error("persona_gateway.personas[] requires personaId");
        }
        const auto personality = persona.value("personality", Json::object());
        record.personality.name = GetString(
            personality, "name", record.persona_id);
        record.personality.description = GetString(personality, "description");
        if (const auto traits = personality.find("traits");
            traits != personality.end() && traits->is_array()) {
            record.personality.traits = traits->get<std::vector<std::string>>();
        }
        record.personality.openness = GetFloat(personality, "openness", 0.5f);
        record.personality.extraversion = GetFloat(personality, "extraversion", 0.5f);
        record.personality.humor_tendency = GetFloat(
            personality, "humorTendency", 0.5f);
        record.personality.empathy_level = GetFloat(
            personality, "empathyLevel", 0.5f);
        record.personality.curiosity_level = GetFloat(
            personality, "curiosityLevel", 0.5f);
        record.personality.formality = GetFloat(personality, "formality", 0.5f);
        record.emotion_state_config.noise_sigma = 0.0;
        config.gateway.default_personas.push_back(std::move(record));
    }

    const auto request_filter = gateway.value("request_filter", Json::object());
    config.gateway.http.request_filter.enabled = GetBool(request_filter, "enabled", true);
    config.gateway.http.request_filter.reject_control_chars = GetBool(request_filter, "reject_control_chars", true);
    config.gateway.http.request_filter.reject_suspicious_patterns = GetBool(request_filter, "reject_suspicious_patterns", true);

    const auto static_files = gateway.value("static_files", Json::object());
    if (GetBool(static_files, "enabled", true)) {
        net::StaticFileOptions files;
        files.root = ResolvePath(config_path, GetString(static_files, "root", "../dist"));
        files.index_file = GetString(static_files, "index_file", "index.html");
        files.spa_fallback = GetBool(static_files, "spa_fallback", true);
        config.gateway.static_files = files;
    }

    const auto auth = config.root.value("gateway_auth", Json::object());
    config.gateway.auth.enabled = GetBool(auth, "enabled", true);
    config.gateway.auth.allow_dev_identity = GetBool(auth, "allow_dev_identity", false);
    config.gateway.auth.require_auth_for_api = GetBool(auth, "require_auth_for_api", true);
    config.gateway.auth.enable_dev_registration = GetBool(
        auth, "enable_dev_registration", false);
    config.gateway.auth.cookie_name = GetString(auth, "cookie_name", "agent_auth");
    config.gateway.auth.issuer = GetString(auth, "issuer", "agent-e2e");
    config.gateway.auth.audience = GetString(auth, "audience", "agent-gateway");
    config.gateway.auth.clock_skew = std::chrono::seconds(GetInt(auth, "clock_skew_seconds", 60));
    config.gateway.auth.token_ttl = std::chrono::seconds(GetInt(auth, "token_ttl_seconds", 28800));
    config.gateway.auth.cookie_http_only = GetBool(auth, "cookie_http_only", true);
    config.gateway.auth.cookie_secure = GetBool(auth, "cookie_secure", false);
    config.gateway.auth.cookie_same_site = GetString(auth, "cookie_same_site", "Lax");
    config.gateway.auth.require_session_record = GetBool(auth, "require_session_record", false);
    config.gateway.auth.auto_provision_session = GetBool(auth, "auto_provision_session", true);
    config.gateway.auth.session_store_backend = GetString(auth, "session_store_backend", "sqlite");

    const auto session_db = GetString(auth, "session_database_path", "../tmp/persona_gateway_e2e_auth.db");
    config.gateway.auth.session_database_path = ResolvePath(config_path, session_db).string();
    config.gateway.auth.redis_host = GetString(auth, "redis_host", "127.0.0.1");
    config.gateway.auth.redis_port = GetString(auth, "redis_port", "5000");
    config.gateway.auth.redis_password = GetString(auth, "redis_password");
    config.gateway.auth.redis_pool_size = GetSize(auth, "redis_pool_size", 16);
    config.gateway.auth.redis_command_timeout = std::chrono::milliseconds(GetInt(auth, "redis_command_timeout_ms", 5000));
    config.gateway.auth.redis_key_prefix = GetString(auth, "redis_key_prefix", "agent:e2e:gateway:auth");

    config.gateway.auth.public_key_pem = GetString(auth, "public_key_pem");
    config.gateway.auth.private_key_pem = GetString(auth, "private_key_pem");
    const auto public_key_file = GetString(auth, "public_key_file");
    const auto private_key_file = GetString(auth, "private_key_file");
    if (!public_key_file.empty()) {
        config.gateway.auth.public_key_pem = ReadTextFile(ResolvePath(config_path, public_key_file));
    }
    if (!private_key_file.empty()) {
        config.gateway.auth.private_key_pem = ReadTextFile(ResolvePath(config_path, private_key_file));
    }
    if (config.gateway.auth.enabled &&
        (config.gateway.auth.public_key_pem.empty() || config.gateway.auth.private_key_pem.empty()) &&
        GetBool(auth, "generate_dev_keys", true)) {
        auto keys = GenerateRsaKeyPair();
        config.gateway.auth.private_key_pem = std::move(keys.private_key_pem);
        config.gateway.auth.public_key_pem = std::move(keys.public_key_pem);
    }

    const auto llm = config.root.value("llm", Json::object());
    config.cloud_llm_enabled = GetBool(llm, "enabled", true);
    config.async_http_io_threads = GetSize(llm, "async_http_io_threads", 1);
    config.async_http_keep_alive = GetBool(llm, "http_keep_alive", true);
    config.async_http_max_idle_connections = GetSize(
        llm, "http_max_idle_connections", 64);
    config.async_http_max_idle_connections_per_origin = GetSize(
        llm, "http_max_idle_connections_per_origin", 64);
    config.async_http_idle_timeout = std::chrono::milliseconds(
        GetInt(llm, "http_idle_timeout_ms", 30000));
    if (config.async_http_keep_alive &&
        (config.async_http_max_idle_connections == 0 ||
         config.async_http_max_idle_connections_per_origin == 0 ||
         config.async_http_max_idle_connections_per_origin >
             config.async_http_max_idle_connections ||
         config.async_http_idle_timeout <= std::chrono::milliseconds::zero())) {
        throw std::runtime_error("llm HTTP keep-alive pool options are invalid");
    }
    config.allow_placeholder_llm = GetBool(llm, "allow_placeholder", false);
    config.disable_tls_verify_on_windows = GetBool(llm, "disable_tls_verify_on_windows", true);
    config.cloud_llm.base_url = GetString(llm, "base_url");
    config.cloud_llm.default_model = GetString(llm, "model", "deepseek-chat");
    config.cloud_llm.timeout_ms = GetInt(llm, "timeout_ms", 30000);
    config.cloud_llm.retry_policy.max_retries = GetInt(llm, "max_retries", 2);
    config.cloud_llm.api_key = ResolveApiKey(config_path, llm);
    config.cloud_llm.require_api_key = GetBool(llm, "require_api_key", true);
    if (config.gateway.runtime.default_model.empty()) {
        config.gateway.runtime.default_model = config.cloud_llm.default_model;
    }

    const auto local = config.root.value("local_llm", Json::object());
    config.local_llm_enabled = GetBool(local, "enabled", false);
    config.local_llm.target = GetString(local, "target", "127.0.0.1:50051");
    config.local_llm.deadline = std::chrono::milliseconds(GetInt(local, "deadline_ms", 30000));
    config.local_llm.auth_token = GetString(local, "auth_token");
    config.local_llm.auth_metadata_key = GetString(local, "auth_metadata_key", "authorization");

    const auto embedding = config.root.value("embedding", Json::object());
    config.tokenizer_path = ResolvePath(config_path, GetString(embedding, "tokenizer_path", "../onnx_models/minilm/tokenizer.json"));
    config.embedding_model_path = ResolvePath(config_path, GetString(embedding, "model_path", "../onnx_models/minilm/model.onnx"));
    config.embedding_provider = GetString(embedding, "execution_provider", "auto");
    config.embedding_dimension = GetInt(embedding, "dimension", 384);

    const auto l0 = config.root.value("l0_memory", Json::object());
    config.l0_enabled = GetBool(l0, "enabled", true);
    config.l0_redis_host = GetString(l0, "redis_host", "127.0.0.1");
    config.l0_redis_port = GetInt(l0, "redis_port", 5000);
    config.l0_sqlite_path = ResolvePath(config_path, GetString(l0, "sqlite_path", "../data/persona_gateway_e2e/l0_memory.db"));
    config.l0_max_cached_records = GetSize(l0, "max_cached_records", 1000);
    config.l0_top_k = GetSize(l0, "top_k", 5);
    config.l0_neighbors_per_hit = GetSize(l0, "neighbors_per_hit", 1);
    auto similarity = l0.find("similarity_floor");
    if (similarity != l0.end() && similarity->is_number()) {
        config.l0_similarity_floor = similarity->get<float>();
    }
    config.l0_user_uuid = GetString(l0, "user_uuid", "e2e-l0");

    const auto emotion = config.root.value("emotion_analyzer", Json::object());
    const auto emotion_backend = GetString(emotion, "backend", "neutral");
    config.grpc_emotion_enabled = GetBool(emotion, "enabled", emotion_backend == "grpc" || emotion_backend == "grpc_multimodal");
    config.grpc_emotion.target = GetString(emotion, "target", "127.0.0.1:50051");
    config.grpc_emotion.deadline = std::chrono::milliseconds(GetInt(emotion, "deadline_ms", 3000));
    config.grpc_emotion.auth_token = GetString(emotion, "auth_token");
    config.grpc_emotion.auth_metadata_key = GetString(emotion, "auth_metadata_key", "authorization");
    config.grpc_emotion.tokenizer_options.max_length = GetSize(emotion, "max_length", 128);
    config.grpc_emotion.tokenizer_options.truncation = GetBool(emotion, "truncation", true);
    config.grpc_emotion.tokenizer_options.padding = GetBool(emotion, "padding", true);
    config.grpc_emotion.tokenizer_options.pad_to_longest_in_batch = false;
    config.grpc_emotion.tokenizer_options.add_special_tokens = GetBool(emotion, "add_special_tokens", true);
    config.emotion_tokenizer_path = ResolvePath(
        config_path,
        GetString(emotion, "tokenizer_path", config.tokenizer_path.string()));

    const auto emotion_fusion = config.root.value("emotion_fusion", Json::object());
    config.emotion_fusion_enabled = GetBool(emotion_fusion, "enabled", true);
    config.emotion_fusion.enabled = config.emotion_fusion_enabled;
    config.emotion_fusion.bert_weight =
        GetFloat(emotion_fusion, "bert_weight", static_cast<float>(config.emotion_fusion.bert_weight));
    config.emotion_fusion.default_reliability =
        GetFloat(emotion_fusion, "default_reliability", static_cast<float>(config.emotion_fusion.default_reliability));
    config.emotion_fusion.accept_confidence =
        GetFloat(emotion_fusion, "accept_confidence", static_cast<float>(config.emotion_fusion.accept_confidence));
    config.emotion_fusion.ambiguity_margin =
        GetFloat(emotion_fusion, "ambiguity_margin", static_cast<float>(config.emotion_fusion.ambiguity_margin));
    config.emotion_fusion.head_bias =
        GetFloat(emotion_fusion, "head_bias", static_cast<float>(config.emotion_fusion.head_bias));
    config.emotion_fusion.bert_signal_weight =
        GetFloat(emotion_fusion, "bert_signal_weight", static_cast<float>(config.emotion_fusion.bert_signal_weight));
    const auto evidence_weight =
        GetFloat(emotion_fusion, "evidence_signal_weight", static_cast<float>(config.emotion_fusion.keyword_signal_weight));
    config.emotion_fusion.keyword_signal_weight =
        GetFloat(emotion_fusion, "keyword_signal_weight", evidence_weight);
    config.emotion_fusion.vector_signal_weight =
        GetFloat(emotion_fusion, "vector_signal_weight", evidence_weight);
    config.emotion_fusion.llm_signal_weight =
        GetFloat(emotion_fusion, "llm_signal_weight", static_cast<float>(config.emotion_fusion.llm_signal_weight));
    config.emotion_fusion.margin_signal_weight =
        GetFloat(emotion_fusion, "margin_signal_weight", static_cast<float>(config.emotion_fusion.margin_signal_weight));
    config.emotion_fusion.llm_gate_confidence =
        GetFloat(emotion_fusion, "llm_gate_confidence", static_cast<float>(config.emotion_fusion.llm_gate_confidence));
    config.emotion_fusion.llm_gate_min_delta =
        GetFloat(emotion_fusion, "llm_gate_min_delta", static_cast<float>(config.emotion_fusion.llm_gate_min_delta));
    if (auto it = emotion_fusion.find("label_reliability"); it != emotion_fusion.end() && it->is_object()) {
        for (auto label = it->begin(); label != it->end(); ++label) {
            if (label.value().is_number()) {
                config.emotion_fusion.label_reliability[label.key()] = label.value().get<double>();
            }
        }
    }
    if (auto it = emotion_fusion.find("source_weights"); it != emotion_fusion.end() && it->is_object()) {
        for (auto source = it->begin(); source != it->end(); ++source) {
            if (source.value().is_number()) {
                config.emotion_fusion.source_weights[source.key()] = source.value().get<double>();
            }
        }
    }
    config.emotion_keyword_rules = ParseEmotionKeywordRules(emotion_fusion);
    config.emotion_vector_enabled = GetBool(emotion_fusion, "vector_enabled", false);

    const auto skill_session = config.root.value("skill_session", Json::object());
    config.skill_session_enabled = GetBool(skill_session, "enabled", true);
    config.skill_session_options.startup_timeout =
        std::chrono::milliseconds(GetInt(skill_session, "startup_timeout_ms", 15000));
    config.skill_session_options.max_duration =
        std::chrono::milliseconds(GetInt(skill_session, "max_duration_ms", 120000));
    config.skill_session_options.idle_timeout =
        std::chrono::milliseconds(GetInt(skill_session, "idle_timeout_ms", 60000));
    config.skill_session_options.closing_timeout =
        std::chrono::milliseconds(GetInt(skill_session, "closing_timeout_ms", 10000));
    config.skill_session_options.max_recent_observations =
        GetSize(skill_session, "max_recent_observations", 8);
    config.skill_session_cleanup_interval_seconds =
        GetInt(skill_session, "cleanup_interval_seconds", 30);

    return config;
}

struct LlmClientBundle {
    std::shared_ptr<agent::llm::ILlmClient> sync;
    std::shared_ptr<agent::llm::IAsyncLlmClient> async;
};

core::Result<LlmClientBundle> CreateLlmClient(const ToolConfig& config) {
    std::shared_ptr<agent::llm::ILlmClient> primary;
    std::shared_ptr<agent::llm::IAsyncLlmClient> async_primary;
    std::shared_ptr<agent::llm::ILlmClient> fallback;

    if (config.cloud_llm_enabled && !config.cloud_llm.base_url.empty()) {
        agent::net::TlsClientOptions tls_opts;
#ifdef _WIN32
        if (config.disable_tls_verify_on_windows) {
            tls_opts.verify_mode = agent::net::TlsVerifyMode::None;
        }
#endif
        auto tls = agent::net::TlsContext::CreateClient(tls_opts);
        if (!tls.ok()) {
            return tls.status();
        }
        auto tls_context = std::move(tls).value();
        agent::net::BeastHttpClientOptions http_opts;
        http_opts.tls_context = tls_context;
        auto http = agent::net::BeastHttpClient::Create(std::move(http_opts));
        if (!http.ok()) {
            return http.status();
        }
        auto http_client = std::shared_ptr<agent::net::IHttpClient>(std::move(http).value());

        auto cloud = agent::llm::OpenAiLlmClient::Create(config.cloud_llm, *http_client);
        if (!cloud.ok()) {
            return cloud.status();
        }
        struct ClientWithTransport final : public agent::llm::ILlmClient {
            std::shared_ptr<agent::net::IHttpClient> transport;
            std::unique_ptr<agent::llm::OpenAiLlmClient> client;
            core::Result<agent::llm::ChatCompletionResponse> Complete(
                const agent::llm::ChatCompletionRequest& req) override {
                return client->Complete(req);
            }
        };
        auto holder = std::make_shared<ClientWithTransport>();
        holder->transport = std::move(http_client);
        holder->client = std::move(cloud).value();
        primary = holder;

        agent::net::AsyncBeastHttpClientOptions async_http_options;
        async_http_options.tls_context = std::move(tls_context);
        async_http_options.io_thread_count = config.async_http_io_threads;
        async_http_options.enable_keep_alive = config.async_http_keep_alive;
        async_http_options.max_idle_connections = config.async_http_max_idle_connections;
        async_http_options.max_idle_connections_per_origin =
            config.async_http_max_idle_connections_per_origin;
        async_http_options.idle_connection_timeout = config.async_http_idle_timeout;
        auto async_http = agent::net::AsyncBeastHttpClient::Create(
            std::move(async_http_options));
        if (!async_http.ok()) {
            return async_http.status();
        }
        auto async_http_client = std::shared_ptr<agent::net::IAsyncHttpClient>(
            std::move(async_http).value());
        auto async_cloud = agent::llm::OpenAiAsyncLlmClient::Create(
            config.cloud_llm, *async_http_client);
        if (!async_cloud.ok()) {
            return async_cloud.status();
        }
        struct AsyncClientWithTransport final : public agent::llm::IAsyncLlmClient,
                                                public agent::llm::IAsyncStreamingLlmClient {
            std::shared_ptr<agent::net::IAsyncHttpClient> transport;
            std::unique_ptr<agent::llm::OpenAiAsyncLlmClient> client;

            core::Result<std::shared_ptr<agent::llm::IAsyncLlmOperation>> CompleteAsync(
                agent::llm::ChatCompletionRequest request,
                Callback callback) override {
                return client->CompleteAsync(std::move(request), std::move(callback));
            }
            core::Result<std::shared_ptr<agent::llm::IAsyncLlmOperation>> CompleteStreamingAsync(
                agent::llm::ChatCompletionRequest request, agent::llm::LlmEventSink sink, Callback callback) override {
                return client->CompleteStreamingAsync(std::move(request), std::move(sink), std::move(callback));
            }
        };
        auto async_holder = std::make_shared<AsyncClientWithTransport>();
        async_holder->transport = std::move(async_http_client);
        async_holder->client = std::move(async_cloud).value();
        async_primary = std::move(async_holder);
    }

    if (config.local_llm_enabled) {
        auto local_llm = std::make_shared<agent::llm::GrpcLocalLlmClient>(config.local_llm);
        agent::llm::LocalLlmChatClientOptions local_options;
        local_options.default_model = config.gateway.runtime.default_model.empty()
            ? "local-llm"
            : config.gateway.runtime.default_model;
        fallback = std::make_shared<agent::llm::LocalLlmChatClient>(std::move(local_llm), local_options);
    }

    if (primary && fallback) {
        // 本地 fallback 尚无异步组合接口，保留与生产装配一致的同步故障切换语义。
        return LlmClientBundle{
            .sync = std::make_shared<agent::llm::FallbackLlmClient>(primary, fallback),
        };
    }
    if (primary) {
        return LlmClientBundle{.sync = std::move(primary), .async = std::move(async_primary)};
    }
    if (fallback) {
        return LlmClientBundle{.sync = std::move(fallback)};
    }
    if (config.allow_placeholder_llm) {
        return LlmClientBundle{.sync = std::make_shared<PlaceholderLlmClient>()};
    }
    return core::Status::Error(core::ErrorCode::FailedPrecondition, "no LLM client configured");
}

core::Result<std::shared_ptr<vector::EmbeddingPipeline>> CreateEmbeddingPipeline(const ToolConfig& config) {
    if (!fs::exists(config.tokenizer_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "tokenizer not found: " + config.tokenizer_path.string());
    }
    if (!fs::exists(config.embedding_model_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "embedding model not found: " + config.embedding_model_path.string());
    }

    auto tokenizer = vector::HfTokenizer::LoadFromFile(config.tokenizer_path);
    if (!tokenizer.ok()) {
        return tokenizer.status();
    }
    auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(std::move(tokenizer).value());

    vector::EmbeddingModelOptions model_options;
    model_options.model_path = config.embedding_model_path;
    model_options.execution_provider = config.embedding_provider;
    model_options.allow_cpu_fallback = true;
    model_options.expected_dimension = static_cast<std::size_t>(config.embedding_dimension);
    model_options.pooling = vector::PoolingStrategy::Mean;
    model_options.normalize = true;
    auto model = vector::OnnxTextEmbeddingModel::Load(model_options);
    if (!model.ok()) {
        return model.status();
    }
    std::shared_ptr<vector::IEmbeddingModel> model_ptr(std::move(model).value());
    return std::make_shared<vector::EmbeddingPipeline>(std::move(tokenizer_ptr), std::move(model_ptr));
}

core::Result<L0MemoryCacheBundle> CreateL0MemoryCache(const ToolConfig& config) {
    if (!config.l0_enabled) {
        L0MemoryCacheBundle bundle;
        bundle.cache = std::make_shared<NoopSemanticCache>();
        return bundle;
    }
    if (!fs::exists(config.tokenizer_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "tokenizer not found: " + config.tokenizer_path.string());
    }
    if (!fs::exists(config.embedding_model_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "embedding model not found: " + config.embedding_model_path.string());
    }

    auto tokenizer = vector::HfTokenizer::LoadFromFile(config.tokenizer_path);
    if (!tokenizer.ok()) {
        return tokenizer.status();
    }
    auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(std::move(tokenizer).value());

    vector::EmbeddingModelOptions model_options;
    model_options.model_path = config.embedding_model_path;
    model_options.execution_provider = config.embedding_provider;
    model_options.allow_cpu_fallback = true;
    model_options.expected_dimension = static_cast<std::size_t>(config.embedding_dimension);
    model_options.pooling = vector::PoolingStrategy::Mean;
    model_options.normalize = true;
    auto model = vector::OnnxTextEmbeddingModel::Load(model_options);
    if (!model.ok()) {
        return model.status();
    }
    std::shared_ptr<vector::IEmbeddingModel> model_ptr(std::move(model).value());
    auto embedding = std::make_shared<vector::EmbeddingPipeline>(std::move(tokenizer_ptr), std::move(model_ptr));

    agent::semantic_cache::RedisPoolOptions redis_options;
    redis_options.host = config.l0_redis_host;
    redis_options.port = std::to_string(config.l0_redis_port);
    redis_options.pool_size = 4;
    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(redis_options);
    auto redis_start = redis->Start();
    if (!redis_start.ok()) {
        return redis_start;
    }

    fs::create_directories(config.l0_sqlite_path.parent_path());
    agent::semantic_cache::L0MemoryCacheAdapterOptions options;
    options.top_k = config.l0_top_k;
    options.neighbors_per_hit = config.l0_neighbors_per_hit;
    options.similarity_floor = config.l0_similarity_floor;
    L0MemoryCacheBundle bundle;
    bundle.redis_pool = redis;
    bundle.adapter = std::make_shared<agent::semantic_cache::L0MemoryCacheAdapter>(
        std::move(embedding),
        redis,
        config.l0_sqlite_path.string(),
        config.l0_max_cached_records,
        options);
    bundle.cache = bundle.adapter;
    return bundle;
}

core::Result<std::shared_ptr<agent::service::persona::IEmotionAnalyzer>> CreateEmotionAnalyzer(
    const ToolConfig& config,
    std::shared_ptr<vector::EmbeddingPipeline> embedding_pipeline) {
    std::shared_ptr<agent::service::persona::IEmotionAnalyzer> base;
    if (!config.grpc_emotion_enabled) {
        base = std::make_shared<agent::service::persona::NeutralEmotionAnalyzer>();
    } else {
        if (!fs::exists(config.emotion_tokenizer_path)) {
            return core::Status::Error(core::ErrorCode::NotFound, "emotion tokenizer not found: " + config.emotion_tokenizer_path.string());
        }
        auto tokenizer = vector::HfTokenizer::LoadFromFile(config.emotion_tokenizer_path);
        if (!tokenizer.ok()) {
            return tokenizer.status();
        }
        auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(std::move(tokenizer).value());
        base = std::make_shared<agent::service::persona::GrpcEmotionAnalyzer>(
            config.grpc_emotion,
            std::move(tokenizer_ptr));
    }

    if (!config.emotion_fusion_enabled) {
        return base;
    }

    std::vector<std::shared_ptr<agent::service::persona::IEmotionEvidenceProvider>> providers;
    providers.push_back(std::make_shared<agent::service::persona::KeywordEmotionEvidenceProvider>(
        config.emotion_keyword_rules));
    if (config.emotion_vector_enabled && embedding_pipeline) {
        auto vector_provider = agent::service::persona::VectorEmotionEvidenceProvider::Create(
            std::move(embedding_pipeline),
            agent::service::persona::DefaultEmotionVectorPrototypes());
        if (!vector_provider.ok()) {
            return vector_provider.status();
        }
        providers.push_back(std::move(vector_provider).value());
    }
    return std::shared_ptr<agent::service::persona::IEmotionAnalyzer>(
        std::make_shared<agent::service::persona::FusedEmotionAnalyzer>(
            std::move(base),
            config.emotion_fusion,
            std::move(providers)));
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    bool logging_initialized = false;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <config.json> [--no-stdin-stop] [--sync-llm]\n";
        return 2;
    }
    bool stop_on_stdin = true;
    bool force_sync_llm = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--no-stdin-stop") {
            stop_on_stdin = false;
        } else if (std::string_view(argv[i]) == "--sync-llm") {
            force_sync_llm = true;
        }
    }

    try {
        const fs::path config_path = argv[1];
        const auto repo_root = FindRepoRoot(config_path);
        const auto dump_dir = repo_root / "dumps";
        fs::create_directories(dump_dir);
        crash_dump::Install(dump_dir.string());

        auto config = LoadConfig(argv[1]);
        fs::create_directories(config.logging.log_dir);
        if (!logging::Initialize(config.logging)) {
            return Fail("logging initialization failed");
        }
        logging_initialized = true;
        LOG_INFO("[gateway-e2e] log file: {}", logging::GetLogFilePath(config.logging).string());
        LOG_INFO("[gateway-e2e] dump dir: {}", config.dump_dir.string());
        LOG_INFO("[gateway-e2e] config: {}", fs::absolute(config.config_path).string());

        auto llm = CreateLlmClient(config);
        if (!llm.ok()) {
            logging::Shutdown();
            return Fail("LLM client: " + llm.status().message());
        }

        auto cache = CreateL0MemoryCache(config);
        if (!cache.ok()) {
            logging::Shutdown();
            return Fail("L0 memory: " + cache.status().message());
        }
        auto l0_bundle = std::move(cache).value();
        LOG_INFO("[gateway-e2e] L0 memory: {} redis={}:{} tokenizer={} model={}",
                 config.l0_enabled ? "enabled" : "disabled",
                 config.l0_redis_host,
                 config.l0_redis_port,
                 config.tokenizer_path.string(),
                 config.embedding_model_path.string());
        auto memory = std::make_shared<agent::service::persona::SemanticMemoryContextProvider>(l0_bundle.cache);

        std::shared_ptr<vector::EmbeddingPipeline> emotion_vector_embedding;
        if (config.emotion_vector_enabled) {
            auto embedding = CreateEmbeddingPipeline(config);
            if (!embedding.ok()) {
                logging::Shutdown();
                return Fail("emotion vector embedding: " + embedding.status().message());
            }
            emotion_vector_embedding = std::move(embedding).value();
        }
        auto emotion = CreateEmotionAnalyzer(config, std::move(emotion_vector_embedding));
        if (!emotion.ok()) {
            logging::Shutdown();
            return Fail("emotion analyzer: " + emotion.status().message());
        }
        LOG_INFO("[gateway-e2e] emotion analyzer backend={} fusion={} vector={}",
                 config.grpc_emotion_enabled ? "grpc" : "neutral",
                 config.emotion_fusion_enabled,
                 config.emotion_vector_enabled);

        agent::service::gateway::PersonaGatewayServerDependencies dependencies;
        dependencies.memory_provider = std::move(memory);
        dependencies.l0_memory_adapter = l0_bundle.adapter;
        dependencies.emotion_analyzer = std::move(emotion).value();
        auto llm_bundle = std::move(llm).value();
        dependencies.llm_client = std::move(llm_bundle.sync);
        if (!force_sync_llm) {
            dependencies.async_llm_client = std::move(llm_bundle.async);
        }
        LOG_INFO("[gateway-e2e] Persona LLM execution mode={}",
                 force_sync_llm ? "sync" : "async");
        std::shared_ptr<agent::service::persona::SkillSessionManager> skill_sessions;
        if (config.skill_session_enabled) {
            skill_sessions = std::make_shared<agent::service::persona::SkillSessionManager>(
                config.skill_session_options,
                core::LoggerAdapter::ForModule("skill"));
            dependencies.skill_session_manager = skill_sessions;
            dependencies.maintenance_tasks.push_back(
                std::make_shared<agent::service::gateway::SkillSessionMaintenanceTask>(
                    skill_sessions,
                    std::chrono::seconds(config.skill_session_cleanup_interval_seconds),
                    core::LoggerAdapter::ForModule("gateway")));
            LOG_INFO("[gateway-e2e] skill session enabled startup_timeout_ms={} max_duration_ms={} idle_timeout_ms={}",
                     config.skill_session_options.startup_timeout.count(),
                     config.skill_session_options.max_duration.count(),
                     config.skill_session_options.idle_timeout.count());
        }

        if (config.gateway.auth.session_store_backend != "redis") {
            fs::create_directories(fs::path(config.gateway.auth.session_database_path).parent_path());
        }
        if (config.gateway.static_files) {
            if (!fs::exists(config.gateway.static_files->root)) {
                logging::Shutdown();
                return Fail("static dist root does not exist: " + config.gateway.static_files->root.string());
            }
        }

        agent::service::gateway::PersonaGatewayServer server(
            std::move(config.gateway),
            std::move(dependencies));
        auto start = server.Start();
        if (!start.ok()) {
            logging::Shutdown();
            return Fail("server start: " + start.message());
        }

        std::cout << "[gateway-e2e] started\n";
        std::cout << "[gateway-e2e] frontend: http://127.0.0.1:" << server.port() << "/\n";
        std::cout << "[gateway-e2e] auth:     POST http://127.0.0.1:" << server.port() << "/api/auth/register\n";
        std::cout << "[gateway-e2e] ws:       ws://127.0.0.1:" << server.port() << "/ws/session\n";
        std::cout << "[gateway-e2e] press Enter or Ctrl+C to stop\n";

        std::thread input_thread;
        if (stop_on_stdin) {
            input_thread = std::thread([] {
                std::string line;
                std::getline(std::cin, line);
                g_stop_requested.store(true);
            });
        }

        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        server.Stop();
        logging::Shutdown();
        logging_initialized = false;
        if (input_thread.joinable()) {
            input_thread.detach();
        }
        std::cout << "[gateway-e2e] stopped\n";
        return 0;
    } catch (const std::exception& e) {
        if (logging_initialized) {
            LOG_ERROR("[gateway-e2e] fatal: {}", e.what());
            logging::Shutdown();
        }
        return Fail(e.what());
    }
}
