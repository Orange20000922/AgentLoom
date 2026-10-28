#include "vector_index_manager.h"
#include "../../src/storage/vector/sqlite_vector_repository.h"
#include "../../src/storage/vector/vector_partition_registry.h"
#include "../../src/storage/sqlite/sqlite_connection.h"
#include "../../src/storage/sqlite/sqlite_connection_pool.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <random>
#include <thread>
#include <atomic>

using namespace agent;
using namespace agent::vector;
using namespace agent::vector_storage;

namespace {

std::filesystem::path MakeTempDbPath(const std::string& name) {
    auto tmp = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return tmp;
}

std::vector<float> RandomVector(std::size_t dim, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(dim);
    for (auto& x : v) x = dist(rng);
    return v;
}

}  // namespace

class VectorIndexManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = MakeTempDbPath("agent_index_manager_test.db");

        // Pre-create DB file with WAL enabled (pool's read connections use READONLY)
        {
            auto conn_r = storage::sqlite::SqliteConnection::Open(db_path_.string());
            ASSERT_TRUE(conn_r.ok());
            auto conn = std::move(conn_r).value();
            ASSERT_TRUE(conn.EnableWal().ok());
        }

        storage::sqlite::SqliteConnectionPoolOptions pool_opts;
        pool_opts.path = db_path_.string();
        pool_opts.read_connection_count = 2;
        pool_opts.write_connection_count = 1;
        pool_opts.busy_timeout_ms = 1000;
        pool_opts.enable_wal = true;
        pool_ = std::make_shared<storage::sqlite::SqliteConnectionPool>(pool_opts);
        ASSERT_TRUE(pool_->Start().ok());

        repo_ = std::make_shared<SqliteVectorRepository>(pool_);
        ASSERT_TRUE(repo_->EnsureSchema().ok());

        registry_ = std::make_shared<PartitionRegistry>(repo_);

        // Create test collection
        CollectionDescriptor desc;
        desc.name = "test_collection";
        desc.embedding_model_fingerprint = "e1";
        desc.tokenizer_fingerprint = "t1";
        desc.pooling_strategy = "mean";
        desc.normalization = "l2";
        desc.dimension = kDim;
        desc.corpus_version = "v1";
        desc.policy_version = "v1";
        auto coll_r = repo_->EnsureCollection(desc);
        ASSERT_TRUE(coll_r.ok());
        collection_id_ = std::move(coll_r).value();
    }

    void TearDown() override {
        registry_.reset();
        repo_.reset();
        if (pool_) pool_->Close();
        pool_.reset();
        std::error_code ec;
        std::filesystem::remove(db_path_, ec);
    }

    std::int64_t CreatePartition(const std::string& level) {
        PartitionKey key;
        key.collection_id = collection_id_;
        key.tenant_id = "";
        key.user_id = "test_user";
        key.memory_level = level;
        auto r = registry_->Resolve(key);
        EXPECT_TRUE(r.ok());
        return std::move(r).value();
    }

    std::int64_t InsertEntry(std::int64_t partition_id, const std::vector<float>& vec, const std::string& hash,
                             std::int64_t created_at_ms = 1000) {
        EntryRecord entry;
        entry.partition_id = partition_id;
        entry.cache_key = hash;
        entry.text_hash = hash;
        entry.content_hash = hash;
        entry.memory_hash = hash;
        entry.vector = vec;
        entry.memory_type = "test";
        entry.payload = "{}";
        entry.created_at_ms = created_at_ms;
        auto r = repo_->InsertEntry(std::move(entry));
        EXPECT_TRUE(r.ok());
        return std::move(r).value();
    }

    static constexpr std::size_t kDim = 8;
    std::filesystem::path db_path_;
    std::shared_ptr<storage::sqlite::SqliteConnectionPool> pool_;
    std::shared_ptr<SqliteVectorRepository> repo_;
    std::shared_ptr<PartitionRegistry> registry_;
    std::int64_t collection_id_ = 0;
};

