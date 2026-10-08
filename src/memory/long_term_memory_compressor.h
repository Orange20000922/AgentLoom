#pragma once

#include "../llm/llm_client.h"
#include "../semantic_cache/redis_connection_pool.h"
#include "../semantic_cache/semantic_cache_pipeline.h"
#include "../storage/vector/vector_repository.h"
#include "../storage/vector/vector_partition_registry.h"
#include "../storage/sqlite/sqlite_connection_pool.h"
#include "../vector/vector_index_manager.h"
#include "../vector/embedding_pipeline.h"
#include "../core/result.h"
#include "../core/logger_adapter.h"
#include <string>
#include <vector>
#include <memory>
#include <future>

namespace agent::memory {

enum class CompressorMode {
    Production,  // Requires llm_client + embedding_pipeline
    Testing      // Allows nullptr llm_client, skips LLM/embedding calls
};

struct MemoryOwner {
    std::string tenant_id;
    std::string user_id;

    bool operator==(const MemoryOwner&) const = default;
};

class IL0RecordSource {
public:
    virtual ~IL0RecordSource() = default;

    /// 按完整 owner 和本地业务日期读取 L0；实现不得把 Redis key 文本当作记录事实。
    virtual core::Result<std::vector<storage::CacheRecord>> ListDailyRecords(
        const MemoryOwner& owner,
        std::string_view date,
        std::size_t limit) = 0;
};

class RedisL0RecordSource final : public IL0RecordSource {
public:
    explicit RedisL0RecordSource(
        std::shared_ptr<semantic_cache::RedisConnectionPool> redis_pool,
        core::LoggerAdapter logger = core::LoggerAdapter::ForModule("memory"));

    core::Result<std::vector<storage::CacheRecord>> ListDailyRecords(
        const MemoryOwner& owner,
        std::string_view date,
        std::size_t limit) override;

private:
    std::shared_ptr<semantic_cache::RedisConnectionPool> redis_pool_;
    core::LoggerAdapter logger_;
};

struct LongTermMemoryRecord {
    std::string tenant_id;
    std::string user_uuid;
    std::string date;           // YYYY-MM-DD
    std::string summary;        // LLM compressed summary (bullet-point list)
    int64_t timestamp;          // Unix timestamp (seconds)
    int source_record_count;    // Number of L0 records compressed
};

struct LongTermMemoryCompressorOptions {
    /// Redis connection pool for fetching L0 records.
    std::shared_ptr<semantic_cache::RedisConnectionPool> redis_pool;

    /// 可替换的 L0 读取边界；为空时由 redis_pool 构造 v2 Redis source。
    std::shared_ptr<IL0RecordSource> l0_record_source;

    /// LLM client for compression (OpenAI-compatible).
    /// Can be nullptr in Testing mode.
    std::shared_ptr<llm::ILlmClient> llm_client;

    /// Vector repository (SQLite-backed) for L3 fact storage.
    std::shared_ptr<vector_storage::IVectorRepository> vector_repo;

    /// Partition registry for resolving user+level to partition_id.
    std::shared_ptr<vector_storage::PartitionRegistry> partition_registry;

    /// Optional registry pool used to persist the set of observed L3 user UUIDs.
    /// If omitted, the compressor will not maintain a persistent user registry.
    std::shared_ptr<storage::sqlite::SqliteConnectionPool> registry_pool;

    /// Vector index manager for search and insert notification.
    std::shared_ptr<vector::VectorIndexManager> index_manager;

    /// Embedding pipeline (tokenizer + ONNX model) for vectorizing facts.
    /// Can be nullptr in Testing mode.
    std::shared_ptr<::vector::EmbeddingPipeline> embedding_pipeline;

    /// Collection ID in vector_collections (must be created beforehand).
    int64_t collection_id = 0;

    /// Compressor mode: Production requires llm_client + embedding, Testing allows nullptr.
    CompressorMode mode = CompressorMode::Production;

    /// Maximum number of L0 records to compress in one batch.
    int max_records_per_batch = 1000;

