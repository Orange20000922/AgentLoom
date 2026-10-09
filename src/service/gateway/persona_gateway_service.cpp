#include "persona_gateway_service.h"

#include "redis_connection_pool.h"
#include "result.h"
#include "sqlite/sqlite_connection.h"
#include "sqlite/sqlite_connection_pool.h"
#include "sqlite/sqlite_statement.h"
#include "trace_context.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace agent::service::gateway {
namespace {

using Json = nlohmann::json;
using storage::sqlite::SqliteConnectionLease;
using storage::sqlite::SqliteConnectionPool;
using storage::sqlite::SqliteConnectionPoolOptions;
using storage::sqlite::SqliteStepResult;

std::shared_ptr<SqliteConnectionPool> MakePersonaMetadataPool(std::string database_path) {
    SqliteConnectionPoolOptions options;
    options.path = std::move(database_path);
    options.read_connection_count = 4;
    options.write_connection_count = 1;
    options.busy_timeout_ms = 5000;
    options.enable_wal = true;
    return std::make_shared<SqliteConnectionPool>(std::move(options));
}

core::Result<SqliteConnectionLease> AcquirePersonaMetadataConnection(
    const std::shared_ptr<SqliteConnectionPool>& pool,
    bool write) {
    if (!pool) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "SQLite persona metadata pool is not configured");
    }
    if (auto status = pool->Start(); !status.ok()) {
        return status;
    }
    return write ? pool->WaitAcquireWrite() : pool->WaitAcquireRead();
}

std::string NowIso8601Utc() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::chrono::milliseconds Since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
}

std::string DefaultPersonaName(std::string_view persona_id) {
    if (!persona_id.empty()) {
        return std::string(persona_id);
    }
    return "default_persona";
}

core::Status ValidatePersonaMetadata(const PersonaMetadataRecord& record) {
    if (record.tenant_id.empty() || record.user_uuid.empty() || record.persona_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "tenant_id, user_uuid and persona_id are required");
    }
    if (record.personality.name.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "persona personality name is required");
    }
    return core::Status::Ok();
}

Json EmotionStateConfigToJson(const persona::EmotionStateConfig& config) {
    return Json{
        {"alpha", config.alpha},
        {"beta", config.beta},
        {"gamma", config.gamma},
        {"delta", config.delta},
        {"baseline_valence", config.baseline_valence},
        {"baseline_arousal", config.baseline_arousal},
        {"kappa", config.kappa},
        {"negativity_bias", config.negativity_bias},
        {"noise_sigma", config.noise_sigma},
        {"injection_threshold", config.injection_threshold},
        {"save_interval_turns", config.save_interval_turns},
        {"persist_to_l4", config.persist_to_l4},
    };
}

persona::EmotionStateConfig EmotionStateConfigFromJson(const Json& json) {
    persona::EmotionStateConfig config;
    if (!json.is_object()) {
        return config;
    }
    config.alpha = json.value("alpha", config.alpha);
    config.beta = json.value("beta", config.beta);
    config.gamma = json.value("gamma", config.gamma);
    config.delta = json.value("delta", config.delta);
    config.baseline_valence = json.value("baseline_valence", json.value("baselineValence", config.baseline_valence));
    config.baseline_arousal = json.value("baseline_arousal", json.value("baselineArousal", config.baseline_arousal));
    config.kappa = json.value("kappa", config.kappa);
    config.negativity_bias = json.value("negativity_bias", json.value("negativityBias", config.negativity_bias));
    config.noise_sigma = json.value("noise_sigma", json.value("noiseSigma", config.noise_sigma));
    config.injection_threshold = json.value("injection_threshold", json.value("injectionThreshold", config.injection_threshold));
    config.save_interval_turns = json.value("save_interval_turns", json.value("saveIntervalTurns", config.save_interval_turns));
    config.persist_to_l4 = json.value("persist_to_l4", json.value("persistToL4", config.persist_to_l4));
    return config;
}

Json PersonalityConfigToJson(const persona::PersonalityConfig& personality) {
    return Json{
        {"name", personality.name},
        {"description", personality.description},
        {"traits", personality.traits},
        {"openness", personality.openness},
        {"extraversion", personality.extraversion},
        {"humor_tendency", personality.humor_tendency},
        {"empathy_level", personality.empathy_level},
        {"curiosity_level", personality.curiosity_level},
        {"formality", personality.formality},
    };
}

persona::PersonalityConfig PersonalityConfigFromJson(const Json& json) {
    persona::PersonalityConfig personality;
    if (!json.is_object()) {
        return personality;
    }
    personality.name = json.value("name", std::string{});
    personality.description = json.value("description", std::string{});
    personality.traits = json.value("traits", std::vector<std::string>{});
    personality.openness = json.value("openness", personality.openness);
    personality.extraversion = json.value("extraversion", personality.extraversion);
    personality.humor_tendency = json.value("humor_tendency", json.value("humorTendency", personality.humor_tendency));
    personality.empathy_level = json.value("empathy_level", json.value("empathyLevel", personality.empathy_level));
    personality.curiosity_level = json.value("curiosity_level", json.value("curiosityLevel", personality.curiosity_level));
    personality.formality = json.value("formality", personality.formality);
    return personality;
}

Json EmotionPromptConfigToJson(const std::optional<persona::EmotionPromptConfig>& config) {
    if (!config) {
        return Json(nullptr);
    }
    return Json{
        {"emotion_map", config->emotion_map},
        {"emotion_reliability", config->emotion_reliability},
        {"confidence_thresholds", config->confidence_thresholds},
        {"intensity_levels", config->intensity_levels},
    };
}

std::optional<persona::EmotionPromptConfig> EmotionPromptConfigFromJson(const Json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    persona::EmotionPromptConfig config;
    config.emotion_map = json.value("emotion_map", json.value("emotionMap", config.emotion_map));
    config.emotion_reliability = json.value("emotion_reliability", json.value("emotionReliability", config.emotion_reliability));
    config.confidence_thresholds = json.value("confidence_thresholds", json.value("confidenceThresholds", config.confidence_thresholds));
    config.intensity_levels = json.value("intensity_levels", json.value("intensityLevels", config.intensity_levels));
    return config;
}

