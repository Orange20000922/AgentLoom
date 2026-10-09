// Production-style Persona Gateway server entry.
//
// Starts one C++ HTTP server that hosts static frontend files, Gateway API
// routes, WebSocket sessions, document analysis, cache layers, and runtime
// maintenance tasks.

#include "persona_gateway_server.h"
#include "../../skill/skill_registry.h"
#include "grpc_emotion_analyzer.h"
#include "emotion_fusion_analyzer.h"
#include "semantic_cache_types.h"
#include "document_llm_chunk_cache.h"
#include "isemantic_cache.h"
#include "openai_llm_client.h"
#if defined(AGENTLOOM_HAS_LOCAL_LLM)
#include "local_llm_client.h"
#endif
#include "beast_http_client.h"
#include "async_beast_http_client.h"
#include "tls_context.h"
#include "logger.h"
#include "redis_connection_pool.h"
#include "semantic_cache_pipeline.h"
#include "l0_memory_cache_adapter.h"
#include "sqlite/sqlite_connection.h"
#include "sqlite/sqlite_connection_pool.h"
#include "sqlite_vector_repository.h"
#include "vector_partition_registry.h"
#include "vector_index_manager.h"
#include "long_term_memory_compressor.h"
#include "embedding_pipeline.h"
#include "hf_tokenizer.h"
#include "onnx_text_embedding_model.h"
#include "tool_memory_provider.h"
#include "option_parser.h"
#include "server_options.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include "crash_dump.h"
#endif


#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::atomic_bool g_stop_requested{false};

int Fail(const std::string& msg) {
    std::cerr << "[agent-gateway] FAIL: " << msg << std::endl;
    return 1;
}