TEST_F(VectorIndexManagerTest, HydrateOnFirstSearch) {
    auto partition_id = CreatePartition("L1");
    std::mt19937 rng(42);
    auto v1 = RandomVector(kDim, rng);
    auto v2 = RandomVector(kDim, rng);
    InsertEntry(partition_id, v1, "h1");
    InsertEntry(partition_id, v2, "h2");

    IndexManagerOptions opts;
    opts.max_resident_partitions = 2;
    opts.backend = "exact";
    VectorIndexManager mgr(repo_, registry_, kDim, opts);

    EXPECT_FALSE(mgr.IsResident(partition_id));

    IndexSearchOptions search_opts;
    search_opts.top_k = 2;
    auto results_r = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(results_r.ok());
    auto results = std::move(results_r).value();

    EXPECT_TRUE(mgr.IsResident(partition_id));
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].memory_hash, "h1");  // v1 should be closest to itself
}

TEST_F(VectorIndexManagerTest, NotifyInsertedSyncsResidentIndex) {
    auto partition_id = CreatePartition("L1");
    std::mt19937 rng(42);
    auto v1 = RandomVector(kDim, rng);
    InsertEntry(partition_id, v1, "h1");

    IndexManagerOptions opts;
    opts.backend = "exact";
    VectorIndexManager mgr(repo_, registry_, kDim, opts);

    // Hydrate
    IndexSearchOptions search_opts;
    search_opts.top_k = 5;
    auto r1 = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.value().size(), 1u);

    // Insert new entry and notify
    auto v2 = RandomVector(kDim, rng);
    auto entry_id = InsertEntry(partition_id, v2, "h2");
    ASSERT_TRUE(mgr.NotifyInserted(partition_id, entry_id, v2).ok());

    // Search again — should find both without re-hydration
    auto r2 = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().size(), 2u);
}

TEST_F(VectorIndexManagerTest, NotifyDeletedSyncsResidentIndex) {
    auto partition_id = CreatePartition("L1");
    std::mt19937 rng(42);
    auto v1 = RandomVector(kDim, rng);
    auto v2 = RandomVector(kDim, rng);
    auto id1 = InsertEntry(partition_id, v1, "h1");
    InsertEntry(partition_id, v2, "h2");

    IndexManagerOptions opts;
    opts.backend = "exact";
    VectorIndexManager mgr(repo_, registry_, kDim, opts);

    IndexSearchOptions search_opts;
    search_opts.top_k = 5;
    auto r1 = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.value().size(), 2u);

    // Delete entry and notify
    ASSERT_TRUE(repo_->DeleteEntry(id1).ok());
    ASSERT_TRUE(mgr.NotifyDeleted(partition_id, id1).ok());

    auto r2 = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().size(), 1u);
    EXPECT_EQ(r2.value()[0].memory_hash, "h2");
}

TEST_F(VectorIndexManagerTest, LruEviction) {
    IndexManagerOptions opts;
    opts.max_resident_partitions = 2;
    opts.backend = "exact";
    VectorIndexManager mgr(repo_, registry_, kDim, opts);

    auto p1 = CreatePartition("L1");
    auto p2 = CreatePartition("L2");
    auto p3 = CreatePartition("L3");

    std::mt19937 rng(42);
    auto v = RandomVector(kDim, rng);
    InsertEntry(p1, v, "h1");
    InsertEntry(p2, v, "h2");
    InsertEntry(p3, v, "h3");

    IndexSearchOptions search_opts;
    search_opts.top_k = 1;

    // Hydrate p1, p2
    ASSERT_TRUE(mgr.Search(p1, v, search_opts).ok());
    ASSERT_TRUE(mgr.Search(p2, v, search_opts).ok());
    EXPECT_EQ(mgr.ResidentCount(), 2u);

    // Hydrate p3 → should evict p1 (LRU)
    ASSERT_TRUE(mgr.Search(p3, v, search_opts).ok());
    EXPECT_EQ(mgr.ResidentCount(), 2u);
    EXPECT_FALSE(mgr.IsResident(p1));
    EXPECT_TRUE(mgr.IsResident(p2));
    EXPECT_TRUE(mgr.IsResident(p3));
}

