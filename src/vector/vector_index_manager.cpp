#include "vector_index_manager.h"
#include "exact_vector_index.h"
#include "faiss_vector_index.h"

#include <chrono>
#include <algorithm>

namespace agent::vector {

namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

core::Result<std::unique_ptr<::vector::IVectorIndex>> CreateIndex(
    const std::string& backend, std::size_t dimension) {
    ::vector::VectorIndexOptions opts;
    opts.dimension = dimension;
    if (backend == "faiss_flat") {
        auto r = ::vector::FaissVectorIndex::Create(opts);
        if (!r) return r.status();
        return std::unique_ptr<::vector::IVectorIndex>(std::move(r).value());
    }
    auto r = ::vector::ExactVectorIndex::Create(opts);
    if (!r) return r.status();
    return std::unique_ptr<::vector::IVectorIndex>(std::move(r).value());
}

}  // namespace

VectorIndexManager::VectorIndexManager(
    std::shared_ptr<vector_storage::IVectorRepository> repo,
    std::shared_ptr<vector_storage::PartitionRegistry> registry,
    std::size_t dimension,
    IndexManagerOptions options)
    : repo_(std::move(repo)),
      registry_(std::move(registry)),
      dimension_(dimension),
      options_(std::move(options)) {}

core::Result<std::vector<vector_storage::EntryRecord>> VectorIndexManager::Search(
    std::int64_t partition_id,
    std::span<const float> query,
    const IndexSearchOptions& options) {

    if (query.size() != dimension_) {
        return core::Status(core::ErrorCode::InvalidArgument, "Query dimension mismatch");
    }

    if (options.top_k == 0 || options_.max_resident_partitions == 0) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "top_k and resident partition capacity must be positive");
    }
    std::shared_ptr<ResidentIndex> resident;
    {
        std::lock_guard lock(lock_);
        auto r = EnsureResidentLocked(partition_id);
        if (!r) return r.status();
        resident = std::move(r).value();
        TouchLruLocked(*resident);
    }

    // Run vector search (outside lock — index is thread-safe)
    std::size_t overfetch = options.top_k * 2;  // overfetch for post-filter
    auto search_r = resident->index->Search(query, overfetch);
    if (!search_r) return search_r.status();
    auto results = std::move(search_r).value();

    if (results.empty()) {
        return std::vector<vector_storage::EntryRecord>{};
    }

    // Load full EntryRecords
    std::vector<std::int64_t> ids;
    ids.reserve(results.size());
    for (auto& r : results) {
        ids.push_back(r.id);
    }
    auto entries_r = repo_->LookupEntries(ids);
    if (!entries_r) return entries_r.status();
    auto entries = std::move(entries_r).value();

    // Build id→entry map for filtering
    std::unordered_map<std::int64_t, vector_storage::EntryRecord> entry_map;
    for (auto& e : entries) {
        entry_map[e.entry_id] = std::move(e);
    }

    // Post-filter and collect
    std::vector<vector_storage::EntryRecord> out;
    out.reserve(options.top_k);
    for (auto& r : results) {
        auto it = entry_map.find(r.id);
        if (it == entry_map.end()) continue;  // deleted between search and load
        auto& entry = it->second;

        if (!options.include_forgotten && entry.forgotten) continue;
        if (options.memory_type && entry.memory_type != *options.memory_type) continue;
        if (options.min_score && r.score < *options.min_score) continue;

        entry.search_score = r.score;
        out.push_back(std::move(entry));
        if (out.size() >= options.top_k) break;
    }

    return out;
}

core::Status VectorIndexManager::NotifyInserted(std::int64_t partition_id,
                                                std::int64_t entry_id,
                                                std::span<const float> vector) {
    std::lock_guard lock(lock_);
    auto it = resident_.find(partition_id);
    if (it == resident_.end()) {
        return core::Status::Ok();  // not resident, next Search will hydrate
    }
    return it->second->index->Add(vector, std::span<const std::int64_t>{&entry_id, 1});
}

