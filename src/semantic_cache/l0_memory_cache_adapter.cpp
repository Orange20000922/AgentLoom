#include "l0_memory_cache_adapter.h"

#include "redis_connection_pool.h"
#include "sqlite/sqlite_connection_pool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace agent::semantic_cache {
namespace {

std::int64_t NowUnixMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool MatchesOwner(const CacheLookupRequest& req, const storage::CacheRecord& record) {
    if (!req.tenant_id.empty() && record.metadata.tenant_id != req.tenant_id) {
        return false;
    }
    if (!req.user_id.empty() && record.metadata.user_id != req.user_id) {
        return false;
    }
    // L0 严格绑定当前 Session；缺少 session_id 的历史记录不能进入有 Session 约束的结果。
    if (!req.session_id.empty() && record.metadata.session_id != req.session_id) {
        return false;
    }
    return true;
}

bool IsRecentTurn(const CacheLookupRequest& req, const storage::CacheRecord& record) {
    if (const auto id = record.extra_metadata.find("turn_id");
        id != record.extra_metadata.end() && !id->second.empty()) {
        if (std::find(req.recent_turn_ids.begin(), req.recent_turn_ids.end(), id->second) !=
            req.recent_turn_ids.end()) return true;
        // 旧会话快照可能尚未携带 turn_id，仍需使用完整问答兼容匹配。
    }
    const std::string prefix = "用户: " + record.input + "\n助手: ";
    for (const auto& recent : req.recent_turns) {
        if (recent.rfind(prefix, 0) != 0) continue;
        if (recent.substr(prefix.size()) == record.response) return true;
    }
    return false;
}

std::string RecentTurnText(const storage::CacheRecord& record) {
    return "用户: " + record.input + "\n助手: " + record.response;
}

std::string RecentTurnKey(const storage::CacheRecord& record) {
    const auto id = record.extra_metadata.find("turn_id");
    if (id != record.extra_metadata.end() && !id->second.empty()) return "id:" + id->second;
    return "text:" + RecentTurnText(record);
}

bool IsExpired(const storage::CacheRecord& record, std::int64_t now_ms) {
    return record.metadata.expires_at_ms > 0 && record.metadata.expires_at_ms <= now_ms;
}

std::int64_t SecondsToMs(std::int64_t seconds) {
    if (seconds <= 0) {
        return 0;
    }
    constexpr std::int64_t kMaxSafeSeconds = std::numeric_limits<std::int64_t>::max() / 1000;
    if (seconds > kMaxSafeSeconds) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return seconds * 1000;
}

float TimeDecay(std::int64_t created_at_ms,
                std::int64_t now_ms,
                const L0MemoryCacheAdapterOptions& options) {
    if (created_at_ms <= 0 || now_ms <= created_at_ms) {
        return 1.0f;
    }

    const auto age_ms = now_ms - created_at_ms;
    const auto warm_ms = SecondsToMs(options.warm_window_seconds);
    const auto half_life_ms = SecondsToMs(options.half_life_seconds);
    const auto max_age_ms = SecondsToMs(options.max_age_seconds);

    if (max_age_ms > 0 && age_ms > max_age_ms) {
        return 0.0f;
    }
    if (age_ms <= warm_ms) {
        return 1.0f;
    }
    if (half_life_ms <= 0) {
        return 0.0f;
    }

    const double periods = static_cast<double>(age_ms - warm_ms) /
                           static_cast<double>(half_life_ms);
    return static_cast<float>(std::pow(0.5, periods));
}

struct RankedL0Hit {
    cache_vector::SearchWithContextResult hit;
    float decayed_score = 0.0f;
    float decay = 1.0f;
};

std::shared_ptr<storage::sqlite::SqliteConnectionPool> MakeL0SqlitePool(
    std::string sqlite_path) {
    if (sqlite_path.empty()) {
        return nullptr;
    }
    storage::sqlite::SqliteConnectionPoolOptions options;
    options.path = std::move(sqlite_path);
    options.read_connection_count = 4;
    options.write_connection_count = 1;
    options.busy_timeout_ms = 5000;
    options.enable_wal = true;
    return std::make_shared<storage::sqlite::SqliteConnectionPool>(std::move(options));
}

}

