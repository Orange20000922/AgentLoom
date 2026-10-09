#include "config_section.h"

#include <map>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>

namespace server_config {
namespace {

DECLARE_CONFIG_SECTION(PersonaGatewayConfigSection, "persona_gateway")
    CONFIG_CLI_STRING(kWebSocketPath, "--gateway-ws-path");
    CONFIG_CLI_STRING(kStaticRoot, "--gateway-static-root");
    CONFIG_CLI_STRING(kStaticIndex, "--gateway-static-index");
    CONFIG_CLI_STRING(kStaticSpa, "--gateway-static-spa");
    CONFIG_CLI_STRING(kDocumentStoreEnabled, "--gateway-document-store-enabled");
    CONFIG_CLI_STRING(kDocumentStoreRoot, "--gateway-document-store-root");
    CONFIG_CLI_STRING(kDocumentStoreDb, "--gateway-document-store-db");
    CONFIG_CLI_STRING(kDocumentStoreRetentionHours, "--gateway-document-store-retention-hours");
    CONFIG_CLI_STRING(kDocumentStoreCleanupSeconds, "--gateway-document-store-cleanup-seconds");
    CONFIG_CLI_STRING(kDocumentStoreEnablePathRegisterTest, "--gateway-document-store-enable-path-register-test");
    CONFIG_CLI_STRING(kDocumentStoreEnablePathAnalyzeTest, "--gateway-document-store-enable-path-analyze-test");
    CONFIG_CLI_STRING(kComputeWorkers, "--gateway-compute-workers");
    CONFIG_CLI_STRING(kComputeQueue, "--gateway-compute-queue");
    CONFIG_CLI_STRING(kIoWorkers, "--gateway-io-workers");
    CONFIG_CLI_STRING(kIoQueue, "--gateway-io-queue");
    CONFIG_CLI_STRING(kLlmWorkers, "--gateway-llm-workers");
    CONFIG_CLI_STRING(kLlmQueue, "--gateway-llm-queue");
    CONFIG_CLI_STRING(kSessionIdle, "--gateway-session-idle-minutes");
    CONFIG_CLI_STRING(kSessionTurns, "--gateway-session-max-recent-turns");
    CONFIG_CLI_STRING(kSessionMaxActive, "--gateway-session-max-active");
    CONFIG_CLI_STRING(kRecentRawTurns, "--gateway-runtime-recent-raw-turns");
    CONFIG_CLI_STRING(kDefaultModel, "--gateway-runtime-model");
    CONFIG_CLI_STRING(kFilterDisabled, "--gateway-filter-disabled");
    CONFIG_CLI_STRING(kFilterEnabled, "--gateway-filter-enabled");
    void Validate(MultimodalServerOptions& options) const override;
};

std::filesystem::path ResolveRelativeToConfig(const std::filesystem::path& path,
                                              const std::filesystem::path& config_path) {
    if (path.empty() || path.is_absolute() || config_path.empty()) {
        return path;
    }
    return config_path.parent_path() / path;
}

void LoadThreadPoolJson(const Json& section,
                        std::string_view section_name,
                        std::string_view field_name,
                        GatewayThreadPoolConfigOptions& options) {
    const Json* pool = FindField(section, section_name, field_name);
    if (!pool) {
        return;
    }
    if (!pool->is_object()) {
        throw std::runtime_error(std::string(section_name) + "." + std::string(field_name) + " must be an object");
    }
    SetSize(*pool, field_name, "worker_count", options.worker_count, 0);
    SetSize(*pool, field_name, "queue_capacity", options.queue_capacity, 0);
    SetString(*pool, field_name, "scheduler", options.scheduler);
    SetSize(*pool, field_name, "max_active_keys", options.max_active_keys, 1);
    SetSize(*pool, field_name, "max_outstanding_per_key", options.max_outstanding_per_key, 1);
    SetSize(*pool,
            field_name,
            "max_outstanding_per_fairness_key",
            options.max_outstanding_per_fairness_key,
            1);
    SetSize(*pool,
            field_name,
            "max_outstanding_per_tenant",
            options.max_outstanding_per_tenant,
            1);
}

GatewayThreadPoolConfigOptions& EnsureLlmPool(PersonaGatewayConfigOptions& gateway) {
    if (!gateway.llm_pool) {
        GatewayThreadPoolConfigOptions options;
        options.scheduler = "session_affinity";
        gateway.llm_pool = std::move(options);
    }
    return *gateway.llm_pool;
}

void ValidateThreadPool(std::string_view field_name,
                        const GatewayThreadPoolConfigOptions& options) {
    if (options.scheduler != "default_fifo" && options.scheduler != "session_affinity") {
        throw std::runtime_error("persona_gateway." + std::string(field_name) +
                                 ".scheduler must be default_fifo or session_affinity");
    }
}

void SetDoubleField(const Json& section,
                    std::string_view section_name,
                    std::string_view field_name,
                    double& target,
                    double min_value,
                    double max_value) {
    const Json* field = FindField(section, section_name, field_name);
    if (!field) {
        return;
    }
    if (!field->is_number()) {
        throw std::runtime_error(std::string(section_name) + "." + std::string(field_name) + " must be a number");
    }
    const auto value = field->get<double>();
    if (value < min_value || value > max_value) {
        throw std::runtime_error(std::string(section_name) + "." + std::string(field_name) + " is out of range");
    }
    target = value;
}

std::unordered_map<std::string, double> DoubleMapFromJson(const Json& json, std::string_view label) {
    if (!json.is_object()) {
        throw std::runtime_error(std::string(label) + " must be an object");
    }
    std::unordered_map<std::string, double> out;
    for (const auto& item : json.items()) {
        if (!item.value().is_number()) {
            throw std::runtime_error(std::string(label) + "." + item.key() + " must be a number");
        }
        out[item.key()] = item.value().get<double>();
    }
    return out;
}

std::unordered_map<std::string, std::string> StringMapFromJson(const Json& json, std::string_view label) {
    if (!json.is_object()) {
        throw std::runtime_error(std::string(label) + " must be an object");
    }
    std::unordered_map<std::string, std::string> out;
    for (const auto& item : json.items()) {
        if (!item.value().is_string()) {
            throw std::runtime_error(std::string(label) + "." + item.key() + " must be a string");
        }
        out[item.key()] = item.value().get<std::string>();
    }
    return out;
}

void LoadEmotionStateJson(const Json& json, GatewayEmotionStateConfigOptions& options) {
    if (!json.is_object()) {
        throw std::runtime_error("persona_gateway.personas[].emotionState must be an object");
    }
    SetDoubleField(json, "emotionState", "alpha", options.alpha, 0.0, 1000000.0);
    SetDoubleField(json, "emotionState", "beta", options.beta, 0.0, 1000000.0);
    SetDoubleField(json, "emotionState", "gamma", options.gamma, 0.0, 1000000.0);
    SetDoubleField(json, "emotionState", "delta", options.delta, 0.0, 1000000.0);
    if (const Json* value = FindField(json, "emotionState", "baselineValence")) {
        options.baseline_valence = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "baseline_valence")) {
        options.baseline_valence = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "baselineArousal")) {
        options.baseline_arousal = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "baseline_arousal")) {
        options.baseline_arousal = value->get<double>();
    }
    SetDoubleField(json, "emotionState", "kappa", options.kappa, 0.0, 1000000.0);
    if (const Json* value = FindField(json, "emotionState", "negativityBias")) {
        options.negativity_bias = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "negativity_bias")) {
        options.negativity_bias = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "noiseSigma")) {
        options.noise_sigma = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "noise_sigma")) {
        options.noise_sigma = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "injectionThreshold")) {
        options.injection_threshold = value->get<double>();
    }
    if (const Json* value = FindField(json, "emotionState", "injection_threshold")) {
        options.injection_threshold = value->get<double>();
    }
    SetInt(json, "emotionState", "save_interval_turns", options.save_interval_turns, 1, 1000000);
    if (const Json* value = FindField(json, "emotionState", "saveIntervalTurns")) {
        options.save_interval_turns = value->get<int>();
    }
    SetBool(json, "emotionState", "persist_to_l4", options.persist_to_l4);
    if (const Json* value = FindField(json, "emotionState", "persistToL4")) {
        if (!value->is_boolean()) {
            throw std::runtime_error("emotionState.persistToL4 must be a boolean");
        }
        options.persist_to_l4 = value->get<bool>();
    }
}