core::Status VectorIndexManager::NotifyDeleted(std::int64_t partition_id, std::int64_t entry_id) {
    std::lock_guard lock(lock_);
    auto it = resident_.find(partition_id);
    if (it == resident_.end()) {
        return core::Status::Ok();
    }
    return it->second->index->Remove(std::span<const std::int64_t>{&entry_id, 1});
}

bool VectorIndexManager::IsResident(std::int64_t partition_id) const {
    std::lock_guard lock(lock_);
    return resident_.contains(partition_id);
}

void VectorIndexManager::Evict(std::int64_t partition_id) {
    std::lock_guard lock(lock_);
    auto it = resident_.find(partition_id);
    if (it == resident_.end()) return;
    lru_.erase(it->second->lru_iter);
    resident_.erase(it);
}

std::size_t VectorIndexManager::ResidentCount() const {
    std::lock_guard lock(lock_);
    return resident_.size();
}

core::Result<std::shared_ptr<VectorIndexManager::ResidentIndex>> VectorIndexManager::EnsureResidentLocked(
    std::int64_t partition_id) {

    auto it = resident_.find(partition_id);
    if (it != resident_.end()) {
        // Check staleness
        auto snapshot_r = repo_->GetPartitionSnapshot(partition_id);
        if (!snapshot_r) return snapshot_r.status();
        auto snapshot = std::move(snapshot_r).value();

        if (it->second->source_modified_at_ms >= snapshot.last_modified_at_ms) {
            return it->second;  // still fresh
        }
        // Stale — evict and rebuild
        lru_.erase(it->second->lru_iter);
        resident_.erase(it);
    }

    // Hydrate from repository
    auto entries_r = repo_->ListEntries(partition_id, false);  // exclude forgotten
    if (!entries_r) return entries_r.status();
    auto entries = std::move(entries_r).value();

    auto index_r = CreateIndex(options_.backend, dimension_);
    if (!index_r) return index_r.status();
    auto index = std::move(index_r).value();
    if (!entries.empty()) {
        std::vector<float> vectors;
        std::vector<std::int64_t> ids;
        vectors.reserve(entries.size() * dimension_);
        ids.reserve(entries.size());
        for (auto& e : entries) {
            if (e.vector.size() != dimension_) {
                return core::Status(core::ErrorCode::InvalidArgument,
                    "Entry vector dimension mismatch");
            }
            vectors.insert(vectors.end(), e.vector.begin(), e.vector.end());
            ids.push_back(e.entry_id);
        }
        auto add_r = index->Add(vectors, ids);
        if (!add_r.ok()) return add_r;
    }

    auto snapshot_r = repo_->GetPartitionSnapshot(partition_id);
    if (!snapshot_r) return snapshot_r.status();
    auto snapshot = std::move(snapshot_r).value();

    ResidentIndex resident;
    resident.partition_id = partition_id;
    resident.index = std::move(index);
    resident.hydrated_at_ms = NowMs();
    resident.source_modified_at_ms = snapshot.last_modified_at_ms;

    // Evict LRU if needed
    while (resident_.size() >= options_.max_resident_partitions) {
        EvictLruLocked();
    }

    // Insert into LRU (front = MRU)
    lru_.push_front(partition_id);
    resident.lru_iter = lru_.begin();
    auto [inserted_it, _] = resident_.emplace(
        partition_id, std::make_shared<ResidentIndex>(std::move(resident)));
    return inserted_it->second;
}

void VectorIndexManager::TouchLruLocked(ResidentIndex& entry) {
    lru_.erase(entry.lru_iter);
    lru_.push_front(entry.partition_id);
    entry.lru_iter = lru_.begin();
}

void VectorIndexManager::EvictLruLocked() {
    if (lru_.empty()) return;
    auto victim_id = lru_.back();
    lru_.pop_back();
    resident_.erase(victim_id);
}

}  // namespace agent::vector