L0MemoryCacheAdapter::L0MemoryCacheAdapter(
    std::shared_ptr<::vector::EmbeddingPipeline> embedding,
    std::shared_ptr<cache_vector::VectorIndexManager> index,
    L0MemoryCacheAdapterOptions options)
    : embedding_(std::move(embedding)),
      index_(std::move(index)),
      options_(options) {
    if (index_) {
        fixed_index_entry_ = std::make_shared<SessionIndexEntry>(index_);
    }
}

L0MemoryCacheAdapter::L0MemoryCacheAdapter(
    std::shared_ptr<::vector::EmbeddingPipeline> embedding,
    std::shared_ptr<RedisConnectionPool> redis_pool,
    std::string sqlite_path,
    std::size_t max_cached_records,
    L0MemoryCacheAdapterOptions options)
    : embedding_(std::move(embedding)),
      redis_pool_(std::move(redis_pool)),
      sqlite_pool_(MakeL0SqlitePool(std::move(sqlite_path))),
      max_cached_records_(max_cached_records),
      options_(options),
      metadata_store_(std::make_shared<SqliteL0SessionBatchMetadataStore>(sqlite_pool_)) {
    auto sqlite_store = std::static_pointer_cast<SqliteL0SessionBatchMetadataStore>(metadata_store_);
    metadata_ready_ = [sqlite_store = std::move(sqlite_store)] {
        return EnsureL0MetadataReady(*sqlite_store);
    };
}

L0MemoryCacheAdapter::~L0MemoryCacheAdapter() {
    ShutdownBatching();
}

L0MemoryCacheAdapter::L0MemoryCacheAdapter(
    std::shared_ptr<::vector::EmbeddingPipeline> embedding,
    std::shared_ptr<RedisConnectionPool> redis_pool,
    std::shared_ptr<IL0SessionBatchMetadataStore> metadata_store,
    std::size_t max_cached_records,
    L0MemoryCacheAdapterOptions options)
    : embedding_(std::move(embedding)),
      redis_pool_(std::move(redis_pool)),
      max_cached_records_(max_cached_records),
      options_(options),
      metadata_store_(std::move(metadata_store)) {}

core::Status L0MemoryCacheAdapter::ConfigureBatching(
    core::ThreadPool& compute_pool,
    core::ThreadPool& completion_pool,
    ::vector::EmbeddingBatchCoordinatorOptions options) {
    if (!embedding_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "L0 embedding pipeline is not initialized");
    }
    options.completion_pool = &completion_pool;
    auto provider = std::make_shared<::vector::EmbeddingPipelineBatchProvider>(embedding_);
    batch_coordinator_ = std::make_shared<::vector::EmbeddingBatchCoordinator>(
        compute_pool, std::move(provider), options,
        core::LoggerAdapter::ForModule("l0-embedding-batch"));
    return core::Status::Ok();
}

core::Status L0MemoryCacheAdapter::StartBatching() {
    if (!batch_coordinator_) {
        return core::Status::Ok();
    }
    return batch_coordinator_->Start();
}

void L0MemoryCacheAdapter::ShutdownBatching() noexcept {
    if (batch_coordinator_) {
        batch_coordinator_->Shutdown();
    }
}

core::Result<std::shared_ptr<L0MemoryCacheAdapter::SessionIndexEntry>> L0MemoryCacheAdapter::ResolveIndex(
    const CacheLookupRequest& req) {
    if (fixed_index_entry_) {
        return fixed_index_entry_;
    }
    if (req.session_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "L0 memory session_id is required");
    }
    if (req.user_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "L0 memory user_id is required");
    }
    if (!redis_pool_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "L0 memory redis pool is not initialized");
    }
    if (!sqlite_pool_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "L0 memory sqlite path is not configured");
    }

    const L0SessionKey key{
        .tenant_id = req.tenant_id.empty() ? "default" : req.tenant_id,
        .user_id = req.user_id,
        .session_id = req.session_id,
    };
    {
        std::shared_lock lock(indices_mutex_);
        auto found = per_session_indices_.find(key);
        if (found != per_session_indices_.end()) {
            if (found->second->released.load(std::memory_order_acquire)) {
                return core::Status::Error(core::ErrorCode::Cancelled, "L0 session has been released");
            }
            return found->second;
        }
    }

    if (auto status = sqlite_pool_->Start(); !status.ok()) {
        return status;
    }
    if (metadata_ready_) {
        if (auto status = metadata_ready_(); !status.ok()) {
            return status;
        }
    }
    // 索引初始化可能访问 SQLite，放在映射锁之外，避免一个新 Session 阻塞全部热路径。
    auto index = std::make_shared<cache_vector::VectorIndexManager>(
        key,
        redis_pool_,
        metadata_store_,
        max_cached_records_);
    auto candidate = std::make_shared<SessionIndexEntry>(std::move(index));

    // 同 Session 并发首次访问允许构造候选对象，但最终只发布一个共享实例。
    std::unique_lock lock(indices_mutex_);
    auto found = per_session_indices_.try_emplace(key, candidate).first;
    return found->second;
}