Json PersonaMetadataRecordToJson(const PersonaMetadataRecord& record) {
    return Json{
        {"tenant_id", record.tenant_id.empty() ? "default" : record.tenant_id},
        {"user_uuid", record.user_uuid},
        {"persona_id", record.persona_id},
        {"personality", PersonalityConfigToJson(record.personality)},
        {"emotion_prompt_config", EmotionPromptConfigToJson(record.emotion_prompt_config)},
        {"emotion_state_config", EmotionStateConfigToJson(record.emotion_state_config)},
        {"schema_version", 1},
    };
}

core::Result<PersonaMetadataRecord> PersonaMetadataRecordFromJson(std::string_view payload) {
    Json json;
    try {
        json = Json::parse(payload);
    } catch (const Json::exception& e) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, e.what());
    }
    PersonaMetadataRecord record;
    record.tenant_id = json.value("tenant_id", std::string{"default"});
    record.user_uuid = json.value("user_uuid", std::string{});
    record.persona_id = json.value("persona_id", std::string{});
    record.personality = PersonalityConfigFromJson(json.value("personality", Json::object()));
    record.emotion_prompt_config = EmotionPromptConfigFromJson(json.value("emotion_prompt_config", Json(nullptr)));
    record.emotion_state_config = EmotionStateConfigFromJson(json.value("emotion_state_config", Json::object()));
    if (auto status = ValidatePersonaMetadata(record); !status.ok()) {
        return status;
    }
    return record;
}

core::Status EnsureSessionOwner(const persona::SessionSnapshot& session,
                                std::string_view authenticated_user_uuid) {
    if (authenticated_user_uuid.empty()) {
        return core::Status::Ok();
    }
    if (session.user_uuid != authenticated_user_uuid) {
        return core::Status::Error(core::ErrorCode::PermissionDenied, "session does not belong to authenticated user");
    }
    return core::Status::Ok();
}

}

std::string InMemoryPersonaMetadataStore::Key(std::string_view tenant_id,
                                              std::string_view user_uuid,
                                              std::string_view persona_id) {
    return std::string(tenant_id) + '\n' + std::string(user_uuid) + '\n' + std::string(persona_id);
}

core::Status InMemoryPersonaMetadataStore::EnsureSchema() {
    return core::Status::Ok();
}

core::Status InMemoryPersonaMetadataStore::Upsert(PersonaMetadataRecord record) {
    if (auto status = ValidatePersonaMetadata(record); !status.ok()) {
        return status;
    }
    std::lock_guard lock(mutex_);
    records_[Key(record.tenant_id, record.user_uuid, record.persona_id)] = std::move(record);
    return core::Status::Ok();
}

core::Result<PersonaMetadataRecord> InMemoryPersonaMetadataStore::Get(std::string_view tenant_id,
                                                                      std::string_view user_uuid,
                                                                      std::string_view persona_id) const {
    if (tenant_id.empty() || user_uuid.empty() || persona_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "tenant_id, user_uuid and persona_id are required");
    }
    std::lock_guard lock(mutex_);
    auto it = records_.find(Key(tenant_id, user_uuid, persona_id));
    if (it == records_.end()) {
        return core::Status::Error(core::ErrorCode::NotFound, "persona metadata not found");
    }
    return it->second;
}

core::Result<std::vector<PersonaMetadataRecord>> InMemoryPersonaMetadataStore::ListByAccount(
    std::string_view tenant_id,
    std::string_view user_uuid) const {
    if (tenant_id.empty() || user_uuid.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "tenant_id and user_uuid are required");
    }
    std::vector<PersonaMetadataRecord> out;
    std::lock_guard lock(mutex_);
    for (const auto& [key, record] : records_) {
        if (record.tenant_id == tenant_id && record.user_uuid == user_uuid) {
            out.push_back(record);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.persona_id < rhs.persona_id;
    });
    return out;
}

ServerDefaultPersonaMetadataStore::ServerDefaultPersonaMetadataStore(std::vector<PersonaMetadataRecord> records) {
    for (auto& record : records) {
        if (record.persona_id.empty() && !record.personality.name.empty()) {
            record.persona_id = record.personality.name;
        }
        if (!record.persona_id.empty()) {
            if (record.personality.name.empty()) {
                record.personality.name = record.persona_id;
            }
            records_[record.persona_id] = std::move(record);
        }
    }
}

core::Status ServerDefaultPersonaMetadataStore::EnsureSchema() {
    return core::Status::Ok();
}

core::Status ServerDefaultPersonaMetadataStore::Upsert(PersonaMetadataRecord record) {
    if (record.persona_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "persona_id is required");
    }
    if (record.personality.name.empty()) {
        record.personality.name = record.persona_id;
    }
    std::lock_guard lock(mutex_);
    records_[record.persona_id] = std::move(record);
    return core::Status::Ok();
}

core::Result<PersonaMetadataRecord> ServerDefaultPersonaMetadataStore::Get(std::string_view tenant_id,
                                                                           std::string_view user_uuid,
                                                                           std::string_view persona_id) const {
    if (persona_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "persona_id is required");
    }
    std::lock_guard lock(mutex_);
    auto it = records_.find(std::string(persona_id));
    if (it == records_.end()) {
        return core::Status::Error(core::ErrorCode::NotFound, "server default persona metadata not found");
    }
    auto record = it->second;
    record.tenant_id = tenant_id.empty() ? "default" : std::string(tenant_id);
    record.user_uuid = std::string(user_uuid);
    return record;
}

core::Result<std::vector<PersonaMetadataRecord>> ServerDefaultPersonaMetadataStore::ListByAccount(
    std::string_view tenant_id,
    std::string_view user_uuid) const {
    std::vector<PersonaMetadataRecord> out;
    std::lock_guard lock(mutex_);
    out.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        auto copy = record;
        copy.tenant_id = tenant_id.empty() ? "default" : std::string(tenant_id);
        copy.user_uuid = std::string(user_uuid);
        out.push_back(std::move(copy));
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.persona_id < rhs.persona_id;
    });
    return out;
}

OverlayPersonaMetadataStore::OverlayPersonaMetadataStore(std::shared_ptr<IPersonaMetadataStore> defaults,
                                                         std::shared_ptr<IPersonaMetadataStore> account)
    : defaults_(std::move(defaults)),
      account_(std::move(account)) {}