void OnSignal(int) {
    g_stop_requested.store(true);
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

template <typename T>
std::map<std::string, T> OrderedMap(const std::unordered_map<std::string, T>& input) {
    return std::map<std::string, T>(input.begin(), input.end());
}

std::vector<agent::service::gateway::PersonaMetadataRecord> BuildDefaultPersonas(
    const std::vector<GatewayPersonaConfigOptions>& configs) {
    std::vector<agent::service::gateway::PersonaMetadataRecord> records;
    records.reserve(configs.size());
    for (const auto& config : configs) {
        agent::service::gateway::PersonaMetadataRecord record;
        record.tenant_id = "server";
        record.user_uuid = "server";
        record.persona_id = config.persona_id;
        record.personality.name = config.persona_id;
        record.personality.description = config.description;
        record.personality.traits = config.traits;
        record.personality.openness = config.openness;
        record.personality.extraversion = config.extraversion;
        record.personality.humor_tendency = config.humor_tendency;
        record.personality.empathy_level = config.empathy_level;
        record.personality.curiosity_level = config.curiosity_level;
        record.personality.formality = config.formality;
        if (config.emotion_prompts) {
            agent::service::persona::EmotionPromptConfig prompts;
            prompts.emotion_map = OrderedMap(config.emotion_prompts->emotion_map);
            prompts.emotion_reliability = OrderedMap(config.emotion_prompts->emotion_reliability);
            if (!config.emotion_prompts->confidence_thresholds.empty()) {
                prompts.confidence_thresholds = OrderedMap(config.emotion_prompts->confidence_thresholds);
            }
            if (!config.emotion_prompts->intensity_levels.empty()) {
                prompts.intensity_levels = OrderedMap(config.emotion_prompts->intensity_levels);
            }
            record.emotion_prompt_config = std::move(prompts);
        }
        record.emotion_state_config.alpha = config.emotion_state.alpha;
        record.emotion_state_config.beta = config.emotion_state.beta;
        record.emotion_state_config.gamma = config.emotion_state.gamma;
        record.emotion_state_config.delta = config.emotion_state.delta;
        record.emotion_state_config.baseline_valence = config.emotion_state.baseline_valence;
        record.emotion_state_config.baseline_arousal = config.emotion_state.baseline_arousal;
        record.emotion_state_config.kappa = config.emotion_state.kappa;
        record.emotion_state_config.negativity_bias = config.emotion_state.negativity_bias;
        record.emotion_state_config.noise_sigma = config.emotion_state.noise_sigma;
        record.emotion_state_config.injection_threshold = config.emotion_state.injection_threshold;
        record.emotion_state_config.save_interval_turns = config.emotion_state.save_interval_turns;
        record.emotion_state_config.persist_to_l4 = config.emotion_state.persist_to_l4;
        records.push_back(std::move(record));
    }
    return records;
}

class PlaceholderLlmClient final : public agent::llm::ILlmClient {
public:
    core::Result<agent::llm::ChatCompletionResponse> Complete(
        const agent::llm::ChatCompletionRequest& req) override {
        agent::llm::ChatCompletionResponse response;
        response.id = "placeholder-e2e";
        response.model = req.model.empty() ? "placeholder-e2e" : req.model;
        response.content =
            "This is a placeholder E2E response from agent_gateway_server. "
            "Configure llm.base_url and an API key to measure the real maximum-latency chain.";
        response.completion_tokens = 32;
        response.total_tokens = 32;
        return response;
    }
};

class DocumentEmbeddingProvider final : public agent::document::IDocumentEmbeddingProvider {
public:
    explicit DocumentEmbeddingProvider(std::shared_ptr<vector::EmbeddingPipeline> pipeline)
        : pipeline_(std::move(pipeline)) {}

    core::Result<std::vector<float>> EmbedText(std::string_view text) override {
        return pipeline_->Encode(text);
    }

private:
    std::shared_ptr<vector::EmbeddingPipeline> pipeline_;
};

struct L0MemoryCacheBundle {
    std::shared_ptr<agent::semantic_cache::ISemanticCache> cache;
    std::shared_ptr<agent::semantic_cache::L0MemoryCacheAdapter> adapter;
    std::shared_ptr<agent::semantic_cache::RedisConnectionPool> redis_pool;
};

using ToolConfig = MultimodalServerOptions;

std::vector<agent::service::persona::EmotionKeywordRule> BuildEmotionKeywordRules(
    const std::vector<EmotionKeywordRuleConfigOptions>& rules) {
    auto out = agent::service::persona::DefaultEmotionKeywordRules();
    for (const auto& rule : rules) {
        if (!rule.label.empty() && !rule.pattern.empty()) {
            out.push_back({rule.label, rule.pattern, rule.score});
        }
    }
    return out;
}

agent::service::persona::EmotionFusionAnalyzerOptions BuildEmotionFusionOptions(
    const EmotionFusionConfigOptions& config) {
    agent::service::persona::EmotionFusionAnalyzerOptions options;
    options.enabled = config.enabled;
    options.bert_weight = config.bert_weight;
    options.default_reliability = config.default_reliability;
    options.accept_confidence = config.accept_confidence;
    options.ambiguity_margin = config.ambiguity_margin;
    options.head_bias = config.head_bias;
    options.bert_signal_weight = config.bert_signal_weight;
    options.keyword_signal_weight = config.keyword_signal_weight;
    options.vector_signal_weight = config.vector_signal_weight;
    options.llm_signal_weight = config.llm_signal_weight;
    options.margin_signal_weight = config.margin_signal_weight;
    options.llm_gate_confidence = config.llm_gate_confidence;
    options.llm_gate_min_delta = config.llm_gate_min_delta;
    options.source_weights.clear();
    for (const auto& [key, value] : config.source_weights) {
        options.source_weights[key] = value;
    }
    options.label_reliability.clear();
    for (const auto& [key, value] : config.label_reliability) {
        options.label_reliability[key] = value;
    }
    return options;
}

struct LlmClientBundle {
    std::shared_ptr<agent::llm::ILlmClient> sync;
    std::shared_ptr<agent::llm::IAsyncLlmClient> async;
};

class HfCompletionTokenCounter final : public agent::llm::ICompletionTokenCounter {
public:
    HfCompletionTokenCounter(std::shared_ptr<vector::HfTokenizer> tokenizer,
                             std::string expected_model)
        : tokenizer_(std::move(tokenizer)), expected_model_(std::move(expected_model)) {}

    core::Result<std::size_t> CountTokens(
        std::string_view model,
        const agent::llm::ChatCompletionResponse& response) const override {
        if (!expected_model_.empty() && model != expected_model_) {
            return core::Status::Error(
                core::ErrorCode::FailedPrecondition,
                "configured response tokenizer does not match model " + std::string(model));
        }
        std::size_t total = 0;
        const auto append_count = [this, &total](std::string_view text) -> core::Status {
            if (text.empty()) {
                return core::Status::Ok();
            }
            vector::EncodeOptions options;
            // UTF-8 字节数是不添加特殊 Token 时 Token 数的安全上界。
            options.max_length = std::max<std::size_t>(1, text.size());
            options.truncation = false;
            options.padding = false;
            options.pad_to_longest_in_batch = false;
            options.add_special_tokens = false;
            auto encoded = tokenizer_->Encode(text, options);
            if (!encoded.ok()) {
                return encoded.status();
            }
            if (encoded.value().sequence_length >
                std::numeric_limits<std::size_t>::max() - total) {
                return core::Status::Error(core::ErrorCode::ResourceExhausted,
                                           "completion token count overflow");
            }
            total += encoded.value().sequence_length;
            return core::Status::Ok();
        };

        if (response.reasoning_content) {
            if (auto status = append_count(*response.reasoning_content); !status.ok()) {
                return status;
            }
        }
        if (auto status = append_count(response.content); !status.ok()) {
            return status;
        }
        for (const auto& call : response.tool_calls) {
            if (auto status = append_count(call.name); !status.ok()) {
                return status;
            }
            if (auto status = append_count(call.arguments_json); !status.ok()) {
                return status;
            }
        }
        return total;
    }

private:
    std::shared_ptr<vector::HfTokenizer> tokenizer_;
    std::string expected_model_;
};

agent::llm::CompletionTokenValidationMode ParseTokenValidationMode(std::string_view mode) {
    if (mode == "audit") {
        return agent::llm::CompletionTokenValidationMode::Audit;
    }
    if (mode == "strict") {
        return agent::llm::CompletionTokenValidationMode::Strict;
    }
    return agent::llm::CompletionTokenValidationMode::Off;
}

core::Result<LlmClientBundle> CreateLlmClient(const ToolConfig& config,
                                              std::string_view default_model) {
    std::shared_ptr<agent::llm::ILlmClient> primary;
    std::shared_ptr<agent::llm::IAsyncLlmClient> async_primary;
    std::shared_ptr<agent::llm::ILlmClient> fallback;

    if (config.llm.enabled && !config.llm.base_url.empty()) {
        agent::net::TlsClientOptions tls_opts;
        if (!config.llm.ca_bundle_path.empty()) {
            fs::path bundle_path = config.llm.ca_bundle_path;
            if (bundle_path.is_relative() && !config.config_file_path.empty()) {
                bundle_path = config.config_file_path.parent_path() / bundle_path;
            }
            tls_opts.ca_bundle_path = bundle_path.string();
        }
#ifdef _WIN32
        if (config.llm.disable_tls_verify_on_windows) {
            LOG_WARN("[agent-gateway] TLS peer verification is disabled by configuration");
            tls_opts.verify_mode = agent::net::TlsVerifyMode::None;
        } else if (tls_opts.ca_bundle_path.empty()) {
            return core::Status::Error(
                core::ErrorCode::FailedPrecondition,
                "llm.ca_bundle_path is required for verified HTTPS on Windows OpenSSL");
        }
#endif
        auto tls = agent::net::TlsContext::CreateClient(tls_opts);
        if (!tls.ok()) {
            return tls.status();
        }
        agent::net::BeastHttpClientOptions http_opts;
        auto tls_context = std::move(tls).value();
        http_opts.tls_context = tls_context;
        auto http = agent::net::BeastHttpClient::Create(std::move(http_opts));
        if (!http.ok()) {
            return http.status();
        }
        auto http_client = std::shared_ptr<agent::net::IHttpClient>(std::move(http).value());

        agent::llm::OpenAiLlmClientOptions cloud_options;
        cloud_options.base_url = config.llm.base_url;
        cloud_options.api_key = config.llm.api_key;
        cloud_options.default_model = config.llm.model;
        cloud_options.timeout_ms = config.llm.timeout_ms;
        cloud_options.retry_policy.max_retries = config.llm.max_retries;
        cloud_options.require_api_key = config.llm.require_api_key;
        cloud_options.response_validation.token_count_mode =
            ParseTokenValidationMode(config.llm.response_token_count_mode);
        cloud_options.response_validation.max_token_difference =
            config.llm.response_max_token_difference;
        if (cloud_options.response_validation.token_count_mode !=
            agent::llm::CompletionTokenValidationMode::Off) {
            if (config.llm.response_tokenizer_path.empty()) {
                LOG_WARN("[agent-gateway] LLM token count audit disabled: tokenizer_path is empty");
            } else {
                auto tokenizer = vector::HfTokenizer::LoadFromFile(
                    config.llm.response_tokenizer_path);
                if (!tokenizer.ok()) {
                    if (cloud_options.response_validation.token_count_mode ==
                        agent::llm::CompletionTokenValidationMode::Strict) {
                        return tokenizer.status();
                    }
                    LOG_WARN("[agent-gateway] LLM token count audit disabled: {}",
                             tokenizer.status().message());
                } else {
                    auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(
                        std::move(tokenizer).value());
                    cloud_options.response_validation.token_counter =
                        std::make_shared<HfCompletionTokenCounter>(
                            std::move(tokenizer_ptr),
                            config.llm.response_tokenizer_model.empty()
                                ? config.llm.model
                                : config.llm.response_tokenizer_model);
                }
            }
        }
        auto cloud = agent::llm::OpenAiLlmClient::Create(cloud_options, *http_client);
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
        async_http_options.io_thread_count = config.llm.async_http_io_threads;
        async_http_options.enable_keep_alive = config.llm.http_keep_alive;
        async_http_options.max_idle_connections = config.llm.http_max_idle_connections;
        async_http_options.max_idle_connections_per_origin =
            config.llm.http_max_idle_connections_per_origin;
        async_http_options.idle_connection_timeout =
            std::chrono::milliseconds(config.llm.http_idle_timeout_ms);
        auto async_http = agent::net::AsyncBeastHttpClient::Create(std::move(async_http_options));
        if (!async_http.ok()) {
            return async_http.status();
        }
        auto async_http_client = std::shared_ptr<agent::net::IAsyncHttpClient>(
            std::move(async_http).value());
        auto async_cloud = agent::llm::OpenAiAsyncLlmClient::Create(
            cloud_options, *async_http_client);
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

#if defined(AGENTLOOM_HAS_LOCAL_LLM)
    if (config.local_llm.enabled) {
        agent::llm::GrpcLocalLlmClientOptions grpc_local_options;
        grpc_local_options.target = config.local_llm.target;
        grpc_local_options.deadline = std::chrono::milliseconds(config.local_llm.deadline_ms);
        grpc_local_options.auth_token = config.local_llm.auth_token;
        grpc_local_options.auth_metadata_key = config.local_llm.auth_metadata_key;
        auto local_llm = std::make_shared<agent::llm::GrpcLocalLlmClient>(grpc_local_options);
        agent::llm::LocalLlmChatClientOptions local_options;
        local_options.default_model = default_model.empty()
            ? "local-llm"
            : std::string(default_model);
        fallback = std::make_shared<agent::llm::LocalLlmChatClient>(std::move(local_llm), local_options);
    }
#else
    if (config.local_llm.enabled) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "local_llm is enabled but AgentLoom was built without local LLM support");
    }
#endif

    if (primary && fallback) {
        // Fallback 当前仍是同步组合接口；在完成异步 gRPC fallback 编排前保留既有语义。
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
    if (config.llm.allow_placeholder) {
        return LlmClientBundle{.sync = std::make_shared<PlaceholderLlmClient>()};
    }
    return core::Status::Error(core::ErrorCode::FailedPrecondition, "no LLM client configured");
}

core::Result<std::shared_ptr<agent::service::persona::IEmotionAnalyzer>> CreateEmotionAnalyzer(const ToolConfig& config) {
    if (!config.emotion_analyzer.enabled) {
        return std::shared_ptr<agent::service::persona::IEmotionAnalyzer>(
            std::make_shared<agent::service::persona::NeutralEmotionAnalyzer>());
    }
    if (!fs::exists(config.emotion_analyzer.tokenizer_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "emotion tokenizer not found: " + config.emotion_analyzer.tokenizer_path.string());
    }

    auto tokenizer = vector::HfTokenizer::LoadFromFile(config.emotion_analyzer.tokenizer_path);
    if (!tokenizer.ok()) {
        return tokenizer.status();
    }
    auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(std::move(tokenizer).value());
    agent::service::persona::GrpcEmotionAnalyzerOptions grpc_options;
    grpc_options.target = config.emotion_analyzer.target;
    grpc_options.deadline = std::chrono::milliseconds(config.emotion_analyzer.deadline_ms);
    grpc_options.auth_token = config.emotion_analyzer.auth_token;
    grpc_options.auth_metadata_key = config.emotion_analyzer.auth_metadata_key;
    grpc_options.tokenizer_options.max_length = config.emotion_analyzer.max_length;
    grpc_options.tokenizer_options.truncation = config.emotion_analyzer.truncation;
    grpc_options.tokenizer_options.padding = config.emotion_analyzer.padding;
    grpc_options.tokenizer_options.pad_to_longest_in_batch = false;
    grpc_options.tokenizer_options.add_special_tokens = config.emotion_analyzer.add_special_tokens;
    std::shared_ptr<agent::service::persona::IEmotionAnalyzer> grpc =
        std::make_shared<agent::service::persona::GrpcEmotionAnalyzer>(
            grpc_options,
            std::move(tokenizer_ptr));
    if (!config.emotion_fusion.enabled) {
        return grpc;
    }
    std::vector<std::shared_ptr<agent::service::persona::IEmotionEvidenceProvider>> providers;
    providers.push_back(std::make_shared<agent::service::persona::KeywordEmotionEvidenceProvider>(
        BuildEmotionKeywordRules(config.emotion_fusion.keyword_rules)));
    return std::shared_ptr<agent::service::persona::IEmotionAnalyzer>(
        std::make_shared<agent::service::persona::FusedEmotionAnalyzer>(
            std::move(grpc),
            BuildEmotionFusionOptions(config.emotion_fusion),
            std::move(providers)));
}

core::Result<std::shared_ptr<vector::OnnxTextEmbeddingModel>> CreateEmbeddingModel(
    const ToolConfig& config) {
    if (!fs::exists(config.embedding.onnx_model_path)) {
        return core::Status::Error(
            core::ErrorCode::NotFound,
            "embedding model not found: " + config.embedding.onnx_model_path);
    }

    vector::EmbeddingModelOptions model_options;
    model_options.model_path = config.embedding.onnx_model_path;
    model_options.execution_provider = config.embedding.execution_provider;
    model_options.allow_cpu_fallback = config.embedding.allow_cpu_fallback;
    model_options.cuda_device_id = config.embedding.cuda_device_id;
    model_options.intra_op_num_threads = config.embedding.intra_op_num_threads;
    model_options.inter_op_num_threads = config.embedding.inter_op_num_threads;
    model_options.expected_dimension = static_cast<std::size_t>(config.embedding.expected_dimension);
    if (config.embedding.pooling_strategy == "mean") {
        model_options.pooling = vector::PoolingStrategy::Mean;
    } else if (config.embedding.pooling_strategy == "cls") {
        model_options.pooling = vector::PoolingStrategy::Cls;
    } else {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "unsupported embedding pooling_strategy: " + config.embedding.pooling_strategy);
    }
    model_options.normalize = config.embedding.normalize;
    model_options.require_token_type_ids = config.embedding.require_token_type_ids;
    auto model = vector::OnnxTextEmbeddingModel::Load(std::move(model_options));
    if (!model.ok()) {
        return model.status();
    }
    return std::shared_ptr<vector::OnnxTextEmbeddingModel>(std::move(model).value());
}