void L0MemoryCacheAdapter::ReleaseSession(const L0SessionKey& key) {
    if (key.tenant_id.empty() || key.user_id.empty() || key.session_id.empty()) {
        return;
    }
    std::unique_lock lock(indices_mutex_);
    auto found = per_session_indices_.find(key);
    if (found != per_session_indices_.end()) {
        // 先标记关闭再移出 map，保证已捕获 entry 的异步回调不会重新写入 Session。
        found->second->released.store(true, std::memory_order_release);
        per_session_indices_.erase(found);
    }
}

core::Result<CacheLookupResult> L0MemoryCacheAdapter::LookupWithEmbedding(
    const CacheLookupRequest& req,
    const std::vector<float>& query_embedding) {
    if (!embedding_ || (!index_ && !redis_pool_)) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "L0 memory adapter is not initialized");
    }
    if (req.text.empty()) {
        return CacheLookupResult{};
    }

    auto entry = ResolveIndex(req);
    if (!entry.ok()) {
        return entry.status();
    }
    return LookupWithEmbedding(req, entry.value(), query_embedding);
}

core::Result<CacheLookupResult> L0MemoryCacheAdapter::LookupWithEmbedding(
    const CacheLookupRequest& req,
    const std::shared_ptr<SessionIndexEntry>& entry,
    const std::vector<float>& query_embedding) {
    if (entry->released.load(std::memory_order_acquire)) {
        return core::Status::Error(core::ErrorCode::Cancelled, "L0 session has been released");
    }

    const auto candidate_multiplier = std::max<std::size_t>(1, options_.candidate_multiplier);
    const auto max_top_k = std::numeric_limits<std::size_t>::max() / candidate_multiplier;
    const auto candidate_k = options_.top_k > max_top_k
        ? std::numeric_limits<std::size_t>::max()
        : options_.top_k * candidate_multiplier;
    core::Result<std::vector<cache_vector::SearchWithContextResult>> hits = [&] {
        std::lock_guard lock(entry->operation_mutex);
        if (entry->released.load(std::memory_order_acquire)) {
            return core::Result<std::vector<cache_vector::SearchWithContextResult>>(
                core::Status::Error(core::ErrorCode::Cancelled, "L0 session has been released"));
        }
        return entry->index->SearchWithContext(
            query_embedding, candidate_k, options_.neighbors_per_hit);
    }();
    if (!hits.ok()) {
        if (hits.status().code() == core::ErrorCode::NotFound) {
            return CacheLookupResult{};
        }
        return hits.status();
    }

    return BuildL0RecallContext(req, std::move(hits).value(), options_, NowUnixMs());
}