core::Status OverlayPersonaMetadataStore::EnsureSchema() {
    if (defaults_) {
        if (auto status = defaults_->EnsureSchema(); !status.ok()) {
            return status;
        }
    }
    return account_ ? account_->EnsureSchema() : core::Status::Ok();
}

core::Status OverlayPersonaMetadataStore::Upsert(PersonaMetadataRecord record) {
    if (!account_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "account persona metadata store is not configured");
    }
    return account_->Upsert(std::move(record));
}

core::Result<PersonaMetadataRecord> OverlayPersonaMetadataStore::Get(std::string_view tenant_id,
                                                                     std::string_view user_uuid,
                                                                     std::string_view persona_id) const {
    if (defaults_) {
        auto record = defaults_->Get(tenant_id, user_uuid, persona_id);
        if (record.ok() || record.status().code() != core::ErrorCode::NotFound) {
            return record;
        }
    }
    if (!account_) {
        return core::Status::Error(core::ErrorCode::NotFound, "persona metadata not found");
    }
    return account_->Get(tenant_id, user_uuid, persona_id);
}

core::Result<std::vector<PersonaMetadataRecord>> OverlayPersonaMetadataStore::ListByAccount(
    std::string_view tenant_id,
    std::string_view user_uuid) const {
    std::vector<PersonaMetadataRecord> out;
    std::unordered_set<std::string> default_ids;
    if (defaults_) {
        auto defaults = defaults_->ListByAccount(tenant_id, user_uuid);
        if (!defaults.ok()) {
            return defaults.status();
        }
        for (auto& record : defaults.value()) {
            default_ids.insert(record.persona_id);
            out.push_back(std::move(record));
        }
    }
    if (account_) {
        auto account = account_->ListByAccount(tenant_id, user_uuid);
        if (!account.ok()) {
            return account.status();
        }
        for (auto& record : account.value()) {
            if (!default_ids.contains(record.persona_id)) {
                out.push_back(std::move(record));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.persona_id < rhs.persona_id;
    });
    return out;
}

SqlitePersonaMetadataStore::SqlitePersonaMetadataStore(std::string database_path)
    : pool_(MakePersonaMetadataPool(std::move(database_path))) {}

SqlitePersonaMetadataStore::SqlitePersonaMetadataStore(std::shared_ptr<SqliteConnectionPool> pool)
    : pool_(std::move(pool)) {}

core::Status SqlitePersonaMetadataStore::EnsureSchema() {
    if (!pool_) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "persona metadata store pool is not configured");
    }
    storage::sqlite::SqliteMigrationRunner runner(pool_);
    std::array<storage::sqlite::ISqliteMigrationSource*, 1> sources{this};
    return runner.ApplyAll(sources);
}

namespace {

core::Status ApplyPersonaMetadataV1(storage::sqlite::SqliteConnection& connection) {
    return connection.Execute(
        "CREATE TABLE IF NOT EXISTS gateway_persona_metadata ("
        "tenant_id TEXT NOT NULL,"
        "user_uuid TEXT NOT NULL,"
        "persona_id TEXT NOT NULL,"
        "record_json TEXT NOT NULL,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "PRIMARY KEY(tenant_id,user_uuid,persona_id)"
        ")");
}

const std::array<storage::sqlite::SqliteMigrationStep, 1> kPersonaMetadataMigrations{{
    {
        .version = 1,
        .name = "create_gateway_persona_metadata",
        .checksum = "gateway_persona_metadata_v1_create_table",
        .apply = ApplyPersonaMetadataV1,
    },
}};

}

std::string_view SqlitePersonaMetadataStore::MigrationNamespace() const noexcept {
    return "gateway_persona_metadata";
}

std::span<const storage::sqlite::SqliteMigrationStep>
SqlitePersonaMetadataStore::MigrationSteps() const noexcept {
    return kPersonaMetadataMigrations;
}

core::Status SqlitePersonaMetadataStore::Upsert(PersonaMetadataRecord record) {
    if (auto status = ValidatePersonaMetadata(record); !status.ok()) {
        return status;
    }
    auto lease_result = AcquirePersonaMetadataConnection(pool_, true);
    if (!lease_result.ok()) {
        return lease_result.status();
    }
    auto lease = std::move(lease_result).value();
    auto& connection = lease.connection();
    auto statement_result = connection.Prepare(
        "INSERT INTO gateway_persona_metadata(tenant_id,user_uuid,persona_id,record_json,created_at,updated_at) "
        "VALUES(?1,?2,?3,?4,?5,?5) "
        "ON CONFLICT(tenant_id,user_uuid,persona_id) DO UPDATE SET "
        "record_json=excluded.record_json,updated_at=excluded.updated_at");
    if (!statement_result.ok()) {
        return statement_result.status();
    }
    auto statement = std::move(statement_result).value();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto payload = PersonaMetadataRecordToJson(record).dump();
    if (auto status = statement.BindText(1, record.tenant_id.empty() ? std::string("default") : record.tenant_id); !status.ok()) return status;
    if (auto status = statement.BindText(2, record.user_uuid); !status.ok()) return status;
    if (auto status = statement.BindText(3, record.persona_id); !status.ok()) return status;
    if (auto status = statement.BindText(4, payload); !status.ok()) return status;
    if (auto status = statement.BindInt64(5, now); !status.ok()) return status;
    auto step = statement.Step();
    if (!step.ok()) {
        return step.status();
    }
    return core::Status::Ok();
}

core::Result<PersonaMetadataRecord> SqlitePersonaMetadataStore::Get(std::string_view tenant_id,
                                                                    std::string_view user_uuid,
                                                                    std::string_view persona_id) const {
    auto lease_result = AcquirePersonaMetadataConnection(pool_, false);
    if (!lease_result.ok()) {
        return lease_result.status();
    }
    auto lease = std::move(lease_result).value();
    auto& connection = lease.connection();
    auto statement_result = connection.Prepare(
        "SELECT record_json FROM gateway_persona_metadata "
        "WHERE tenant_id=?1 AND user_uuid=?2 AND persona_id=?3");
    if (!statement_result.ok()) {
        return statement_result.status();
    }
    auto statement = std::move(statement_result).value();
    if (auto status = statement.BindText(1, tenant_id.empty() ? std::string("default") : std::string(tenant_id)); !status.ok()) return status;
    if (auto status = statement.BindText(2, std::string(user_uuid)); !status.ok()) return status;
    if (auto status = statement.BindText(3, std::string(persona_id)); !status.ok()) return status;
    auto step = statement.Step();
    if (!step.ok()) {
        return step.status();
    }
    if (step.value() != SqliteStepResult::Row) {
        return core::Status::Error(core::ErrorCode::NotFound, "persona metadata not found");
    }
    return PersonaMetadataRecordFromJson(statement.ColumnText(0));
}