std::optional<GatewayEmotionPromptConfigOptions> LoadEmotionPromptsJson(const Json& json) {
    if (!json.is_object()) {
        throw std::runtime_error("persona_gateway.personas[].emotionPrompts must be an object");
    }
    GatewayEmotionPromptConfigOptions options;
    if (const Json* value = FindField(json, "emotionPrompts", "emotionMap")) {
        options.emotion_map = StringMapFromJson(*value, "emotionPrompts.emotionMap");
    }
    if (const Json* value = FindField(json, "emotionPrompts", "emotion_map")) {
        options.emotion_map = StringMapFromJson(*value, "emotionPrompts.emotion_map");
    }
    if (const Json* value = FindField(json, "emotionPrompts", "emotionReliability")) {
        options.emotion_reliability = DoubleMapFromJson(*value, "emotionPrompts.emotionReliability");
    }
    if (const Json* value = FindField(json, "emotionPrompts", "emotion_reliability")) {
        options.emotion_reliability = DoubleMapFromJson(*value, "emotionPrompts.emotion_reliability");
    }
    if (const Json* value = FindField(json, "emotionPrompts", "confidenceThresholds")) {
        options.confidence_thresholds = DoubleMapFromJson(*value, "emotionPrompts.confidenceThresholds");
    }
    if (const Json* value = FindField(json, "emotionPrompts", "confidence_thresholds")) {
        options.confidence_thresholds = DoubleMapFromJson(*value, "emotionPrompts.confidence_thresholds");
    }
    if (const Json* value = FindField(json, "emotionPrompts", "intensityLevels")) {
        options.intensity_levels = DoubleMapFromJson(*value, "emotionPrompts.intensityLevels");
    }
    if (const Json* value = FindField(json, "emotionPrompts", "intensity_levels")) {
        options.intensity_levels = DoubleMapFromJson(*value, "emotionPrompts.intensity_levels");
    }
    return options;
}