core::Result<CacheLookupResult> BuildL0RecallContext(
    const CacheLookupRequest& req,
    std::vector<cache_vector::SearchWithContextResult> hits,
    const L0MemoryCacheAdapterOptions& options,
    std::int64_t now_ms) {
    std::vector<RankedL0Hit> ranked;
    ranked.reserve(hits.size());
    for (auto& hit : hits) {
        if (!MatchesOwner(req, hit.hit) || IsExpired(hit.hit, now_ms) ||
            IsRecentTurn(req, hit.hit)) {
            continue;
        }
        const float decay = TimeDecay(hit.hit.metadata.created_at_ms, now_ms, options);
        const float decayed_score = hit.score * decay;
        if (decayed_score < options.similarity_floor) {
            continue;
        }
        std::erase_if(hit.context, [&](const storage::CacheRecord& neighbor) {
            return !MatchesOwner(req, neighbor) || IsExpired(neighbor, now_ms) ||
                IsRecentTurn(req, neighbor);
        });
        ranked.push_back(RankedL0Hit{std::move(hit), decayed_score, decay});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.decayed_score > rhs.decayed_score;
    });
    if (ranked.size() > options.top_k) {
        ranked.resize(options.top_k);
    }

    std::ostringstream payload;
    float best_score = 0.0f;
    std::size_t accepted = 0;
    std::size_t payload_bytes = 0;
    std::unordered_set<std::string> included_records;
    for (const auto& ranked_hit : ranked) {
        const auto& hit = ranked_hit.hit;
        if (!included_records.insert(RecentTurnKey(hit.hit)).second) {
            continue;
        }
        if (accepted == 0) {
            best_score = ranked_hit.decayed_score;
        }
        std::ostringstream entry;
        entry << "Score: " << ranked_hit.decayed_score << "\n"
                << "RawScore: " << hit.score << "\n"
                << "TimeDecay: " << ranked_hit.decay << "\n"
                << "Q: " << hit.hit.input << "\n"
                << "A: " << hit.hit.response << "\n";
        for (const auto& neighbor : hit.context) {
            if ((!neighbor.input.empty() || !neighbor.response.empty()) &&
                included_records.insert(RecentTurnKey(neighbor)).second) {
                entry << "Context Q: " << neighbor.input << "\n"
                        << "Context A: " << neighbor.response << "\n";
            }
        }
        const auto entry_text = entry.str();
        if (options.max_payload_bytes == 0 || payload_bytes > options.max_payload_bytes ||
            entry_text.size() > options.max_payload_bytes - payload_bytes) {
            return core::Status::Error(
                core::ErrorCode::ResourceExhausted,
                "L0 memory payload exceeds the configured byte budget");
        }
        payload << entry_text;
        payload_bytes += entry_text.size();
        ++accepted;
    }

    const auto serialized_payload = payload.str();

    CacheLookupResult result;
    result.hit = accepted > 0;
    result.similarity_score = best_score;
    result.payload = serialized_payload;
    result.retrieved_at = std::chrono::system_clock::now();
    return result;
}

core::Result<CacheLookupResult> L0MemoryCacheAdapter::Lookup(const CacheLookupRequest& req) {
    if (!embedding_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "L0 memory adapter is not initialized");
    }
    if (req.text.empty()) {
        return CacheLookupResult{};
    }
    auto query_embedding = embedding_->Encode(req.text);
    if (!query_embedding.ok()) {
        return query_embedding.status();
    }
    return LookupWithEmbedding(req, query_embedding.value());
}

core::Status L0MemoryCacheAdapter::LookupAsync(
    CacheLookupRequest request,
    LookupCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "L0 lookup completion is required");
    }
    if (!batch_coordinator_) {
        try {
            completion(Lookup(request));
        } catch (...) {
            return core::Status::Error(core::ErrorCode::InternalError,
                                       "L0 lookup completion threw an exception");
        }
        return core::Status::Ok();
    }
    if (request.text.empty()) {
        try {
            completion(CacheLookupResult{});
        } catch (...) {
            return core::Status::Error(core::ErrorCode::InternalError,
                                       "L0 lookup completion threw an exception");
        }
        return core::Status::Ok();
    }

    auto self = weak_from_this().lock();
    if (!self) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "asynchronous L0 lookup requires shared adapter ownership");
    }
    auto request_holder = std::make_shared<CacheLookupRequest>(std::move(request));
    auto entry = ResolveIndex(*request_holder);
    if (!entry.ok()) {
        return entry.status();
    }
    return batch_coordinator_->Submit({
        .session_id = request_holder->session_id,
        .tenant_id = request_holder->tenant_id,
        .user_uuid = request_holder->user_id,
        .trace_id = request_holder->extra.contains("trace_id")
            ? request_holder->extra.at("trace_id")
            : std::string{},
        .text = request_holder->text,
        .completion = [self = std::move(self), request_holder, entry = std::move(entry).value(),
                       completion = std::move(completion)](
            core::Result<std::vector<float>> embedding) mutable {
            if (!embedding.ok()) {
                completion(embedding.status());
                return;
            }
            completion(self->LookupWithEmbedding(*request_holder, entry, embedding.value()));
        },
    });
}