core::Result<L0MemoryCacheBundle> CreateL0MemoryCache(
    const ToolConfig& config,
    std::shared_ptr<vector::OnnxTextEmbeddingModel> embedding_model) {
    if (!config.l0_memory.enabled) {
        L0MemoryCacheBundle bundle;
        bundle.cache = std::make_shared<NoopSemanticCache>();
        return bundle;
    }
    if (!fs::exists(config.embedding.tokenizer_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "tokenizer not found: " + config.embedding.tokenizer_path);
    }
    if (!embedding_model) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "shared embedding model is required");
    }

    auto tokenizer = vector::HfTokenizer::LoadFromFile(config.embedding.tokenizer_path);
    if (!tokenizer.ok()) {
        return tokenizer.status();
    }
    auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(std::move(tokenizer).value());

    auto embedding = std::make_shared<vector::EmbeddingPipeline>(
        std::move(tokenizer_ptr),
        std::move(embedding_model));

    agent::semantic_cache::RedisPoolOptions redis_options;
    redis_options.host = config.l0_memory.redis_host;
    redis_options.port = std::to_string(config.l0_memory.redis_port);
    redis_options.pool_size = 4;
    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(redis_options);
    auto redis_start = redis->Start();
    if (!redis_start.ok()) {
        return redis_start;
    }

    fs::create_directories(config.l0_memory.sqlite_path.parent_path());

    agent::semantic_cache::L0MemoryCacheAdapterOptions options;
    options.top_k = config.l0_memory.top_k;
    options.candidate_multiplier = config.l0_memory.candidate_multiplier;
    options.neighbors_per_hit = config.l0_memory.neighbors_per_hit;
    options.similarity_floor = config.l0_memory.similarity_floor;
    options.warm_window_seconds = config.l0_memory.warm_window_seconds;
    options.half_life_seconds = config.l0_memory.half_life_seconds;
    options.max_age_seconds = config.l0_memory.max_age_seconds;
    L0MemoryCacheBundle bundle;
    bundle.redis_pool = redis;
    bundle.adapter = std::make_shared<agent::semantic_cache::L0MemoryCacheAdapter>(
            std::move(embedding),
            redis,
            config.l0_memory.sqlite_path.string(),
            config.l0_memory.max_cached_records,
            options);
    bundle.cache = bundle.adapter;
    return bundle;
}