GatewayPersonaConfigOptions LoadPersonaJson(std::string persona_id, const Json& json) {
    if (!json.is_object()) {
        throw std::runtime_error("persona_gateway.personas entry must be an object");
    }
    GatewayPersonaConfigOptions options;
    options.persona_id = std::move(persona_id);
    if (options.persona_id.empty()) {
        options.persona_id = json.value("personaId", json.value("persona_id", json.value("name", std::string{})));
    }
    options.description = json.value("description", std::string{});
    options.traits = json.value("traits", std::vector<std::string>{});
    options.openness = json.value("openness", options.openness);
    options.extraversion = json.value("extraversion", options.extraversion);
    options.humor_tendency = json.value("humorTendency", json.value("humor_tendency", options.humor_tendency));
    options.empathy_level = json.value("empathyLevel", json.value("empathy_level", options.empathy_level));
    options.curiosity_level = json.value("curiosityLevel", json.value("curiosity_level", options.curiosity_level));
    options.formality = json.value("formality", options.formality);
    if (const Json* prompts = FindField(json, "persona", "emotionPrompts")) {
        options.emotion_prompts = LoadEmotionPromptsJson(*prompts);
    }
    if (const Json* prompts = FindField(json, "persona", "emotion_prompts")) {
        options.emotion_prompts = LoadEmotionPromptsJson(*prompts);
    }
    if (const Json* emotion_state = FindField(json, "persona", "emotionState")) {
        LoadEmotionStateJson(*emotion_state, options.emotion_state);
    }
    if (const Json* emotion_state = FindField(json, "persona", "emotion_state")) {
        LoadEmotionStateJson(*emotion_state, options.emotion_state);
    }
    if (options.persona_id.empty()) {
        throw std::runtime_error("persona_gateway.personas entry requires personaId or name");
    }
    return options;
}

void LoadPersonasJson(const Json& section, PersonaGatewayConfigOptions& gateway) {
    const Json* personas = FindField(section, "persona_gateway", "personas");
    if (!personas) {
        return;
    }
    gateway.personas.clear();
    if (personas->is_object()) {
        for (const auto& item : personas->items()) {
            gateway.personas.push_back(LoadPersonaJson(item.key(), item.value()));
        }
        return;
    }
    if (!personas->is_array()) {
        throw std::runtime_error("persona_gateway.personas must be an object or array");
    }
    for (const auto& item : *personas) {
        if (!item.is_object()) {
            throw std::runtime_error("persona_gateway.personas[] must be an object");
        }
        if (item.size() == 1 && !item.contains("personaId") && !item.contains("persona_id") && !item.contains("name")) {
            const auto& entry = *item.begin();
            gateway.personas.push_back(LoadPersonaJson(item.begin().key(), entry));
        } else {
            gateway.personas.push_back(LoadPersonaJson({}, item));
        }
    }
}