core::Status L0MemoryCacheAdapter::Store(const CacheStoreRequest& req) {
    if (!embedding_ || (!index_ && !redis_pool_)) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "L0 memory adapter is not initialized");
    }
    if (req.origin.text.empty() || req.response_payload.empty()) {
        return core::Status::Ok();
    }

    auto text_embedding = embedding_->Encode(req.origin.text);
    if (!text_embedding.ok()) {
        return text_embedding.status();
    }
    return StoreWithEmbedding(req, std::move(text_embedding).value());
}

core::Status L0MemoryCacheAdapter::StoreAsync(
    CacheStoreRequest request,
    StoreCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "L0 store completion is required");
    }
    if (!batch_coordinator_) {
        try {
            completion(Store(request));
        } catch (...) {
            return core::Status::Error(core::ErrorCode::InternalError,
                                       "L0 store completion threw an exception");
        }
        return core::Status::Ok();
    }
    if (request.origin.text.empty() || request.response_payload.empty()) {
        try {
            completion(core::Status::Ok());
        } catch (...) {
            return core::Status::Error(core::ErrorCode::InternalError,
                                       "L0 store completion threw an exception");
        }
        return core::Status::Ok();
    }
    auto self = weak_from_this().lock();
    if (!self) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "asynchronous L0 store requires shared adapter ownership");
    }
    auto request_holder = std::make_shared<CacheStoreRequest>(std::move(request));
    auto entry = ResolveIndex(request_holder->origin);
    if (!entry.ok()) {
        return entry.status();
    }
    return batch_coordinator_->Submit({
        .session_id = request_holder->origin.session_id,
        .tenant_id = request_holder->origin.tenant_id,
        .user_uuid = request_holder->origin.user_id,
        .trace_id = request_holder->origin.extra.contains("trace_id")
            ? request_holder->origin.extra.at("trace_id")
            : std::string{},
        .text = request_holder->origin.text,
        .completion = [self = std::move(self), request_holder, entry = std::move(entry).value(),
                       completion = std::move(completion)](
            core::Result<std::vector<float>> embedding) mutable {
            if (!embedding.ok()) {
                completion(embedding.status());
                return;
            }
            completion(self->StoreWithEmbedding(
                *request_holder, entry, std::move(embedding).value()));
        },
    });
}

core::Status L0MemoryCacheAdapter::StoreWithEmbedding(
    const CacheStoreRequest& req,
    std::vector<float> text_embedding) {
    auto entry = ResolveIndex(req.origin);
    if (!entry.ok()) {
        return entry.status();
    }
    return StoreWithEmbedding(req, entry.value(), std::move(text_embedding));
}

core::Status L0MemoryCacheAdapter::StoreWithEmbedding(
    const CacheStoreRequest& req,
    const std::shared_ptr<SessionIndexEntry>& entry,
    std::vector<float> text_embedding) {
    if (entry->released.load(std::memory_order_acquire)) {
        return core::Status::Error(core::ErrorCode::Cancelled, "L0 session has been released");
    }

    storage::CacheRecord record;
    record.embedding = std::move(text_embedding);
    record.input = req.origin.text;
    record.response = req.response_payload;
    record.metadata.scope = req.origin.scope;
    record.metadata.answer_type = req.answer_type;
    record.metadata.tenant_id = req.origin.tenant_id;
    record.metadata.user_id = req.origin.user_id;
    record.metadata.session_id = req.origin.session_id;
    record.metadata.subject = req.origin.subject;
    record.metadata.grade = req.origin.grade;
    record.metadata.topic = req.origin.topic;
    record.metadata.persona_id = req.origin.persona_id;
    record.metadata.quality_score = req.quality_score;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    record.metadata.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    if (req.ttl) {
        record.metadata.expires_at_ms = record.metadata.created_at_ms +
            std::chrono::duration_cast<std::chrono::milliseconds>(*req.ttl).count();
    }
    record.extra_metadata = req.origin.extra;
    std::lock_guard lock(entry->operation_mutex);
    if (entry->released.load(std::memory_order_acquire)) {
        return core::Status::Error(core::ErrorCode::Cancelled, "L0 session has been released");
    }
    return entry->index->AddRecord(record);
}

}
