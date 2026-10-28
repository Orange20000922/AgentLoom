#pragma once

#include "vector_index.h"
#include "../storage/vector/vector_repository.h"
#include "../storage/vector/vector_partition_registry.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent::vector {

struct IndexManagerOptions {
    std::size_t max_resident_partitions = 32;
    std::string backend = "exact";  // "exact" | "faiss_flat"
    bool log_hydration = true;
};

struct IndexSearchOptions {
    std::size_t top_k = 5;
    bool include_forgotten = false;
    std::optional<std::string> memory_type;  // post-filter by memory_type
    std::optional<float> min_score;          // post-filter by score threshold
};

/// Manages lazy-loaded, LRU-cached vector indices per partition.
///
/// Each partition's index is hydrated on first Search (or after staleness detection)
/// by loading all vectors from the repository. Resident indices are kept in sync via
/// NotifyInserted/Deleted. When the cache exceeds max_resident_partitions, the least
/// recently used partition is evicted.
///
/// Thread-safe: all public methods lock internally.
class VectorIndexManager {
public:
    VectorIndexManager(std::shared_ptr<vector_storage::IVectorRepository> repo,
                       std::shared_ptr<vector_storage::PartitionRegistry> registry,
                       std::size_t dimension,
                       IndexManagerOptions options);

    /// Search a partition: hydrate if needed, run vector search, load full EntryRecords,
    /// apply post-filters (memory_type, min_score, include_forgotten).
    core::Result<std::vector<vector_storage::EntryRecord>> Search(
        std::int64_t partition_id,
        std::span<const float> query,
        const IndexSearchOptions& options);

    /// After Repository::InsertEntry, call this to keep the resident index in sync.
    /// If the partition is not resident, this is a no-op (next Search will hydrate).
    core::Status NotifyInserted(std::int64_t partition_id,
                                std::int64_t entry_id,
                                std::span<const float> vector);

    /// After Repository::DeleteEntry, call this to keep the resident index in sync.
    core::Status NotifyDeleted(std::int64_t partition_id, std::int64_t entry_id);

    /// Check if a partition is currently resident (for testing/diagnostics).
    bool IsResident(std::int64_t partition_id) const;

    /// Manually evict a partition (for testing/diagnostics).
    void Evict(std::int64_t partition_id);

    /// Current number of resident partitions.
    std::size_t ResidentCount() const;

private:
    struct ResidentIndex {
        std::int64_t partition_id;
        std::unique_ptr<::vector::IVectorIndex> index;
        std::int64_t hydrated_at_ms;
        std::int64_t source_modified_at_ms;  // from PartitionSnapshot at hydration time
        std::list<std::int64_t>::iterator lru_iter;
    };

    core::Result<std::shared_ptr<ResidentIndex>> EnsureResidentLocked(std::int64_t partition_id);
    void TouchLruLocked(ResidentIndex& entry);
    void EvictLruLocked();

    std::shared_ptr<vector_storage::IVectorRepository> repo_;
    std::shared_ptr<vector_storage::PartitionRegistry> registry_;
    std::size_t dimension_;
    IndexManagerOptions options_;

    mutable std::mutex lock_;
    std::list<std::int64_t> lru_;  // MRU at front, LRU at back
    // 查询持有共享租约；LRU 淘汰只移除缓存所有权，不销毁正在检索的索引。
    std::unordered_map<std::int64_t, std::shared_ptr<ResidentIndex>> resident_;
};

}  // namespace agent::vector