core::Result<std::vector<PersonaMetadataRecord>> SqlitePersonaMetadataStore::ListByAccount(
    std::string_view tenant_id,
    std::string_view user_uuid) const {
    auto lease_result = AcquirePersonaMetadataConnection(pool_, false);
    if (!lease_result.ok()) {
        return lease_result.status();
    }
    auto lease = std::move(lease_result).value();
    auto& connection = lease.connection();
    auto statement_result = connection.Prepare(
        "SELECT record_json FROM gateway_persona_metadata "
        "WHERE tenant_id=?1 AND user_uuid=?2 ORDER BY persona_id");
    if (!statement_result.ok()) {
        return statement_result.status();
    }
    auto statement = std::move(statement_result).value();
    if (auto status = statement.BindText(1, tenant_id.empty() ? std::string("default") : std::string(tenant_id)); !status.ok()) return status;
    if (auto status = statement.BindText(2, std::string(user_uuid)); !status.ok()) return status;

    std::vector<PersonaMetadataRecord> out;
    while (true) {
        auto step = statement.Step();
        if (!step.ok()) {
            return step.status();
        }
        if (step.value() == SqliteStepResult::Done) {
            break;
        }
        auto record = PersonaMetadataRecordFromJson(statement.ColumnText(0));
        if (!record.ok()) {
            return record.status();
        }
        out.push_back(std::move(record).value());
    }
    return out;
}

RedisPersonaMetadataCache::RedisPersonaMetadataCache(std::shared_ptr<semantic_cache::RedisConnectionPool> redis,
                                                     std::string key_prefix)
    : redis_(std::move(redis)),
      key_prefix_(std::move(key_prefix)) {
    if (key_prefix_.empty()) {
        key_prefix_ = "agent:gateway:persona";
    }
}

core::Status RedisPersonaMetadataCache::EnsureSchema() {
    if (!redis_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "redis persona metadata cache is not configured");
    }
    return redis_->running()
        ? core::Status::Ok()
        : core::Status::Error(core::ErrorCode::FailedPrecondition, "redis persona metadata cache is not running");
}

core::Status RedisPersonaMetadataCache::Upsert(PersonaMetadataRecord record) {
    if (auto status = ValidatePersonaMetadata(record); !status.ok()) {
        return status;
    }
    return redis_->Set(Key(record.tenant_id, record.user_uuid, record.persona_id),
                       PersonaMetadataRecordToJson(record).dump());
}

core::Result<PersonaMetadataRecord> RedisPersonaMetadataCache::Get(std::string_view tenant_id,
                                                                   std::string_view user_uuid,
                                                                   std::string_view persona_id) const {
    auto payload = redis_->Get(Key(tenant_id.empty() ? "default" : tenant_id, user_uuid, persona_id));
    if (!payload.ok()) {
        return payload.status();
    }
    return PersonaMetadataRecordFromJson(payload.value());
}

core::Result<std::vector<PersonaMetadataRecord>> RedisPersonaMetadataCache::ListByAccount(
    std::string_view tenant_id,
    std::string_view user_uuid) const {
    if (tenant_id.empty() || user_uuid.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "tenant_id and user_uuid are required");
    }
    const auto prefix = key_prefix_ + ":tenant:" + std::string(tenant_id) +
                        ":user:" + std::string(user_uuid) + ":persona:";
    auto keys = redis_->Scan(prefix + "*");
    if (!keys.ok()) {
        return keys.status();
    }
    if (keys.value().empty()) {
        return std::vector<PersonaMetadataRecord>{};
    }
    auto values = redis_->MGet(keys.value());
    if (!values.ok()) {
        return values.status();
    }
    std::vector<PersonaMetadataRecord> out;
    for (const auto& payload : values.value()) {
        if (payload.empty()) {
            continue;
        }
        auto record = PersonaMetadataRecordFromJson(payload);
        if (!record.ok()) {
            return record.status();
        }
        out.push_back(std::move(record).value());
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.persona_id < rhs.persona_id;
    });
    return out;
}

std::string RedisPersonaMetadataCache::Key(std::string_view tenant_id,
                                           std::string_view user_uuid,
                                           std::string_view persona_id) const {
    return key_prefix_ + ":tenant:" + std::string(tenant_id) +
           ":user:" + std::string(user_uuid) +
           ":persona:" + std::string(persona_id);
}

CachedPersonaMetadataStore::CachedPersonaMetadataStore(std::shared_ptr<IPersonaMetadataStore> primary,
                                                       std::shared_ptr<IPersonaMetadataStore> cache)
    : primary_(std::move(primary)),
      cache_(std::move(cache)) {}

core::Status CachedPersonaMetadataStore::EnsureSchema() {
    if (!primary_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "primary persona metadata store is not configured");
    }
    if (auto status = primary_->EnsureSchema(); !status.ok()) {
        return status;
    }
    if (cache_) {
        return cache_->EnsureSchema();
    }
    return core::Status::Ok();
}

core::Status CachedPersonaMetadataStore::Upsert(PersonaMetadataRecord record) {
    if (!primary_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "primary persona metadata store is not configured");
    }
    auto copy = record;
    auto status = primary_->Upsert(std::move(record));
    if (!status.ok()) {
        return status;
    }
    if (cache_) {
        auto cached = cache_->Upsert(std::move(copy));
        if (!cached.ok()) {
            return cached;
        }
    }
    return core::Status::Ok();
}

core::Result<PersonaMetadataRecord> CachedPersonaMetadataStore::Get(std::string_view tenant_id,
                                                                    std::string_view user_uuid,
                                                                    std::string_view persona_id) const {
    if (cache_) {
        auto cached = cache_->Get(tenant_id, user_uuid, persona_id);
        if (cached.ok() || cached.status().code() != core::ErrorCode::NotFound) {
            return cached;
        }
    }
    if (!primary_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "primary persona metadata store is not configured");
    }
    auto record = primary_->Get(tenant_id, user_uuid, persona_id);
    if (!record.ok()) {
        return record.status();
    }
    if (cache_) {
        static_cast<void>(cache_->Upsert(record.value()));
    }
    return record;
}

