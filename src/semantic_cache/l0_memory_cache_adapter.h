#pragma once

#include "isemantic_cache.h"
#include "semantic_cache_pipeline.h"

#include "embedding_pipeline.h"
#include "embedding_batch_coordinator.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <functional>

namespace agent::semantic_cache {

class RedisConnectionPool;

struct L0MemoryCacheAdapterOptions {
    std::size_t top_k = 5;
    std::size_t candidate_multiplier = 4;
    std::size_t neighbors_per_hit = 1;
    float similarity_floor = 0.78f;
    std::int64_t warm_window_seconds = 3600;
    std::int64_t half_life_seconds = 172800;
    std::int64_t max_age_seconds = 604800;
    // Adapter 内部防止单次 L0 payload 造成无界内存/传输占用，单位为字节。
    std::size_t max_payload_bytes = 64 * 1024;
};

// 无 I/O 的召回装配：复用实际 adapter 的隔离、去重和预算规则，便于确定性验证。
core::Result<CacheLookupResult> BuildL0RecallContext(
    const CacheLookupRequest& request,
    std::vector<cache_vector::SearchWithContextResult> hits,
    const L0MemoryCacheAdapterOptions& options,
    std::int64_t now_ms);

/// L0 short-context memory adapter.
///
/// This class intentionally implements ISemanticCache only as an integration
/// adapter for SemanticMemoryContextProvider. It is not the answer semantic
/// cache: Store() admits conversation turns into L0, while Lookup() retrieves
/// related short-term dialogue context through cache_vector::VectorIndexManager.
class L0MemoryCacheAdapter final : public ISemanticCache,
                                   public IAsyncSemanticCache,
                                   public std::enable_shared_from_this<L0MemoryCacheAdapter> {
public:
    L0MemoryCacheAdapter(std::shared_ptr<::vector::EmbeddingPipeline> embedding,
                         std::shared_ptr<cache_vector::VectorIndexManager> index,
                         L0MemoryCacheAdapterOptions options = {});
    L0MemoryCacheAdapter(std::shared_ptr<::vector::EmbeddingPipeline> embedding,
                         std::shared_ptr<RedisConnectionPool> redis_pool,
                         std::string sqlite_path,
                         std::size_t max_cached_records,
                         L0MemoryCacheAdapterOptions options = {});
    L0MemoryCacheAdapter(
        std::shared_ptr<::vector::EmbeddingPipeline> embedding,
        std::shared_ptr<RedisConnectionPool> redis_pool,
        std::shared_ptr<IL0SessionBatchMetadataStore> metadata_store,
        std::size_t max_cached_records,
        L0MemoryCacheAdapterOptions options = {});

    template <L0SessionMetadataProvider Provider>
    L0MemoryCacheAdapter(
        std::shared_ptr<::vector::EmbeddingPipeline> embedding,
        std::shared_ptr<RedisConnectionPool> redis_pool,
        std::shared_ptr<Provider> metadata_store,
        std::size_t max_cached_records,
        L0MemoryCacheAdapterOptions options = {})
        : embedding_(std::move(embedding)),
          redis_pool_(std::move(redis_pool)),
          max_cached_records_(max_cached_records),
          options_(options),
          metadata_store_(metadata_store),
          metadata_ready_([metadata_store = std::move(metadata_store)] {
              return EnsureL0MetadataReady(*metadata_store);
          }) {}
    ~L0MemoryCacheAdapter() override;

    core::Result<CacheLookupResult> Lookup(const CacheLookupRequest& req) override;
    core::Status LookupAsync(CacheLookupRequest request,
                             LookupCompletion completion) override;
    core::Status StoreAsync(CacheStoreRequest request,
                            StoreCompletion completion) override;
    core::Status Store(const CacheStoreRequest& req) override;
    void ReleaseSession(const L0SessionKey& key);

    /// 在 Gateway 线程池启动前装配；StartBatching/ShutdownBatching 由生命周期负责。
    core::Status ConfigureBatching(
        core::ThreadPool& compute_pool,
        core::ThreadPool& completion_pool,
        ::vector::EmbeddingBatchCoordinatorOptions options = {});
    core::Status StartBatching();
    void ShutdownBatching() noexcept;

private:
    struct SessionIndexEntry {
        explicit SessionIndexEntry(std::shared_ptr<cache_vector::VectorIndexManager> value)
            : index(std::move(value)) {}

        std::shared_ptr<cache_vector::VectorIndexManager> index;
        std::atomic<bool> released{false};
        // VectorIndexManager 仍包含批次游标等可变状态；同一 Session 暂时严格保序。
        std::mutex operation_mutex;
    };

    core::Result<std::shared_ptr<SessionIndexEntry>> ResolveIndex(
        const CacheLookupRequest& req);
    core::Result<CacheLookupResult> LookupWithEmbedding(
        const CacheLookupRequest& req,
        const std::vector<float>& query_embedding);
    core::Result<CacheLookupResult> LookupWithEmbedding(
        const CacheLookupRequest& req,
        const std::shared_ptr<SessionIndexEntry>& entry,
        const std::vector<float>& query_embedding);
    core::Status StoreWithEmbedding(
        const CacheStoreRequest& req,
        std::vector<float> text_embedding);
    core::Status StoreWithEmbedding(
        const CacheStoreRequest& req,
        const std::shared_ptr<SessionIndexEntry>& entry,
        std::vector<float> text_embedding);

    std::shared_ptr<::vector::EmbeddingPipeline> embedding_;
    std::shared_ptr<cache_vector::VectorIndexManager> index_;
    std::shared_ptr<RedisConnectionPool> redis_pool_;
    std::shared_ptr<storage::sqlite::SqliteConnectionPool> sqlite_pool_;
    std::shared_ptr<IL0SessionBatchMetadataStore> metadata_store_;
    std::function<core::Status()> metadata_ready_;
    std::size_t max_cached_records_ = 1000;
    std::shared_ptr<SessionIndexEntry> fixed_index_entry_;
    std::unordered_map<L0SessionKey, std::shared_ptr<SessionIndexEntry>, L0SessionKeyHash>
        per_session_indices_;
    L0MemoryCacheAdapterOptions options_;
    std::shared_ptr<::vector::EmbeddingBatchCoordinator> batch_coordinator_;
    // 仅保护 Session 到索引的映射，严禁在持锁期间执行 Embedding、Redis 或 SQLite I/O。
    mutable std::shared_mutex indices_mutex_;
};

}