void PersonaGatewayConfigSection::LoadJson(const Json& root, MultimodalServerOptions& options) const {
    const Json* section = FindSection(root, Name());
    if (!section) {
        return;
    }

    SetString(*section, Name(), "websocket_path", options.persona_gateway.websocket_path);
    SetString(*section, Name(), "address", options.http.address);
    if (const Json* port_field = FindField(*section, Name(), "port")) {
        if (port_field->is_number_unsigned()) {
            unsigned int port = port_field->get<unsigned int>();
            if (port > 65535) {
                throw std::runtime_error("persona_gateway.port must be <= 65535");
            }
            options.http.port = static_cast<unsigned short>(port);
        }
    }
    SetSize(*section, Name(), "http_io_threads", options.http.io_threads, 1);
    if (const Json* static_files = FindField(*section, Name(), "static_files")) {
        if (!static_files->is_object()) {
            throw std::runtime_error("persona_gateway.static_files must be an object");
        }
        SetBool(*static_files, "static_files", "enabled", options.persona_gateway.static_files.enabled);
        SetPath(*static_files, "static_files", "root", options.persona_gateway.static_files.root);
        SetString(*static_files, "static_files", "index_file", options.persona_gateway.static_files.index_file);
        SetBool(*static_files, "static_files", "spa_fallback", options.persona_gateway.static_files.spa_fallback);
    }
    if (const Json* document_store = FindField(*section, Name(), "document_store")) {
        if (!document_store->is_object()) {
            throw std::runtime_error("persona_gateway.document_store must be an object");
        }
        SetBool(*document_store, "document_store", "enabled", options.persona_gateway.document_store.enabled);
        SetPath(*document_store, "document_store", "root", options.persona_gateway.document_store.root);
        SetPath(*document_store, "document_store", "database_path", options.persona_gateway.document_store.database_path);
        SetSize(*document_store, "document_store", "read_connection_count", options.persona_gateway.document_store.read_connection_count, 1);
        SetSize(*document_store, "document_store", "write_connection_count", options.persona_gateway.document_store.write_connection_count, 1);
        SetInt(*document_store, "document_store", "busy_timeout_ms", options.persona_gateway.document_store.busy_timeout_ms, 1, 60000);
        SetInt(*document_store, "document_store", "retention_hours", options.persona_gateway.document_store.retention_hours, 1, 24 * 365);
        SetInt(*document_store, "document_store", "cleanup_interval_seconds", options.persona_gateway.document_store.cleanup_interval_seconds, 1, 24 * 3600);
        SetBool(*document_store,
                "document_store",
                "enable_path_register_test_endpoint",
                options.persona_gateway.document_store.enable_path_register_test_endpoint);
        SetBool(*document_store,
                "document_store",
                "enable_path_analyze_test_endpoint",
                options.persona_gateway.document_store.enable_path_analyze_test_endpoint);
    }
    LoadThreadPoolJson(*section, Name(), "compute_pool", options.persona_gateway.compute_pool);
    LoadThreadPoolJson(*section, Name(), "io_pool", options.persona_gateway.io_pool);
    if (FindField(*section, Name(), "llm_pool")) {
        LoadThreadPoolJson(*section, Name(), "llm_pool", EnsureLlmPool(options.persona_gateway));
    }
    SetInt(*section, Name(), "session_idle_timeout_minutes", options.persona_gateway.session_idle_timeout_minutes, 1, 1440);
    SetSize(*section, Name(), "session_max_recent_turns", options.persona_gateway.session_max_recent_turns, 1);
    SetSize(*section, Name(), "session_max_active_sessions", options.persona_gateway.session_max_active_sessions, 1);
    SetSize(*section, Name(), "runtime_recent_raw_turns", options.persona_gateway.runtime_recent_raw_turns, 1);
    SetString(*section, Name(), "runtime_default_model", options.persona_gateway.runtime_default_model);
    if (const Json* streaming = FindField(*section, Name(), "streaming")) {
        if (!streaming->is_object()) throw std::runtime_error("persona_gateway.streaming must be an object");
        auto& value = options.persona_gateway.streaming;
        SetSize(*streaming, "streaming", "max_pending_events", value.transport.outbound.max_items, 1);
        SetSize(*streaming, "streaming", "max_pending_bytes", value.transport.outbound.max_bytes, 1);
        SetSize(*streaming, "streaming", "max_event_bytes", value.transport.max_event_bytes, 1);
        SetSize(*streaming, "streaming", "max_replay_turns", value.max_replay_turns, 1);
        SetSize(*streaming, "streaming", "max_replay_events", value.max_replay_events, 1);
        SetSize(*streaming, "streaming", "max_replay_bytes", value.max_replay_bytes, 1);
        SetInt(*streaming, "streaming", "replay_retention_ms", value.replay_retention_ms, 1, 3600000);
        SetBool(*streaming, "streaming", "require_pong", value.transport.require_pong);
        const auto load_duration = [&](const char* name, std::chrono::milliseconds& target) {
            auto ms = static_cast<int>(target.count());
            SetInt(*streaming, "streaming", name, ms, 1, 3600000);
            target = std::chrono::milliseconds(ms);
        };
        load_duration("heartbeat_interval_ms", value.transport.heartbeat_interval);
        load_duration("idle_timeout_ms", value.transport.idle_timeout);
        load_duration("max_duration_ms", value.transport.max_duration);
        load_duration("write_timeout_ms", value.transport.write_timeout);
        SetInt(*streaming, "streaming", "reconnect_delay_ms", value.transport.reconnect_delay_ms, 0, 3600000);
    }
    if (const Json* filter = FindField(*section, Name(), "request_filter")) {
        if (!filter->is_object()) {
            throw std::runtime_error("persona_gateway.request_filter must be an object");
        }
        SetBool(*filter, "request_filter", "enabled", options.persona_gateway.request_filter_enabled);
        SetBool(*filter, "request_filter", "reject_control_chars", options.persona_gateway.reject_control_chars);
        SetBool(*filter, "request_filter", "reject_suspicious_patterns", options.persona_gateway.reject_suspicious_patterns);
    }
    LoadPersonasJson(*section, options.persona_gateway);
}