core::Result<std::vector<PersonaMetadataRecord>> CachedPersonaMetadataStore::ListByAccount(
    std::string_view tenant_id,
    std::string_view user_uuid) const {
    if (!primary_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "primary persona metadata store is not configured");
    }
    auto records = primary_->ListByAccount(tenant_id, user_uuid);
    if (!records.ok()) {
        return records.status();
    }
    if (cache_) {
        for (const auto& record : records.value()) {
            static_cast<void>(cache_->Upsert(record));
        }
    }
    return records;
}

PersonaGatewayService::PersonaGatewayService(persona::SessionManager& sessions,
                                             persona::PersonaRuntime& runtime,
                                             IClassroomScheduler* classroom_scheduler,
                                             std::shared_ptr<IReportEvaluator> report_evaluator,
                                             std::shared_ptr<IPersonaMetadataStore> persona_metadata_store,
                                             core::LoggerAdapter logger,
                                             GatewayStreamingOptions streaming)
    : sessions_(sessions),
      runtime_(runtime),
      interaction_(sessions, runtime, logger),
      classroom_scheduler_(classroom_scheduler),
      report_evaluator_(std::move(report_evaluator)),
      persona_metadata_store_(std::move(persona_metadata_store)),
      logger_(std::move(logger)),
      streaming_(streaming),
      chat_streams_(streaming.replay) {}

core::Result<PersonaMetadataGatewayResponse> PersonaGatewayService::UpsertPersonaMetadata(
    PersonaMetadataGatewayRequest request) {
    const auto started = std::chrono::steady_clock::now();
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    if (!persona_metadata_store_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "persona metadata store is not configured");
    }
    if (request.persona_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "persona_id is required");
    }
    if (request.personality.name.empty()) {
        request.personality.name = DefaultPersonaName(request.persona_id);
    }

    PersonaMetadataRecord record;
    record.tenant_id = request.tenant_id.empty() ? "default" : std::move(request.tenant_id);
    record.user_uuid = std::move(request.user_uuid);
    record.persona_id = std::move(request.persona_id);
    record.personality = std::move(request.personality);
    record.emotion_prompt_config = std::move(request.emotion_prompt_config);
    record.emotion_state_config = request.emotion_state_config;
    if (auto status = persona_metadata_store_->Upsert(record); !status.ok()) {
        return status;
    }

    PersonaMetadataGatewayResponse response;
    response.trace_id = request.trace_id;
    response.session_id.clear();
    response.latency = Since(started);
    response.persona = std::move(record);
    return response;
}

core::Result<PersonaMetadataGatewayResponse> PersonaGatewayService::GetPersonaMetadata(
    std::string_view tenant_id,
    std::string_view user_uuid,
    std::string_view persona_id,
    std::string trace_id) {
    const auto started = std::chrono::steady_clock::now();
    trace_id = EnsureTrace(std::move(trace_id));
    if (!persona_metadata_store_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "persona metadata store is not configured");
    }
    auto record = persona_metadata_store_->Get(tenant_id.empty() ? "default" : tenant_id, user_uuid, persona_id);
    if (!record.ok()) {
        return record.status();
    }
    PersonaMetadataGatewayResponse response;
    response.trace_id = std::move(trace_id);
    response.latency = Since(started);
    response.persona = std::move(record).value();
    return response;
}

core::Result<std::vector<PersonaMetadataRecord>> PersonaGatewayService::ListPersonaMetadataByAccount(
    std::string_view tenant_id,
    std::string_view user_uuid,
    std::string trace_id) {
    const auto started = std::chrono::steady_clock::now();
    trace_id = EnsureTrace(std::move(trace_id));
    if (!persona_metadata_store_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "persona metadata store is not configured");
    }
    auto records = persona_metadata_store_->ListByAccount(tenant_id.empty() ? "default" : tenant_id, user_uuid);
    if (!records.ok()) {
        return records.status();
    }
    return records.value();
}