TEST_F(VectorIndexManagerTest, StaleIndexRebuilds) {
    auto partition_id = CreatePartition("L1");
    std::mt19937 rng(42);
    auto v1 = RandomVector(kDim, rng);
    InsertEntry(partition_id, v1, "h1");

    IndexManagerOptions opts;
    opts.backend = "exact";
    VectorIndexManager mgr(repo_, registry_, kDim, opts);

    IndexSearchOptions search_opts;
    search_opts.top_k = 5;
    auto r1 = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.value().size(), 1u);

    // Insert directly via repo (bypassing NotifyInserted) with newer created_at_ms
    // so that partition.last_modified_at_ms moves forward and staleness is detected.
    auto v2 = RandomVector(kDim, rng);
    InsertEntry(partition_id, v2, "h2", 2000);
    registry_->NotifyModified(partition_id, 2000);

    // Next search should detect staleness and rebuild
    auto r2 = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().size(), 2u);
}

TEST_F(VectorIndexManagerTest, PostFilterByMemoryType) {
    auto partition_id = CreatePartition("L1");
    std::mt19937 rng(42);
    auto v1 = RandomVector(kDim, rng);
    auto v2 = RandomVector(kDim, rng);

    EntryRecord e1;
    e1.partition_id = partition_id;
    e1.cache_key = "h1";
    e1.text_hash = "h1";
    e1.content_hash = "h1";
    e1.memory_hash = "h1";
    e1.vector = v1;
    e1.memory_type = "fact";
    e1.payload = "{}";
    e1.created_at_ms = 1000;
    ASSERT_TRUE(repo_->InsertEntry(std::move(e1)).ok());

    EntryRecord e2;
    e2.partition_id = partition_id;
    e2.cache_key = "h2";
    e2.text_hash = "h2";
    e2.content_hash = "h2";
    e2.memory_hash = "h2";
    e2.vector = v2;
    e2.memory_type = "state";
    e2.payload = "{}";
    e2.created_at_ms = 1000;
    ASSERT_TRUE(repo_->InsertEntry(std::move(e2)).ok());

    IndexManagerOptions opts;
    opts.backend = "exact";
    VectorIndexManager mgr(repo_, registry_, kDim, opts);

    IndexSearchOptions search_opts;
    search_opts.top_k = 5;
    search_opts.memory_type = "fact";

    auto results_r = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(results_r.ok());
    auto results = std::move(results_r).value();
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].memory_type, "fact");
}

TEST_F(VectorIndexManagerTest, PostFilterExcludesForgotten) {
    auto partition_id = CreatePartition("L1");
    std::mt19937 rng(42);
    auto v1 = RandomVector(kDim, rng);
    auto v2 = RandomVector(kDim, rng);
    auto id1 = InsertEntry(partition_id, v1, "h1");
    InsertEntry(partition_id, v2, "h2");

    // Mark id1 as forgotten
    ASSERT_TRUE(repo_->MarkForgotten(std::span<const std::int64_t>{&id1, 1}, 2000, 3000).ok());

    IndexManagerOptions opts;
    opts.backend = "exact";
    VectorIndexManager mgr(repo_, registry_, kDim, opts);

    IndexSearchOptions search_opts;
    search_opts.top_k = 5;
    search_opts.include_forgotten = false;

    auto results_r = mgr.Search(partition_id, v1, search_opts);
    ASSERT_TRUE(results_r.ok());
    auto results = std::move(results_r).value();
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].memory_hash, "h2");
}

TEST_F(VectorIndexManagerTest, ConcurrentSearchAndEvictionRetainSearchOwnership) {
    auto partition = CreatePartition("L3");
    const std::vector<float> query(kDim, 0.25f);
    InsertEntry(partition, query, "stable");
    IndexManagerOptions options;
    options.backend = "exact";
    options.max_resident_partitions = 1;
    VectorIndexManager manager(repo_, registry_, kDim, options);
    std::atomic<int> failures{0};
    std::jthread search([&] {
        for (int i = 0; i < 300; ++i) {
            auto result = manager.Search(partition, query, {});
            if (!result.ok() || result.value().size() != 1) ++failures;
        }
    });
    for (int i = 0; i < 300; ++i) manager.Evict(partition);
    search.join();
    EXPECT_EQ(failures, 0);
}