core::Result<std::shared_ptr<vector::EmbeddingPipeline>> CreateEmbeddingPipeline(
    const ToolConfig& config,
    std::shared_ptr<vector::OnnxTextEmbeddingModel> embedding_model) {
    if (!fs::exists(config.embedding.tokenizer_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "tokenizer not found: " + config.embedding.tokenizer_path);
    }
    if (!embedding_model) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "shared embedding model is required");
    }

    auto tokenizer = vector::HfTokenizer::LoadFromFile(config.embedding.tokenizer_path);
    if (!tokenizer.ok()) {
        return tokenizer.status();
    }
    auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(std::move(tokenizer).value());

    return std::make_shared<vector::EmbeddingPipeline>(
        std::move(tokenizer_ptr),
        std::move(embedding_model));
}

core::Result<std::shared_ptr<agent::document::IDocumentEmbeddingProvider>> CreateDocumentEmbeddingProvider(
    const ToolConfig& config,
    std::shared_ptr<vector::OnnxTextEmbeddingModel> embedding_model) {
    auto pipeline = CreateEmbeddingPipeline(config, std::move(embedding_model));
    if (!pipeline.ok()) {
        return pipeline.status();
    }
    return std::shared_ptr<agent::document::IDocumentEmbeddingProvider>(
        std::make_shared<DocumentEmbeddingProvider>(std::move(pipeline).value()));
}

core::Result<std::shared_ptr<agent::document::IDocumentLlmChunkCache>> CreateDocumentLlmChunkCache(
    const ToolConfig& config) {
    if (!config.document_llm_chunk_cache.enabled) {
        return std::shared_ptr<agent::document::IDocumentLlmChunkCache>{};
    }

    agent::semantic_cache::RedisPoolOptions redis_options;
    redis_options.host = config.document_llm_chunk_cache.redis_host;
    redis_options.port = std::to_string(config.document_llm_chunk_cache.redis_port);
    redis_options.pool_size = config.document_llm_chunk_cache.redis_pool_size;
    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(redis_options);
    auto status = redis->Start();
    if (!status.ok()) {
        return status;
    }

    agent::document::RedisDocumentLlmChunkCacheOptions options;
    options.key_prefix = config.document_llm_chunk_cache.key_prefix;
    options.ttl = std::chrono::seconds(config.document_llm_chunk_cache.ttl_seconds);
    return std::shared_ptr<agent::document::IDocumentLlmChunkCache>(
        std::make_shared<agent::document::RedisDocumentLlmChunkCache>(std::move(redis), std::move(options)));
}