    /// LLM temperature for compression.
    float compression_temperature = 0.1f;

    /// LLM max tokens for compression output.
    int compression_max_tokens = 800;

    /// 严格限制单批 L3 事实数量与单条 UTF-8 字节数。
    std::size_t max_facts_per_batch = 64;
    std::size_t max_fact_bytes = 512;

    /// L3 召回最低向量分数，避免无关最近邻稳定注入 Prompt。
    float min_relevance_score = 0.50f;
};

core::Result<std::vector<std::string>> ParseL3CompressionFacts(
    std::string_view output,
    std::size_t max_facts,
    std::size_t max_fact_bytes);

/// Long-term memory compressor (L3 layer).
///
/// Compresses daily L0 cache records into structured fact vectors using LLM,
/// then stores them in the unified vector repository for semantic retrieval.
///
/// Architecture:
///   1. Fetch all L0 records for a given user and date from Redis
///   2. Call LLM to extract structured facts
///   3. Embed each fact via EmbeddingPipeline
///   4. Store as EntryRecord in IVectorRepository (memory_level="L3")
///
/// Thread-safety: no internal mutable state beyond logger; all storage access
/// goes through thread-safe components (vector repo, redis pool, index manager).
class LongTermMemoryCompressor final {
public:
    static core::Result<std::unique_ptr<LongTermMemoryCompressor>> Create(
        LongTermMemoryCompressorOptions options);

    ~LongTermMemoryCompressor();

    /// Compress all L0 records for a given user and date into L3 fact entries.
    /// Returns the number of L0 records compressed.
    core::Result<int> CompressDailyMemory(
        const MemoryOwner& owner,
        const std::string& date  // YYYY-MM-DD
    );

    /// Async version: dispatched onto a background thread.
    std::future<core::Result<int>> CompressDailyMemoryAsync(
        MemoryOwner owner,
        const std::string& date
    );

    /// Retrieve L3 summary for a given user and date.
    /// Reconstructs from stored fact entries.
    core::Result<LongTermMemoryRecord> GetDailySummary(
        const MemoryOwner& owner,
        const std::string& date
    );

    /// Retrieve recent L3 summaries for a given user (ordered by date desc).
    core::Result<std::vector<LongTermMemoryRecord>> GetUserSummaries(
        const MemoryOwner& owner,
        int limit = 30
    );

    /// Store a pre-built summary record (for testing / migration).
    /// In production, use CompressDailyMemory instead.
    core::Status StoreSummary(const LongTermMemoryRecord& record);

    /// Semantic search over L3 facts for a user.
    core::Result<std::vector<vector_storage::EntryRecord>> SearchFacts(
        const MemoryOwner& owner,
        const std::string& query,
        int top_k = 5
    );

    /// Return all tenant/user owners registered through this compressor.
    /// This is the source of truth for maintenance tasks.
    core::Result<std::vector<MemoryOwner>> GetRegisteredOwners();

    /// 幂等登记 tenant/user owner，供 L0 admission 和维护任务共享。
    core::Status RegisterOwner(const MemoryOwner& owner) const;

private:
    explicit LongTermMemoryCompressor(LongTermMemoryCompressorOptions options);

    core::Result<int64_t> EnsureUserPartition(const MemoryOwner& owner);
    core::Status EnsureRegistrySchema() const;

    /// Fetch all L0 records for a given user and date from Redis.
    core::Result<std::vector<storage::CacheRecord>> FetchDailyRecords(
        const MemoryOwner& owner,
        const std::string& date
    );

    /// Call LLM to compress records into fact list.
    core::Result<std::vector<std::string>> CompressToFacts(
        const std::vector<storage::CacheRecord>& records
    );

    /// Embed and store a list of facts into the vector repository.
    core::Result<int> StoreFacts(
        const MemoryOwner& owner,
        const std::string& date,
        const std::vector<std::string>& facts,
        int source_record_count
    );

    core::LoggerAdapter logger_;
    LongTermMemoryCompressorOptions options_;
};

}
