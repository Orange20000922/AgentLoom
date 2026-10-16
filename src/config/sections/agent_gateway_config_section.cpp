#include "config_section.h"

#include <stdexcept>
#include <unordered_set>

namespace server_config {
namespace {

DECLARE_CONFIG_SECTION(AgentGatewayConfigSection, "agent_gateway")
    void Validate(MultimodalServerOptions& options) const override;
};

std::filesystem::path ResolveRelativeToConfig(const std::filesystem::path& path,
                                              const std::filesystem::path& config_path) {
    if (path.empty() || path.is_absolute() || config_path.empty()) {
        return path;
    }
    return config_path.parent_path() / path;
}

void LoadLogging(const Json& root, MultimodalServerOptions& options) {
    const Json* section = FindSection(root, "logging");
    if (!section) {
        return;
    }
    SetPath(*section, "logging", "log_dir", options.logging.log_dir);
    SetString(*section, "logging", "logger_name", options.logging.logger_name);
    SetString(*section, "logging", "file_name", options.logging.file_name);
    SetBool(*section, "logging", "enable_console", options.logging.enable_console);
    SetBool(*section, "logging", "use_daily_rotation", options.logging.use_daily_rotation);
    SetSize(*section, "logging", "max_file_size_bytes", options.logging.max_file_size_bytes, 1);
    SetSize(*section, "logging", "max_files", options.logging.max_files, 1);
}

void LoadLocalLlm(const Json& root, MultimodalServerOptions& options) {
    const Json* section = FindSection(root, "local_llm");
    if (!section) {
        return;
    }
    SetBool(*section, "local_llm", "enabled", options.local_llm.enabled);
    SetString(*section, "local_llm", "target", options.local_llm.target);
    SetInt(*section, "local_llm", "deadline_ms", options.local_llm.deadline_ms, 1);
    SetString(*section, "local_llm", "auth_token", options.local_llm.auth_token);
    SetString(*section, "local_llm", "auth_metadata_key", options.local_llm.auth_metadata_key);
}

void LoadEmotionAnalyzer(const Json& root, MultimodalServerOptions& options) {
    const Json* section = FindSection(root, "emotion_analyzer");
    if (!section) {
        return;
    }
    SetBool(*section, "emotion_analyzer", "enabled", options.emotion_analyzer.enabled);
    SetString(*section, "emotion_analyzer", "backend", options.emotion_analyzer.backend);
    SetString(*section, "emotion_analyzer", "target", options.emotion_analyzer.target);
    SetPath(*section, "emotion_analyzer", "tokenizer_path", options.emotion_analyzer.tokenizer_path);
    SetInt(*section, "emotion_analyzer", "deadline_ms", options.emotion_analyzer.deadline_ms, 1);
    SetString(*section, "emotion_analyzer", "auth_token", options.emotion_analyzer.auth_token);
    SetString(*section, "emotion_analyzer", "auth_metadata_key", options.emotion_analyzer.auth_metadata_key);
    SetSize(*section, "emotion_analyzer", "max_length", options.emotion_analyzer.max_length, 1);
    SetBool(*section, "emotion_analyzer", "truncation", options.emotion_analyzer.truncation);
    SetBool(*section, "emotion_analyzer", "padding", options.emotion_analyzer.padding);
    SetBool(*section, "emotion_analyzer", "add_special_tokens", options.emotion_analyzer.add_special_tokens);
}

void LoadEmotionFusion(const Json& root, MultimodalServerOptions& options) {
    const Json* section = FindSection(root, "emotion_fusion");
    if (!section) {
        return;
    }
    SetBool(*section, "emotion_fusion", "enabled", options.emotion_fusion.enabled);
    SetFloat(*section, "emotion_fusion", "bert_weight", options.emotion_fusion.bert_weight, 0.0f, 100.0f);
    SetFloat(*section, "emotion_fusion", "default_reliability", options.emotion_fusion.default_reliability, 0.0f, 1.0f);
    SetFloat(*section, "emotion_fusion", "accept_confidence", options.emotion_fusion.accept_confidence, 0.0f, 1.0f);
    SetFloat(*section, "emotion_fusion", "ambiguity_margin", options.emotion_fusion.ambiguity_margin, 0.0f, 1.0f);
    SetFloat(*section, "emotion_fusion", "head_bias", options.emotion_fusion.head_bias, -100.0f, 100.0f);
    SetFloat(*section, "emotion_fusion", "bert_signal_weight", options.emotion_fusion.bert_signal_weight, -100.0f, 100.0f);
    const float evidence_weight = options.emotion_fusion.keyword_signal_weight;
    float shared_evidence_weight = evidence_weight;
    SetFloat(*section, "emotion_fusion", "evidence_signal_weight", shared_evidence_weight, -100.0f, 100.0f);
    options.emotion_fusion.keyword_signal_weight = shared_evidence_weight;
    options.emotion_fusion.vector_signal_weight = shared_evidence_weight;
    SetFloat(*section, "emotion_fusion", "keyword_signal_weight", options.emotion_fusion.keyword_signal_weight, -100.0f, 100.0f);
    SetFloat(*section, "emotion_fusion", "vector_signal_weight", options.emotion_fusion.vector_signal_weight, -100.0f, 100.0f);
    SetFloat(*section, "emotion_fusion", "llm_signal_weight", options.emotion_fusion.llm_signal_weight, -100.0f, 100.0f);
    SetFloat(*section, "emotion_fusion", "margin_signal_weight", options.emotion_fusion.margin_signal_weight, -100.0f, 100.0f);
    SetFloat(*section, "emotion_fusion", "llm_gate_confidence", options.emotion_fusion.llm_gate_confidence, 0.0f, 1.0f);
    SetFloat(*section, "emotion_fusion", "llm_gate_min_delta", options.emotion_fusion.llm_gate_min_delta, 0.0f, 1.0f);

    if (const Json* weights = FindField(*section, "emotion_fusion", "source_weights")) {
        if (!weights->is_object()) {
            throw std::runtime_error("emotion_fusion.source_weights must be an object");
        }
        for (auto it = weights->begin(); it != weights->end(); ++it) {
            if (it.value().is_number()) {
                options.emotion_fusion.source_weights[it.key()] = it.value().get<double>();
            }
        }
    }
    if (const Json* reliability = FindField(*section, "emotion_fusion", "label_reliability")) {
        if (!reliability->is_object()) {
            throw std::runtime_error("emotion_fusion.label_reliability must be an object");
        }
        for (auto it = reliability->begin(); it != reliability->end(); ++it) {
            if (it.value().is_number()) {
                options.emotion_fusion.label_reliability[it.key()] = it.value().get<double>();
            }
        }
    }
    if (const Json* rules = FindField(*section, "emotion_fusion", "keyword_rules")) {
        if (!rules->is_object()) {
            throw std::runtime_error("emotion_fusion.keyword_rules must be an object");
        }
        for (auto label = rules->begin(); label != rules->end(); ++label) {
            if (!label.value().is_array()) {
                continue;
            }
            for (const auto& item : label.value()) {
                if (item.is_string()) {
                    options.emotion_fusion.keyword_rules.push_back({label.key(), item.get<std::string>(), 1.0f});
                } else if (item.is_object()) {
                    auto pattern = item.value("pattern", std::string{});
                    if (!pattern.empty()) {
                        options.emotion_fusion.keyword_rules.push_back({
                            label.key(),
                            std::move(pattern),
                            item.value("score", 1.0f),
                        });
                    }
                }
            }
        }
    }
}

void LoadL0Memory(const Json& root, MultimodalServerOptions& options) {
    const Json* section = FindSection(root, "l0_memory");
    if (!section) {
        return;
    }
    SetBool(*section, "l0_memory", "enabled", options.l0_memory.enabled);
    SetString(*section, "l0_memory", "redis_host", options.l0_memory.redis_host);
    SetInt(*section, "l0_memory", "redis_port", options.l0_memory.redis_port, 1, 65535);
    SetPath(*section, "l0_memory", "sqlite_path", options.l0_memory.sqlite_path);
    SetSize(*section, "l0_memory", "max_cached_records", options.l0_memory.max_cached_records, 1);
    SetSize(*section, "l0_memory", "top_k", options.l0_memory.top_k, 1);
    SetSize(*section, "l0_memory", "candidate_multiplier", options.l0_memory.candidate_multiplier, 1);
    SetSize(*section, "l0_memory", "neighbors_per_hit", options.l0_memory.neighbors_per_hit, 0);
    SetFloat(*section, "l0_memory", "similarity_floor", options.l0_memory.similarity_floor, 0.0f, 1.0f);
    SetInt64(*section, "l0_memory", "warm_window_seconds", options.l0_memory.warm_window_seconds, 0);
    SetInt64(*section, "l0_memory", "half_life_seconds", options.l0_memory.half_life_seconds, 0);
    SetInt64(*section, "l0_memory", "max_age_seconds", options.l0_memory.max_age_seconds, 0);
    SetSize(*section, "l0_memory", "max_payload_bytes",
            options.l0_memory.max_payload_bytes, 1);
    SetString(*section, "l0_memory", "user_uuid", options.l0_memory.user_uuid);
}

void LoadDocumentCaches(const Json& root, MultimodalServerOptions& options) {
    if (const Json* section = FindSection(root, "document_llm_chunk_cache")) {
        SetBool(*section, "document_llm_chunk_cache", "enabled", options.document_llm_chunk_cache.enabled);
        SetString(*section, "document_llm_chunk_cache", "redis_host", options.document_llm_chunk_cache.redis_host);
        SetInt(*section, "document_llm_chunk_cache", "redis_port", options.document_llm_chunk_cache.redis_port, 1, 65535);
        SetSize(*section, "document_llm_chunk_cache", "redis_pool_size", options.document_llm_chunk_cache.redis_pool_size, 1);
        SetString(*section, "document_llm_chunk_cache", "key_prefix", options.document_llm_chunk_cache.key_prefix);
        SetInt(*section, "document_llm_chunk_cache", "ttl_seconds", options.document_llm_chunk_cache.ttl_seconds, 1);
    }
    if (const Json* section = FindSection(root, "document_semantic_cache")) {
        SetBool(*section, "document_semantic_cache", "enabled", options.document_semantic_cache.enabled);
        SetString(*section, "document_semantic_cache", "redis_host", options.document_semantic_cache.redis_host);
        SetInt(*section, "document_semantic_cache", "redis_port", options.document_semantic_cache.redis_port, 1, 65535);
        SetPath(*section, "document_semantic_cache", "sqlite_path", options.document_semantic_cache.sqlite_path);
        SetString(*section, "document_semantic_cache", "user_uuid", options.document_semantic_cache.user_uuid);
        SetSize(*section, "document_semantic_cache", "max_cached_records", options.document_semantic_cache.max_cached_records, 1);
        SetSize(*section, "document_semantic_cache", "top_k", options.document_semantic_cache.top_k, 1);
        SetFloat(*section, "document_semantic_cache", "similarity_floor", options.document_semantic_cache.similarity_floor, 0.0f, 1.0f);
    }
}

void LoadL3(const Json& root, MultimodalServerOptions& options) {
    if (const Json* section = FindSection(root, "l3_memory")) {
        SetBool(*section, "l3_memory", "enabled", options.l3_memory.enabled);
        SetPath(*section, "l3_memory", "sqlite_path", options.l3_memory.sqlite_path);
        SetPath(*section, "l3_memory", "registry_sqlite_path", options.l3_memory.registry_sqlite_path);
        SetString(*section, "l3_memory", "collection_name", options.l3_memory.collection_name);
        SetString(*section, "l3_memory", "embedding_fingerprint", options.l3_memory.embedding_fingerprint);
        SetString(*section, "l3_memory", "tokenizer_fingerprint", options.l3_memory.tokenizer_fingerprint);
        SetString(*section, "l3_memory", "corpus_version", options.l3_memory.corpus_version);
        SetString(*section, "l3_memory", "policy_version", options.l3_memory.policy_version);
        SetString(*section, "l3_memory", "index_backend", options.l3_memory.index_backend);
        SetSize(*section, "l3_memory", "max_resident_partitions", options.l3_memory.max_resident_partitions, 1);
        SetInt(*section, "l3_memory", "max_records_per_batch", options.l3_memory.max_records_per_batch, 1);
        SetInt(*section, "l3_memory", "compression_max_tokens", options.l3_memory.compression_max_tokens, 1);
        SetFloat(*section, "l3_memory", "compression_temperature", options.l3_memory.compression_temperature, 0.0f, 2.0f);
        if (const Json* users = FindField(*section, "l3_memory", "user_uuids")) {
            if (!users->is_array()) {
                throw std::runtime_error("l3_memory.user_uuids must be an array");
            }
            for (const auto& item : *users) {
                if (item.is_string() && !item.get<std::string>().empty()) {
                    options.l3_memory.user_uuids.push_back(item.get<std::string>());
                }
            }
        }
    }
    if (const Json* section = FindSection(root, "l3_flush_scheduler")) {
        SetBool(*section, "l3_flush_scheduler", "enabled", options.l3_flush_scheduler.enabled);
        SetInt(*section, "l3_flush_scheduler", "check_interval_seconds", options.l3_flush_scheduler.check_interval_seconds, 1);
        SetInt(*section, "l3_flush_scheduler", "flush_hour", options.l3_flush_scheduler.flush_hour, 0, 23);
        SetInt(*section, "l3_flush_scheduler", "flush_minute", options.l3_flush_scheduler.flush_minute, 0, 59);
        SetInt(*section, "l3_flush_scheduler", "flush_date_offset_days", options.l3_flush_scheduler.flush_date_offset_days, -30, 30);
        SetBool(*section, "l3_flush_scheduler", "defer_when_sessions_active", options.l3_flush_scheduler.defer_when_sessions_active);
    }
}

void AgentGatewayConfigSection::LoadJson(const Json& root, MultimodalServerOptions& options) const {
    LoadLogging(root, options);
    LoadLocalLlm(root, options);
    LoadEmotionAnalyzer(root, options);
    LoadEmotionFusion(root, options);
    LoadL0Memory(root, options);
    LoadDocumentCaches(root, options);
    LoadL3(root, options);
}

bool AgentGatewayConfigSection::LoadCli(CliCursor&, MultimodalServerOptions&) const {
    return false;
}

void AgentGatewayConfigSection::Validate(MultimodalServerOptions& options) const {
    options.logging.log_dir = ResolveRelativeToConfig(options.logging.log_dir, options.config_file_path);
    options.l0_memory.sqlite_path = ResolveRelativeToConfig(options.l0_memory.sqlite_path, options.config_file_path);
    options.document_semantic_cache.sqlite_path =
        ResolveRelativeToConfig(options.document_semantic_cache.sqlite_path, options.config_file_path);
    options.l3_memory.sqlite_path = ResolveRelativeToConfig(options.l3_memory.sqlite_path, options.config_file_path);
    options.l3_memory.registry_sqlite_path =
        ResolveRelativeToConfig(options.l3_memory.registry_sqlite_path, options.config_file_path);
    options.emotion_analyzer.tokenizer_path =
        ResolveRelativeToConfig(options.emotion_analyzer.tokenizer_path, options.config_file_path);
    if (options.l0_memory.user_uuid.empty()) {
        options.l0_memory.user_uuid = "gateway-l0";
    }
    std::unordered_set<std::string> seen;
    std::vector<std::string> unique;
    for (auto& user : options.l3_memory.user_uuids) {
        if (!user.empty() && seen.insert(user).second) {
            unique.push_back(std::move(user));
        }
    }
    options.l3_memory.user_uuids = std::move(unique);
}

} // namespace

REGISTER_CONFIG_SECTION(AgentGatewayConfigSection)

} // namespace server_config