core::Result<std::shared_ptr<agent::semantic_cache::ISemanticCache>> CreateDocumentSemanticCache(
    const ToolConfig& config,
    std::shared_ptr<vector::OnnxTextEmbeddingModel> embedding_model) {
    if (!config.document_semantic_cache.enabled) {
        return std::shared_ptr<agent::semantic_cache::ISemanticCache>{};
    }
    if (!fs::exists(config.embedding.tokenizer_path)) {
        return core::Status::Error(core::ErrorCode::NotFound, "tokenizer not found: " + config.embedding.tokenizer_path);
    }
    if (!embedding_model) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "shared embedding model is required");
    }

    auto tokenizer = vector::HfTokenizer::LoadFromFile(config.embedding.tokenizer_path);
    if (!tokenizer.ok()) {
        return tokenizer.status();
    }
    auto tokenizer_ptr = std::make_shared<vector::HfTokenizer>(std::move(tokenizer).value());

    agent::semantic_cache::RedisPoolOptions redis_options;
    redis_options.host = config.document_semantic_cache.redis_host;
    redis_options.port = std::to_string(config.document_semantic_cache.redis_port);
    redis_options.pool_size = 4;
    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(redis_options);
    auto redis_start = redis->Start();
    if (!redis_start.ok()) {
        return redis_start;
    }

    fs::create_directories(config.document_semantic_cache.sqlite_path.parent_path());
    storage::sqlite::SqliteConnectionPoolOptions sqlite_options;
    sqlite_options.path = config.document_semantic_cache.sqlite_path.string();
    sqlite_options.read_connection_count = 4;
    sqlite_options.write_connection_count = 1;
    sqlite_options.busy_timeout_ms = 5000;
    sqlite_options.enable_wal = true;
    auto sqlite_pool = std::make_shared<storage::sqlite::SqliteConnectionPool>(std::move(sqlite_options));
    if (auto status = sqlite_pool->Start(); !status.ok()) {
        redis->Shutdown();
        return status;
    }

    auto index = std::make_shared<agent::semantic_cache::cache_vector::VectorIndexManager>(
        config.document_semantic_cache.user_uuid,
        redis,
        std::move(sqlite_pool),
        config.document_semantic_cache.max_cached_records);

    agent::semantic_cache::SemanticCachePipelineDeps deps;
    deps.tokenizer = std::move(tokenizer_ptr);
    deps.embedding_model = std::move(embedding_model);
    deps.index_manager = std::move(index);

    agent::semantic_cache::SemanticCachePipelineOptions options;
    options.top_k = config.document_semantic_cache.top_k;
    options.similarity_floor = config.document_semantic_cache.similarity_floor;
    options.enable_global_scope = true;

    auto pipeline = agent::semantic_cache::SemanticCachePipeline::Create(std::move(options), std::move(deps));
    if (!pipeline.ok()) {
        redis->Shutdown();
        return pipeline.status();
    }
    return std::shared_ptr<agent::semantic_cache::ISemanticCache>(std::move(pipeline).value());
}

core::Result<std::shared_ptr<agent::memory::LongTermMemoryCompressor>> CreateL3MemoryCompressor(
    const ToolConfig& config,
    std::shared_ptr<agent::llm::ILlmClient> llm_client,
    std::shared_ptr<vector::OnnxTextEmbeddingModel> embedding_model) {
    if (!config.l3_memory.enabled) {
        return std::shared_ptr<agent::memory::LongTermMemoryCompressor>{};
    }

    agent::semantic_cache::RedisPoolOptions redis_options;
    redis_options.host = config.l0_memory.redis_host;
    redis_options.port = std::to_string(config.l0_memory.redis_port);
    redis_options.pool_size = 4;
    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(redis_options);
    auto redis_start = redis->Start();
    if (!redis_start.ok()) {
        return redis_start;
    }

    fs::create_directories(config.l3_memory.sqlite_path.parent_path());
    storage::sqlite::SqliteConnectionPoolOptions pool_options;
    pool_options.path = config.l3_memory.sqlite_path.string();
    pool_options.read_connection_count = 4;
    pool_options.write_connection_count = 1;
    pool_options.enable_wal = true;
    auto sqlite_pool = std::make_shared<storage::sqlite::SqliteConnectionPool>(pool_options);
    auto sqlite_start = sqlite_pool->Start();
    if (!sqlite_start.ok()) {
        redis->Shutdown();
        return sqlite_start;
    }

    auto vector_repo = std::make_shared<agent::vector_storage::SqliteVectorRepository>(sqlite_pool);
    auto schema_status = vector_repo->EnsureSchema();
    if (!schema_status.ok()) {
        sqlite_pool->Close();
        redis->Shutdown();
        return schema_status;
    }

    agent::vector_storage::CollectionDescriptor collection;
    collection.name = config.l3_memory.collection_name;
    collection.embedding_model_fingerprint = config.l3_memory.embedding_fingerprint;
    collection.tokenizer_fingerprint = config.l3_memory.tokenizer_fingerprint;
    collection.pooling_strategy = config.embedding.pooling_strategy;
    collection.normalization = config.embedding.normalize ? "l2" : "none";
    collection.dimension = static_cast<std::size_t>(config.embedding.expected_dimension);
    collection.corpus_version = config.l3_memory.corpus_version;
    collection.policy_version = config.l3_memory.policy_version;
    auto collection_id = vector_repo->EnsureCollection(collection);
    if (!collection_id.ok()) {
        sqlite_pool->Close();
        redis->Shutdown();
        return collection_id.status();
    }

    auto partition_registry = std::make_shared<agent::vector_storage::PartitionRegistry>(vector_repo);

    fs::create_directories(config.l3_memory.registry_sqlite_path.parent_path());
    storage::sqlite::SqliteConnectionPoolOptions registry_pool_options;
    registry_pool_options.path = config.l3_memory.registry_sqlite_path.string();
    registry_pool_options.read_connection_count = 2;
    registry_pool_options.write_connection_count = 1;
    registry_pool_options.enable_wal = true;
    auto registry_pool = std::make_shared<storage::sqlite::SqliteConnectionPool>(registry_pool_options);
    auto registry_start = registry_pool->Start();
    if (!registry_start.ok()) {
        sqlite_pool->Close();
        redis->Shutdown();
        return registry_start;
    }

    agent::vector::IndexManagerOptions index_options;
    index_options.max_resident_partitions = config.l3_memory.max_resident_partitions;
    index_options.backend = config.l3_memory.index_backend;
    index_options.log_hydration = false;
    auto index_manager = std::make_shared<agent::vector::VectorIndexManager>(
        vector_repo,
        partition_registry,
        static_cast<std::size_t>(config.embedding.expected_dimension),
        index_options);

    auto embedding_pipeline = CreateEmbeddingPipeline(config, std::move(embedding_model));
    if (!embedding_pipeline.ok()) {
        sqlite_pool->Close();
        redis->Shutdown();
        return embedding_pipeline.status();
    }

    agent::memory::LongTermMemoryCompressorOptions compressor_options;
    compressor_options.redis_pool = std::move(redis);
    compressor_options.llm_client = std::move(llm_client);
    compressor_options.vector_repo = std::move(vector_repo);
    compressor_options.partition_registry = std::move(partition_registry);
    compressor_options.index_manager = std::move(index_manager);
    compressor_options.embedding_pipeline = std::move(embedding_pipeline).value();
    compressor_options.collection_id = collection_id.value();
    compressor_options.mode = agent::memory::CompressorMode::Production;
    compressor_options.max_records_per_batch = config.l3_memory.max_records_per_batch;
    compressor_options.compression_temperature = config.l3_memory.compression_temperature;
    compressor_options.compression_max_tokens = config.l3_memory.compression_max_tokens;
    compressor_options.registry_pool = std::move(registry_pool);

    auto compressor = agent::memory::LongTermMemoryCompressor::Create(std::move(compressor_options));
    if (!compressor.ok()) {
        sqlite_pool->Close();
        return compressor.status();
    }
    return std::shared_ptr<agent::memory::LongTermMemoryCompressor>(std::move(compressor).value());
}