bool PersonaGatewayConfigSection::LoadCli(CliCursor& cursor, MultimodalServerOptions& options) const {
    CliArgumentParser parser(cursor);

    CONFIG_VALUE_ARG(kWebSocketPath, value, options.persona_gateway.websocket_path = *value;)
    CONFIG_VALUE_ARG(kStaticRoot, value, {
        options.persona_gateway.static_files.enabled = true;
        options.persona_gateway.static_files.root = *value;
    })
    CONFIG_VALUE_ARG(kStaticIndex, value, options.persona_gateway.static_files.index_file = *value;)
    CONFIG_FLAG_ARG(kStaticSpa, {
        options.persona_gateway.static_files.enabled = true;
        options.persona_gateway.static_files.spa_fallback = true;
    })
    CONFIG_FLAG_ARG(kDocumentStoreEnabled, options.persona_gateway.document_store.enabled = true;)
    CONFIG_VALUE_ARG(kDocumentStoreRoot, value, {
        options.persona_gateway.document_store.enabled = true;
        options.persona_gateway.document_store.root = *value;
    })
    CONFIG_VALUE_ARG(kDocumentStoreDb, value, {
        options.persona_gateway.document_store.enabled = true;
        options.persona_gateway.document_store.database_path = *value;
    })
    CONFIG_VALUE_ARG(kDocumentStoreRetentionHours, value, {
        options.persona_gateway.document_store.enabled = true;
        options.persona_gateway.document_store.retention_hours = ParsePositiveOption(kDocumentStoreRetentionHours, *value);
    })
    CONFIG_VALUE_ARG(kDocumentStoreCleanupSeconds, value, {
        options.persona_gateway.document_store.enabled = true;
        options.persona_gateway.document_store.cleanup_interval_seconds = ParsePositiveOption(kDocumentStoreCleanupSeconds, *value);
    })
    CONFIG_FLAG_ARG(kDocumentStoreEnablePathRegisterTest, {
        options.persona_gateway.document_store.enabled = true;
        options.persona_gateway.document_store.enable_path_register_test_endpoint = true;
    })
    CONFIG_FLAG_ARG(kDocumentStoreEnablePathAnalyzeTest, {
        options.persona_gateway.document_store.enabled = true;
        options.persona_gateway.document_store.enable_path_analyze_test_endpoint = true;
    })
    CONFIG_VALUE_ARG(kComputeWorkers, value, options.persona_gateway.compute_pool.worker_count = ParseNonNegativeOption(kComputeWorkers, *value);)
    CONFIG_VALUE_ARG(kComputeQueue, value, options.persona_gateway.compute_pool.queue_capacity = ParseNonNegativeOption(kComputeQueue, *value);)
    CONFIG_VALUE_ARG(kIoWorkers, value, options.persona_gateway.io_pool.worker_count = ParseNonNegativeOption(kIoWorkers, *value);)
    CONFIG_VALUE_ARG(kIoQueue, value, options.persona_gateway.io_pool.queue_capacity = ParseNonNegativeOption(kIoQueue, *value);)
    CONFIG_VALUE_ARG(kLlmWorkers, value, EnsureLlmPool(options.persona_gateway).worker_count = ParseNonNegativeOption(kLlmWorkers, *value);)
    CONFIG_VALUE_ARG(kLlmQueue, value, EnsureLlmPool(options.persona_gateway).queue_capacity = ParseNonNegativeOption(kLlmQueue, *value);)
    CONFIG_VALUE_ARG(kSessionIdle, value, options.persona_gateway.session_idle_timeout_minutes = ParsePositiveOption(kSessionIdle, *value);)
    CONFIG_VALUE_ARG(kSessionTurns, value, options.persona_gateway.session_max_recent_turns = ParsePositiveOption(kSessionTurns, *value);)
    CONFIG_VALUE_ARG(kSessionMaxActive, value, options.persona_gateway.session_max_active_sessions = ParsePositiveOption(kSessionMaxActive, *value);)
    CONFIG_VALUE_ARG(kRecentRawTurns, value, options.persona_gateway.runtime_recent_raw_turns = ParsePositiveOption(kRecentRawTurns, *value);)
    CONFIG_VALUE_ARG(kDefaultModel, value, options.persona_gateway.runtime_default_model = *value;)
    CONFIG_FLAG_ARG(kFilterDisabled, options.persona_gateway.request_filter_enabled = false;)
    CONFIG_FLAG_ARG(kFilterEnabled, options.persona_gateway.request_filter_enabled = true;)

    return false;
}