core::Result<SessionGatewayResponse> PersonaGatewayService::CreateSession(CreateSessionGatewayRequest request) {
    const auto started = std::chrono::steady_clock::now();
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    if (request.persona_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "persona_id is required");
    }
    if (persona_metadata_store_) {
        auto metadata = persona_metadata_store_->Get(
            request.tenant_id.empty() ? "default" : request.tenant_id,
            request.user_uuid,
            request.persona_id);
        if (!metadata.ok()) {
            return metadata.status();
        }
        auto record = std::move(metadata).value();
        request.personality = std::move(record.personality);
        request.emotion_prompt_config = std::move(record.emotion_prompt_config);
        request.emotion_state_config = record.emotion_state_config;
    } else if (request.personality.name.empty()) {
        request.personality.name = DefaultPersonaName(request.persona_id);
    }

    persona::CreateSessionRequest create;
    create.tenant_id = request.tenant_id.empty() ? "default" : request.tenant_id;
    create.user_uuid = std::move(request.user_uuid);
    create.persona_id = std::move(request.persona_id);
    create.session_id = std::move(request.session_id);
    create.trace_id = request.trace_id;
    create.personality = std::move(request.personality);
    create.emotion_prompt_config = std::move(request.emotion_prompt_config);
    create.emotion_state_config = request.emotion_state_config;
    create.time_awareness = true;

    auto snapshot = interaction_.CreateSession(std::move(create));
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    if (classroom_scheduler_ && !request.classroom_id.empty()) {
        std::vector<persona::SessionSnapshot> classroom_snapshots;
        classroom_snapshots.push_back(snapshot.value());

        if (persona_metadata_store_) {
            auto records = persona_metadata_store_->ListByAccount(
                request.tenant_id.empty() ? "default" : request.tenant_id,
                snapshot.value().user_uuid);
            if (!records.ok()) {
                sessions_.CloseSession(snapshot.value().session_id, request.trace_id);
                return records.status();
            }
            for (auto& record : records.value()) {
                if (record.persona_id == snapshot.value().persona_id) {
                    continue;
                }
                persona::CreateSessionRequest classroom_create;
                classroom_create.tenant_id = request.tenant_id.empty() ? "default" : request.tenant_id;
                classroom_create.user_uuid = snapshot.value().user_uuid;
                classroom_create.persona_id = record.persona_id;
                classroom_create.trace_id = request.trace_id;
                classroom_create.personality = std::move(record.personality);
                classroom_create.emotion_prompt_config = std::move(record.emotion_prompt_config);
                classroom_create.emotion_state_config = record.emotion_state_config;
                classroom_create.time_awareness = true;
                auto classroom_snapshot = sessions_.CreateSession(std::move(classroom_create));
                if (!classroom_snapshot.ok()) {
                    for (const auto& created : classroom_snapshots) {
                        sessions_.CloseSession(created.session_id, request.trace_id);
                    }
                    return classroom_snapshot.status();
                }
                classroom_snapshots.push_back(std::move(classroom_snapshot).value());
            }
        }

        for (const auto& classroom_snapshot : classroom_snapshots) {
            CreateSessionGatewayRequest registration_request = request;
            registration_request.persona_id = classroom_snapshot.persona_id;
            registration_request.context_ids.clear();
            registration_request.context_patterns.clear();
            registration_request.default_persona = classroom_snapshot.persona_id == snapshot.value().persona_id
                ? request.default_persona
                : false;
            if (classroom_snapshot.persona_id == snapshot.value().persona_id) {
                registration_request.context_ids = request.context_ids;
                registration_request.context_patterns = request.context_patterns;
            }
            auto registration = BuildClassroomRegistration(registration_request, classroom_snapshot);
            auto registered_status = classroom_scheduler_->RegisterPersona(std::move(registration));
            if (!registered_status.ok()) {
                for (const auto& created : classroom_snapshots) {
                    static_cast<void>(classroom_scheduler_->UnregisterSession(created.session_id));
                    sessions_.CloseSession(created.session_id, request.trace_id);
                }
                return registered_status;
            }
        }
    }

    SessionGatewayResponse response;
    response.trace_id = request.trace_id;
    response.session_id = snapshot.value().session_id;
    response.latency = Since(started);
    response.session = std::move(snapshot).value();
    return response;
}

core::Result<SessionGatewayResponse> PersonaGatewayService::GetSession(std::string_view session_id,
                                                                       std::string trace_id,
                                                                       std::string_view authenticated_user_uuid) {
    const auto started = std::chrono::steady_clock::now();
    trace_id = EnsureTrace(std::move(trace_id));
    auto snapshot = interaction_.GetSession(persona::PersonaSessionQuery{
        std::string(session_id), trace_id, std::string(authenticated_user_uuid)});
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    SessionGatewayResponse response;
    response.trace_id = trace_id;
    response.session_id = snapshot.value().session_id;
    response.latency = Since(started);
    response.session = std::move(snapshot).value();
    return response;
}

core::Result<SessionGatewayResponse> PersonaGatewayService::CloseSession(CloseSessionGatewayRequest request) {
    const auto started = std::chrono::steady_clock::now();
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    auto closed = interaction_.CloseSession(persona::ClosePersonaSessionRequest{
        request.session_id,
        request.trace_id,
        request.authenticated_user_uuid,
        request.reason,
    });
    if (!closed.ok()) {
        return closed.status();
    }
    auto snapshot = std::move(closed).value();
    if (classroom_scheduler_) {
        auto unregister_status = classroom_scheduler_->UnregisterSession(request.session_id);
        if (!unregister_status.ok() && unregister_status.code() != core::ErrorCode::NotFound) {
            logger_.warn("[trace={}] [gateway] classroom unregister failed session={} error={}",
                         request.trace_id,
                         request.session_id,
                         unregister_status.message());
        }
    }
    SessionGatewayResponse response;
    response.trace_id = request.trace_id;
    response.session_id = snapshot.session_id;
    response.latency = Since(started);
    response.session = std::move(snapshot);
    return response;
}