// 构造 L4 工具记忆 provider：建 L4 sqlite/collection/partition，为每个 skill 写入一条
// 能力种子记忆，并以 keyword 正则 + 向量双通道召回。与 skill_llm_e2e_test 的构造流程一致。
core::Result<std::shared_ptr<agent::service::persona::IToolMemoryProvider>> CreateL4ToolMemoryProvider(
    const ToolConfig& config,
    std::shared_ptr<vector::OnnxTextEmbeddingModel> embedding_model) {
    // 空 sqlite 路径或无 skill 时跳过 L4 工具记忆。
    if (config.skills.tool_memory_sqlite_path.empty() || config.skill_manifests.empty()) {
        return std::shared_ptr<agent::service::persona::IToolMemoryProvider>{};
    }
    if (!embedding_model) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "shared embedding model is required for L4 tool memory");
    }
    auto embedding_pipeline = CreateEmbeddingPipeline(config, std::move(embedding_model));
    if (!embedding_pipeline.ok()) return embedding_pipeline.status();

    fs::create_directories(config.skills.tool_memory_sqlite_path.parent_path());
    storage::sqlite::SqliteConnectionPoolOptions pool_options;
    pool_options.path = config.skills.tool_memory_sqlite_path.string();
    pool_options.read_connection_count = 2;
    pool_options.write_connection_count = 1;
    pool_options.enable_wal = true;
    auto sqlite_pool = std::make_shared<storage::sqlite::SqliteConnectionPool>(pool_options);
    if (auto status = sqlite_pool->Start(); !status.ok()) return status;

    auto repository = std::make_shared<agent::vector_storage::SqliteVectorRepository>(sqlite_pool);
    if (auto status = repository->EnsureSchema(); !status.ok()) return status;

    agent::vector_storage::CollectionDescriptor collection;
    collection.name = config.skills.tool_memory_collection_name;
    collection.embedding_model_fingerprint = "configured";
    collection.tokenizer_fingerprint = "configured";
    collection.pooling_strategy = config.embedding.pooling_strategy;
    collection.normalization = config.embedding.normalize ? "l2" : "none";
    collection.dimension = static_cast<std::size_t>(config.embedding.expected_dimension);
    collection.corpus_version = "skill-l4";
    collection.policy_version = "skill-l4";
    auto collection_id = repository->EnsureCollection(collection);
    if (!collection_id.ok()) return collection_id.status();

    auto partition_registry = std::make_shared<agent::vector_storage::PartitionRegistry>(repository);
    agent::vector_storage::PartitionKey partition_key;
    partition_key.collection_id = collection_id.value();
    partition_key.memory_level = "L4";
    auto partition = partition_registry->Resolve(partition_key);
    if (!partition.ok()) return partition.status();

    // 为每个 skill 写入一条 L4 能力种子记忆（语义描述 embedding + 关键词正则触发）。
    for (const auto& manifest : config.skill_manifests) {
        const std::string& payload =
            manifest.l4_payload.empty() ? manifest.description : manifest.l4_payload;
        if (payload.empty()) continue;
        std::string memory_hash = "global:l4:" + manifest.skill_id;
        if (!manifest.intent.empty()) memory_hash += ":intent." + manifest.intent;

        // 幂等：同 memory_hash 种子已存在则跳过（也省去重复 embedding）。
        if (auto existing = repository->FindEntryIdByMemoryHash(partition.value(), memory_hash);
            !existing.ok()) {
            return existing.status();
        } else if (existing.value().has_value()) {
            continue;
        }

        auto encoded = embedding_pipeline.value()->Encode(payload);
        if (!encoded.ok()) return encoded.status();
        agent::vector_storage::EntryRecord entry;
        entry.partition_id = partition.value();
        entry.cache_key = manifest.skill_id;
        entry.text_hash = "skill-l4-canonical";
        entry.content_hash = "skill-l4-canonical";
        entry.memory_hash = std::move(memory_hash);
        entry.vector = std::move(encoded).value();
        entry.memory_type = "capability";
        entry.emotion_intensity = 0.0F;
        entry.payload = payload;
        nlohmann::json meta{
            {"skill_id", manifest.skill_id},
            {"tool_id", manifest.skill_id},
            {"priority", 100},
            {"instruction", manifest.prompt_instruction},
            {"schema", manifest.input_schema_json},
        };
        entry.extra_metadata_json = meta.dump();
        if (auto status = repository->InsertEntry(std::move(entry)); !status.ok()) return status.status();
    }

    agent::vector::IndexManagerOptions index_options;
    index_options.max_resident_partitions = 8;
    index_options.log_hydration = false;
    auto index_manager = std::make_shared<agent::vector::VectorIndexManager>(
        repository, partition_registry,
        static_cast<std::size_t>(config.embedding.expected_dimension), index_options);

    agent::service::persona::VectorToolMemoryProviderOptions provider_options;
    provider_options.collection_id = collection_id.value();
    provider_options.top_k = config.skills.tool_memory_top_k;
    provider_options.min_score = config.skills.tool_memory_min_score;
    provider_options.enable_regex = true;
    provider_options.enable_vector = true;
    for (const auto& manifest : config.skill_manifests) {
        agent::service::persona::ToolKeywordTrigger trigger;
        trigger.tool_id = manifest.skill_id;
        trigger.instruction = manifest.prompt_instruction;
        trigger.schema_json = manifest.input_schema_json;
        trigger.keywords = manifest.keywords;
        trigger.negative_keywords = manifest.negative_keywords;
        provider_options.keyword_triggers.push_back(std::move(trigger));
    }

    return std::shared_ptr<agent::service::persona::IToolMemoryProvider>(
        std::make_shared<agent::service::persona::VectorToolMemoryProvider>(
            std::move(embedding_pipeline).value(),
            std::move(index_manager),
            std::move(partition_registry),
            std::move(provider_options)));
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    bool logging_initialized = false;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config.json> [--no-stdin-stop]\n";
        return 2;
    }
    bool stop_on_stdin = true;
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--no-stdin-stop") {
            stop_on_stdin = false;
        }
    }

    try {
        const fs::path config_path = argv[1];
        const auto repo_root = FindRepoRoot(config_path);
        const auto dump_dir = repo_root / "dumps";
        fs::create_directories(dump_dir);
#ifdef _WIN32
        crash_dump::Install(dump_dir.string());
#endif

        auto config = ParseMultimodalOptions(argc, argv);
        auto gateway_options = ToPersonaGatewayServerOptions<agent::service::gateway::PersonaGatewayServerOptions, net::StaticFileOptions>(config);
        gateway_options.embedding_batch.enabled = config.embedding.batch_enabled;
        gateway_options.embedding_batch.max_pending_requests =
            config.embedding.batch_max_pending_requests;
        gateway_options.embedding_batch.max_batch_size = config.embedding.batch_max_size;
        gateway_options.embedding_batch.max_batch_wait =
            std::chrono::milliseconds(config.embedding.batch_max_wait_ms);
        gateway_options.embedding_batch.max_inflight_batches =
            config.embedding.batch_max_inflight;
        gateway_options.default_personas = BuildDefaultPersonas(config.persona_gateway.personas);
        if (gateway_options.auth.enabled &&
            config.gateway_auth.generate_dev_keys &&
            (gateway_options.auth.public_key_pem.empty() || gateway_options.auth.private_key_pem.empty())) {
            auto keys = agent::service::gateway::GenerateDevelopmentRsaKeyPair();
            if (!keys.ok()) {
                return Fail("gateway auth development key generation: " + keys.status().message());
            }
            gateway_options.auth.private_key_pem = std::move(keys.value().private_key_pem);
            gateway_options.auth.public_key_pem = std::move(keys.value().public_key_pem);
        }
        agent::service::persona::SkillSessionOptions skill_options;
        skill_options.startup_timeout = std::chrono::milliseconds(config.skill_session.startup_timeout_ms);
        skill_options.max_duration = std::chrono::milliseconds(config.skill_session.max_duration_ms);
        skill_options.idle_timeout = std::chrono::milliseconds(config.skill_session.idle_timeout_ms);
        skill_options.closing_timeout = std::chrono::milliseconds(config.skill_session.closing_timeout_ms);
        skill_options.max_recent_observations = config.skill_session.max_recent_observations;
        fs::create_directories(config.logging.log_dir);
        logging::LoggerOptions logger_options;
        logger_options.log_dir = config.logging.log_dir;
        logger_options.logger_name = config.logging.logger_name;
        logger_options.file_name = config.logging.file_name;
        logger_options.enable_console = config.logging.enable_console;
        logger_options.use_daily_rotation = config.logging.use_daily_rotation;
        logger_options.max_file_size_bytes = config.logging.max_file_size_bytes;
        logger_options.max_files = config.logging.max_files;
        logger_options.module_names = {"gateway", "service", "classroom", "gateway-auth"};
        if (!logging::Initialize(logger_options)) {
            return Fail("logging initialization failed");
        }
        logging_initialized = true;
        LOG_INFO("[agent-gateway] log file: {}", logging::GetLogFilePath(logger_options).string());
        LOG_INFO("[agent-gateway] dump dir: {}", dump_dir.string());
        LOG_INFO("[agent-gateway] config: {}", fs::absolute(config.config_file_path).string());

        auto llm = CreateLlmClient(config, gateway_options.runtime.default_model);
        if (!llm.ok()) {
            logging::Shutdown();
            return Fail("LLM client: " + llm.status().message());
        }
        auto llm_bundle = std::move(llm).value();
        auto llm_client = llm_bundle.sync;

        auto loaded_embedding_model = CreateEmbeddingModel(config);
        if (!loaded_embedding_model.ok()) {
            logging::Shutdown();
            return Fail("embedding model: " + loaded_embedding_model.status().message());
        }
        auto embedding_model = std::move(loaded_embedding_model).value();
        LOG_INFO("[agent-gateway] embedding model loaded once provider={} model={}",
                 embedding_model->GetActiveExecutionProvider(),
                 config.embedding.onnx_model_path);

        auto l0 = CreateL0MemoryCache(config, embedding_model);
        if (!l0.ok()) {
            logging::Shutdown();
            return Fail("L0 memory: " + l0.status().message());
        }
        auto l0_bundle = std::move(l0).value();
        LOG_INFO("[agent-gateway] L0 memory: {} redis={}:{} tokenizer={} model={}",
                 config.l0_memory.enabled ? "enabled" : "disabled",
                 config.l0_memory.redis_host,
                 config.l0_memory.redis_port,
                 config.embedding.tokenizer_path,
                 config.embedding.onnx_model_path);

        auto l3 = CreateL3MemoryCompressor(config, llm_client, embedding_model);
        if (!l3.ok()) {
            logging::Shutdown();
            return Fail("L3 memory: " + l3.status().message());
        }
        auto l3_memory = std::move(l3).value();
        if (l3_memory) {
            LOG_INFO("[agent-gateway] L3 memory enabled db={} users={}",
                     config.l3_memory.sqlite_path.string(),
                     config.l3_memory.user_uuids.size());
        }

        auto document_embedding = CreateDocumentEmbeddingProvider(config, embedding_model);
        if (!document_embedding.ok()) {
            logging::Shutdown();
            return Fail("document embedding: " + document_embedding.status().message());
        }
        auto document_llm_chunk_cache = CreateDocumentLlmChunkCache(config);
        if (!document_llm_chunk_cache.ok()) {
            logging::Shutdown();
            return Fail("document LLM chunk cache: " + document_llm_chunk_cache.status().message());
        }
        auto document_semantic_cache = CreateDocumentSemanticCache(config, embedding_model);
        if (!document_semantic_cache.ok()) {
            logging::Shutdown();
            return Fail("document semantic cache: " + document_semantic_cache.status().message());
        }
        if (document_semantic_cache.value()) {
            LOG_INFO("[agent-gateway] document semantic cache enabled redis={}:{} db={}",
                     config.document_semantic_cache.redis_host,
                     config.document_semantic_cache.redis_port,
                     config.document_semantic_cache.sqlite_path.string());
        }

        agent::service::persona::SemanticMemoryContextProviderOptions memory_options;
        auto memory = std::make_shared<agent::service::persona::SemanticMemoryContextProvider>(
            l0_bundle.cache,
            l3_memory,
            memory_options);
        auto emotion = CreateEmotionAnalyzer(config);
        if (!emotion.ok()) {
            logging::Shutdown();
            return Fail("emotion analyzer: " + emotion.status().message());
        }
        LOG_INFO("[agent-gateway] emotion analyzer backend={}",
                 config.emotion_analyzer.enabled ? "grpc" : "neutral");

        agent::service::gateway::PersonaGatewayServerDependencies dependencies;
        auto skill_registry = std::make_shared<agent::skill::InMemorySkillRegistry>();
        if (config.skills.enabled) {
            if (auto status = skill_registry->RegisterAll(config.skill_manifests); !status.ok()) {
                logging::Shutdown();
                return Fail("Skill manifest registration: " + status.message());
            }
        }
        dependencies.skill_registry = skill_registry;
        LOG_INFO("[agent-gateway] Skill manifests registered count={} directory={} regex={}",
                 config.skill_manifests.size(),
                 config.skills.manifest_directory.string(),
                 config.skills.manifest_filename_regex);
        auto l4_tool_memory = CreateL4ToolMemoryProvider(config, embedding_model);
        if (!l4_tool_memory.ok()) {
            logging::Shutdown();
            return Fail("L4 tool memory: " + l4_tool_memory.status().message());
        }
        dependencies.tool_memory_provider = std::move(l4_tool_memory).value();
        if (dependencies.tool_memory_provider) {
            LOG_INFO("[agent-gateway] L4 tool memory seeded collection={}",
                     config.skills.tool_memory_collection_name);
        }
        dependencies.memory_provider = std::move(memory);
        dependencies.emotion_analyzer = std::move(emotion).value();
        dependencies.llm_client = llm_client;
        dependencies.async_llm_client = std::move(llm_bundle.async);
        dependencies.document_embedding_provider = std::move(document_embedding).value();
        dependencies.document_llm_chunk_cache = std::move(document_llm_chunk_cache).value();
        dependencies.document_semantic_cache = std::move(document_semantic_cache).value();
        dependencies.l0_memory_adapter = l0_bundle.adapter;
        std::shared_ptr<agent::service::persona::SkillSessionManager> skill_sessions;
        if (config.skill_session.enabled) {
            skill_sessions = std::make_shared<agent::service::persona::SkillSessionManager>(
                skill_options,
                core::LoggerAdapter::ForModule("skill"));
            dependencies.skill_session_manager = skill_sessions;
            // 静态注册的有状态 Skill 共享同一个公共 Session manager；每次 Start 再创建独立 execution。
            dependencies.stateful_skill_router =
                std::make_shared<agent::service::persona::StatefulSkillExecutionRouter>(skill_sessions);
            dependencies.maintenance_tasks.push_back(
                std::make_shared<agent::service::gateway::SkillSessionMaintenanceTask>(
                    skill_sessions,
                    std::chrono::seconds(config.skill_session.cleanup_interval_seconds),
                    core::LoggerAdapter::ForModule("gateway")));
            LOG_INFO("[agent-gateway] skill session enabled startup_timeout_ms={} max_duration_ms={} idle_timeout_ms={}",
                     skill_options.startup_timeout.count(),
                     skill_options.max_duration.count(),
                     skill_options.idle_timeout.count());
        }

        if (gateway_options.auth.session_store_backend != "redis") {
            fs::create_directories(fs::path(gateway_options.auth.session_database_path).parent_path());
        }
        if (gateway_options.static_files) {
            if (!fs::exists(gateway_options.static_files->root)) {
                logging::Shutdown();
                return Fail("static dist root does not exist: " + gateway_options.static_files->root.string());
            }
        }

        agent::service::gateway::PersonaGatewayServer server(
            std::move(gateway_options),
            std::move(dependencies));
        if (l3_memory && config.l3_flush_scheduler.enabled) {
            agent::service::gateway::L3MemoryFlushMaintenanceOptions flush_options;
            flush_options.interval = std::chrono::seconds(config.l3_flush_scheduler.check_interval_seconds);
            flush_options.flush_hour = config.l3_flush_scheduler.flush_hour;
            flush_options.flush_minute = config.l3_flush_scheduler.flush_minute;
            flush_options.flush_date_offset_days = config.l3_flush_scheduler.flush_date_offset_days;
            flush_options.defer_when_sessions_active = config.l3_flush_scheduler.defer_when_sessions_active;
            for (const auto& user_id : config.l3_memory.user_uuids) {
                flush_options.owners.push_back({"default", user_id});
            }
            auto task = std::make_shared<agent::service::gateway::L3MemoryFlushMaintenanceTask>(
                l3_memory,
                server.sessions(),
                std::move(flush_options));
            auto register_task = server.RegisterMaintenanceTask(std::move(task));
            if (!register_task.ok()) {
                logging::Shutdown();
                return Fail("L3 flush maintenance: " + register_task.message());
            }
        }
        auto start = server.Start();
        if (!start.ok()) {
            logging::Shutdown();
            return Fail("server start: " + start.message());
        }

        std::cout << "[agent-gateway] started\n";
        std::cout << "[agent-gateway] frontend: http://127.0.0.1:" << server.port() << "/\n";
        if (config.gateway_auth.enable_dev_registration) {
            std::cout << "[agent-gateway] dev auth: POST http://127.0.0.1:" << server.port() << "/api/auth/register\n";
        }
        std::cout << "[agent-gateway] ws:       ws://127.0.0.1:" << server.port() << "/ws/session\n";
        std::cout << "[agent-gateway] press Enter or Ctrl+C to stop\n";

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
        std::cout << "[agent-gateway] stopped\n";
        return 0;
    } catch (const std::exception& e) {
        if (logging_initialized) {
            LOG_ERROR("[agent-gateway] fatal: {}", e.what());
            logging::Shutdown();
        }
        return Fail(e.what());
    }
}