void PersonaGatewayConfigSection::Validate(MultimodalServerOptions& options) const {
    auto& gateway = options.persona_gateway;
    if (gateway.streaming.transport.idle_timeout <= gateway.streaming.transport.heartbeat_interval)
        throw std::runtime_error("persona_gateway.streaming idle_timeout_ms must exceed heartbeat_interval_ms");
    ValidateThreadPool("compute_pool", gateway.compute_pool);
    ValidateThreadPool("io_pool", gateway.io_pool);
    if (gateway.llm_pool) {
        ValidateThreadPool("llm_pool", *gateway.llm_pool);
        if (gateway.llm_pool->scheduler != "session_affinity") {
            throw std::runtime_error(
                "persona_gateway.llm_pool.scheduler must be session_affinity");
        }
    }
    if (gateway.websocket_path.empty() || gateway.websocket_path.front() != '/') {
        throw std::runtime_error("persona_gateway.websocket_path must start with '/'");
    }
    if (gateway.static_files.enabled) {
        if (gateway.static_files.root.empty()) {
            throw std::runtime_error("persona_gateway.static_files.root is required when static files are enabled");
        }
        gateway.static_files.root = ResolveRelativeToConfig(gateway.static_files.root, options.config_file_path);
        if (gateway.static_files.index_file.empty()) {
            throw std::runtime_error("persona_gateway.static_files.index_file must not be empty");
        }
    }
    if (gateway.document_store.enabled) {
        if (gateway.document_store.root.empty()) {
            throw std::runtime_error("persona_gateway.document_store.root is required when document store is enabled");
        }
        if (gateway.document_store.database_path.empty()) {
            throw std::runtime_error("persona_gateway.document_store.database_path is required when document store is enabled");
        }
        if (gateway.document_store.write_connection_count != 1) {
            throw std::runtime_error(
                "persona_gateway.document_store.write_connection_count must be 1 for SQLite");
        }
        gateway.document_store.root = ResolveRelativeToConfig(gateway.document_store.root, options.config_file_path);
        gateway.document_store.database_path = ResolveRelativeToConfig(gateway.document_store.database_path, options.config_file_path);
    }
    std::unordered_set<std::string> persona_ids;
    for (const auto& persona : gateway.personas) {
        if (persona.persona_id.empty()) {
            throw std::runtime_error("persona_gateway.personas persona_id must not be empty");
        }
        if (!persona_ids.insert(persona.persona_id).second) {
            throw std::runtime_error("duplicate persona_gateway.personas id: " + persona.persona_id);
        }
    }
}

} // namespace

REGISTER_CONFIG_SECTION(PersonaGatewayConfigSection)

} // namespace server_config