core::Result<ChatGatewayResponse> PersonaGatewayService::Chat(ChatGatewayRequest request) {
    std::promise<core::Result<ChatGatewayResponse>> promise;
    auto future = promise.get_future();
    auto status = SubmitChat(
        std::move(request),
        [&promise](core::Result<ChatGatewayResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    if (!status.ok()) {
        return status;
    }
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        return core::Status::Error(core::ErrorCode::Timeout, "persona gateway callback timed out");
    }
    return future.get();
}

core::Result<ClassroomGatewayResponse> PersonaGatewayService::ClassroomMessage(
    ClassroomMessageGatewayRequest request) {
    std::promise<core::Result<ClassroomGatewayResponse>> promise;
    auto future = promise.get_future();
    auto status = SubmitClassroomMessage(
        std::move(request),
        [&promise](core::Result<ClassroomGatewayResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    if (!status.ok()) {
        return status;
    }
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        return core::Status::Error(core::ErrorCode::Timeout, "classroom message callback timed out");
    }
    return future.get();
}

core::Result<ClassroomGatewayResponse> PersonaGatewayService::ClassroomProactive(
    ClassroomProactiveGatewayRequest request) {
    std::promise<core::Result<ClassroomGatewayResponse>> promise;
    auto future = promise.get_future();
    auto status = SubmitClassroomProactive(
        std::move(request),
        [&promise](core::Result<ClassroomGatewayResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    if (!status.ok()) {
        return status;
    }
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        return core::Status::Error(core::ErrorCode::Timeout, "classroom proactive callback timed out");
    }
    return future.get();
}

core::Result<ClassroomGatewayResponse> PersonaGatewayService::ClassroomPoll(ClassroomPollGatewayRequest request) {
    std::promise<core::Result<ClassroomGatewayResponse>> promise;
    auto future = promise.get_future();
    auto status = SubmitClassroomPoll(
        std::move(request),
        [&promise](core::Result<ClassroomGatewayResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    if (!status.ok()) {
        return status;
    }
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        return core::Status::Error(core::ErrorCode::Timeout, "classroom poll callback timed out");
    }
    return future.get();
}

core::Result<TrainingReportGatewayResponse> PersonaGatewayService::TrainingReport(
    TrainingReportGatewayRequest request) {
    const auto started = std::chrono::steady_clock::now();
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    auto snapshot = sessions_.GetSessionSnapshot(request.session_id);
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    if (auto owner = EnsureSessionOwner(snapshot.value(), request.authenticated_user_uuid); !owner.ok()) {
        return owner;
    }

    TrainingReportGatewayResponse response;
    response.trace_id = request.trace_id;
    response.session_id = snapshot.value().session_id;
    response.latency = Since(started);
    response.generated_at = NowIso8601Utc();
    response.total_turns = snapshot.value().metrics.turn_count;
    response.metrics = snapshot.value().metrics;
    response.summary = "Session metrics report generated.";
    if (report_evaluator_) {
        ReportEvaluationRequest evaluation_request;
        evaluation_request.user_uuid = snapshot.value().user_uuid;
        evaluation_request.session_id = snapshot.value().session_id;
        evaluation_request.trace_id = request.trace_id;
        auto evaluated = report_evaluator_->Evaluate(evaluation_request);
        if (evaluated.ok()) {
            response.evaluation = std::move(evaluated).value();
            response.summary = "Session report evaluation completed.";
        } else {
            logger_.warn("report evaluation failed trace_id={} session_id={} error={}",
                         request.trace_id,
                         snapshot.value().session_id,
                         evaluated.status().message());
            response.evaluation = {
                {"error", evaluated.status().message()},
            };
            response.summary = "Session report evaluation unavailable; metrics are available.";
        }
    }
    return response;
}

core::Result<SystemStatsGatewayResponse> PersonaGatewayService::SystemStats(std::string trace_id) {
    const auto started = std::chrono::steady_clock::now();
    SystemStatsGatewayResponse response;
    response.trace_id = EnsureTrace(std::move(trace_id));
    response.latency = Since(started);
    const auto snapshot = interaction_.SystemSnapshot();
    response.session_count = snapshot.session_count;
    response.pools = snapshot.pools;
    return response;
}

core::Status PersonaGatewayService::SubmitChat(ChatGatewayRequest request, ChatCallback callback) {
    const auto started = std::chrono::steady_clock::now();
    if (!callback) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "chat callback is required");
    }
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    if (request.session_id.empty() || request.message.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "session_id and message are required");
    }
    auto before = sessions_.GetSessionSnapshot(request.session_id);
    if (!before.ok()) {
        return before.status();
    }
    if (auto owner = EnsureSessionOwner(before.value(), request.authenticated_user_uuid); !owner.ok()) {
        return owner;
    }
    if (before.value().status != persona::SessionStatus::Active) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "session is not active");
    }
    if (request.persona_id.empty()) {
        request.persona_id = before.value().persona_id;
    }

    persona::ChatRequest chat;
    chat.session_id = request.session_id;
    chat.user_input = request.message;
    chat.trace_id = request.trace_id;
    chat.model = request.model;
    chat.context_id = request.mode;
    chat.stream = request.stream;
    chat.event_sink = std::move(request.event_sink);

    auto status = interaction_.SubmitTurn(
        persona::PersonaTurnRequest{std::move(chat), request.authenticated_user_uuid},
        [this, request = std::move(request), callback = std::move(callback), started](
            core::Result<persona::ChatResponse> result) mutable {
            if (!result.ok()) {
                sessions_.RecordRequestMetrics(request.session_id, Since(started), false, request.trace_id);
                callback(result.status());
                return;
            }
            callback(ToChatGatewayResponse(request, result.value(), started));
        });
    if (!status.ok()) {
        sessions_.RecordRequestMetrics(request.session_id, Since(started), false, request.trace_id);
    }
    return status;
}

core::Status PersonaGatewayService::SubmitClassroomMessage(ClassroomMessageGatewayRequest request,
                                                           ClassroomCallback callback) {
    if (!callback) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "classroom callback is required");
    }
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    auto route = ResolveClassroomRoute(request);
    if (!route.ok()) {
        return route.status();
    }
    if (classroom_scheduler_) {
        auto user_status = classroom_scheduler_->OnUserMessage(route.value().classroom_id, route.value().persona_id);
        if (!user_status.ok()) {
            return user_status;
        }
    }
    ChatGatewayRequest chat;
    chat.trace_id = request.trace_id;
    chat.session_id = route.value().session_id;
    chat.authenticated_user_uuid = request.authenticated_user_uuid;
    chat.persona_id = route.value().persona_id;
    chat.mode = request.broadcast ? "classroom_broadcast" : "classroom_message";
    chat.message = std::move(request.message);
    chat.model = std::move(request.model);
    const auto classroom_id = route.value().classroom_id;
    return SubmitChat(
        std::move(chat),
        [this, callback = std::move(callback), classroom_id](core::Result<ChatGatewayResponse> result) mutable {
            if (!result.ok()) {
                callback(result.status());
                return;
            }
            callback(ToClassroomGatewayResponse(result.value(), classroom_id));
        });
}

core::Status PersonaGatewayService::SubmitClassroomProactive(ClassroomProactiveGatewayRequest request,
                                                             ClassroomCallback callback) {
    if (!callback) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "classroom callback is required");
    }
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    auto route = ResolveClassroomRoute(request);
    if (!route.ok()) {
        return route.status();
    }
    ChatGatewayRequest chat;
    chat.trace_id = request.trace_id;
    chat.session_id = route.value().session_id;
    chat.authenticated_user_uuid = request.authenticated_user_uuid;
    chat.persona_id = route.value().persona_id;
    chat.mode = "classroom_proactive";
    chat.message = "[proactive] frontend requested proactive generation";
    chat.model = std::move(request.model);
    const auto classroom_id = route.value().classroom_id;
    return SubmitChat(
        std::move(chat),
        [this, callback = std::move(callback), classroom_id](core::Result<ChatGatewayResponse> result) mutable {
            if (!result.ok()) {
                callback(result.status());
                return;
            }
            if (classroom_scheduler_ && !result.value().content.empty()) {
                classroom_scheduler_->OnProactiveDelivered(classroom_id, result.value().persona_id);
            }
            callback(ToClassroomGatewayResponse(result.value(), classroom_id, !result.value().content.empty()));
        });
}

core::Status PersonaGatewayService::SubmitClassroomPoll(ClassroomPollGatewayRequest request,
                                                        ClassroomCallback callback) {
    if (!callback) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "classroom callback is required");
    }
    if (!classroom_scheduler_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "classroom scheduler is not configured");
    }
    request.trace_id = EnsureTrace(std::move(request.trace_id));
    ClassroomPollRequest poll;
    poll.trace_id = request.trace_id;
    poll.classroom_id = request.classroom_id;
    poll.persona_hint = request.persona_id;
    poll.context_id = request.context_id;
    poll.system_event = request.system_event;
    poll.system_event_content = request.system_event_content;
    auto decision = classroom_scheduler_->Poll(poll);
    if (!decision.ok()) {
        return decision.status();
    }
    if (!decision.value().should_speak) {
        ClassroomGatewayResponse response;
        response.trace_id = request.trace_id;
        response.classroom_id = request.classroom_id;
        response.session_id = decision.value().session_id;
        response.speaker_persona_id = decision.value().persona_id;
        response.should_speak = false;
        callback(std::move(response));
        return core::Status::Ok();
    }

    ChatGatewayRequest chat;
    chat.trace_id = request.trace_id;
    chat.session_id = decision.value().session_id;
    chat.authenticated_user_uuid = request.authenticated_user_uuid;
    chat.persona_id = decision.value().persona_id;
    chat.mode = "classroom_proactive";
    chat.message = decision.value().trigger;
    chat.model = std::move(request.model);
    const auto classroom_id = decision.value().classroom_id;
    return SubmitChat(
        std::move(chat),
        [this, callback = std::move(callback), classroom_id](core::Result<ChatGatewayResponse> result) mutable {
            if (!result.ok()) {
                callback(result.status());
                return;
            }
            if (classroom_scheduler_ && !result.value().content.empty()) {
                classroom_scheduler_->OnProactiveDelivered(classroom_id, result.value().persona_id);
            }
            callback(ToClassroomGatewayResponse(result.value(), classroom_id, !result.value().content.empty()));
        });
}

core::Result<ChatGatewayResponse> PersonaGatewayService::ToChatGatewayResponse(
    const ChatGatewayRequest& request,
    const persona::ChatResponse& result,
    std::chrono::steady_clock::time_point started) {
    ChatGatewayResponse response;
    response.trace_id = request.trace_id;
    response.session_id = request.session_id;
    response.persona_id = request.persona_id;
    response.latency = Since(started);
    response.content = result.response;
    response.user_emotion = result.user_emotion;
    response.ai_emotion = result.ai_emotion;
    response.l0_hit = result.l0_hit;
    response.l3_hit = result.l3_hit;
    response.answer_cache = result.answer_cache;
    response.pipeline_latency = result.latency;
    response.turn_index = result.turn_index;
    return response;
}

ClassroomGatewayResponse PersonaGatewayService::ToClassroomGatewayResponse(const ChatGatewayResponse& result,
                                                                           std::string classroom_id,
                                                                           bool should_speak) const {
    ClassroomGatewayResponse response;
    response.trace_id = result.trace_id;
    response.classroom_id = std::move(classroom_id);
    response.session_id = result.session_id;
    response.latency = result.latency;
    response.speaker_persona_id = result.persona_id;
    response.content = result.content;
    response.should_speak = should_speak;
    response.user_emotion = result.user_emotion;
    response.ai_emotion = result.ai_emotion;
    response.turn_index = result.turn_index;
    return response;
}

core::Result<ClassroomRouteResult> PersonaGatewayService::ResolveClassroomRoute(
    const ClassroomMessageGatewayRequest& request) {
    if (!classroom_scheduler_) {
        if (request.session_id.empty()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument, "session_id is required without classroom scheduler");
        }
        ClassroomRouteResult result;
        result.classroom_id = request.classroom_id;
        result.persona_id = request.target_persona_id;
        result.session_id = request.session_id;
        result.context_id = request.context_id;
        return result;
    }
    ClassroomRouteRequest route;
    route.classroom_id = request.classroom_id;
    route.context_id = request.context_id;
    route.persona_hint = request.target_persona_id;
    route.session_id = request.session_id;
    return classroom_scheduler_->Resolve(route);
}

core::Result<ClassroomRouteResult> PersonaGatewayService::ResolveClassroomRoute(
    const ClassroomProactiveGatewayRequest& request) {
    if (!classroom_scheduler_) {
        if (request.session_id.empty()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument, "session_id is required without classroom scheduler");
        }
        ClassroomRouteResult result;
        result.classroom_id = request.classroom_id;
        result.persona_id = request.persona_id;
        result.session_id = request.session_id;
        result.context_id = request.context_id;
        return result;
    }
    ClassroomRouteRequest route;
    route.classroom_id = request.classroom_id;
    route.context_id = request.context_id;
    route.persona_hint = request.persona_id;
    route.session_id = request.session_id;
    return classroom_scheduler_->Resolve(route);
}

ClassroomPersonaRegistration PersonaGatewayService::BuildClassroomRegistration(
    const CreateSessionGatewayRequest& request,
    const persona::SessionSnapshot& snapshot) const {
    ClassroomPersonaRegistration registration;
    registration.classroom_id = request.classroom_id;
    registration.persona_id = snapshot.persona_id;
    registration.session_id = snapshot.session_id;
    registration.context_ids.insert(request.context_ids.begin(), request.context_ids.end());
    registration.context_patterns = request.context_patterns;
    registration.default_persona = request.default_persona;
    registration.agent.proactive_level = ProactiveLevelFromString(request.proactive_level);
    return registration;
}

core::Result<persona::ChatResponse> PersonaGatewayService::SubmitChatAndWait(persona::ChatRequest request) {
    std::promise<core::Result<persona::ChatResponse>> promise;
    auto future = promise.get_future();
    auto status = runtime_.SubmitChat(
        std::move(request),
        [&promise](core::Result<persona::ChatResponse> result) mutable {
            promise.set_value(std::move(result));
        });
    if (!status.ok()) {
        return status;
    }
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        return core::Status::Error(core::ErrorCode::Timeout, "persona runtime callback timed out");
    }
    return future.get();
}

std::string PersonaGatewayService::EnsureTrace(std::string trace_id) const {
    if (!trace_id.empty()) {
        return trace_id;
    }
    if (auto current = core::CurrentTraceId(); current != "-") {
        return std::string(current);
    }
    return core::GenerateTraceId();
}

}
