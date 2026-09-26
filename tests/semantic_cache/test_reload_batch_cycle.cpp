#include "../../src/semantic_cache/semantic_cache_pipeline.h"
#include "../../src/semantic_cache/redis_connection_pool.h"
#include "../../src/storage/sqlite/sqlite_connection_pool.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <chrono>
#include <memory>

using namespace agent::semantic_cache;
using namespace storage;
#define MAX_CACHE_RECORDS 1000
class ReloadBatchCycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto unique = std::chrono::system_clock::now().time_since_epoch().count();
        test_db_path_ = (std::filesystem::temp_directory_path() /
            ("test_reload_cycle_" + std::to_string(unique) + ".db")).string();
        std::filesystem::remove(test_db_path_);

        sqlite::SqliteConnectionPoolOptions sqlite_options;
        sqlite_options.path = test_db_path_;
        sqlite_options.read_connection_count = 2;
        sqlite_options.write_connection_count = 1;
        sqlite_options.busy_timeout_ms = 1000;
        sqlite_options.enable_wal = true;
        sqlite_pool_ = std::make_shared<sqlite::SqliteConnectionPool>(std::move(sqlite_options));
        ASSERT_TRUE(sqlite_pool_->Start().ok());

        redis_pool_ = std::make_shared<RedisConnectionPool>(
            RedisPoolOptions{
                .host = "127.0.0.1",
                .port = "5000",
                .pool_size = 2
            });
        auto redis_status = redis_pool_->Start();
        if (!redis_status.ok()) {
            // Start() 内部执行 Redis PING；服务未启动时跳过外部依赖测试。
            GTEST_SKIP() << "Redis PING failed: " << redis_status.message();
        }

        user_uuid_ = "test_user_reload_cycle_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

        CleanupRedisKeys();
    }

    void TearDown() override {
        if (redis_pool_) {
            CleanupRedisKeys();
            redis_pool_->Shutdown();
        }
        if (sqlite_pool_) {
            sqlite_pool_->Close();
            sqlite_pool_.reset();
        }
        std::error_code ec;
        std::filesystem::remove(test_db_path_, ec);
    }

    void CleanupRedisKeys() {
        std::string pattern = "cache:batch:" + user_uuid_ + ":*";
        auto result = redis_pool_->Scan(pattern);
        if (result.ok()) {
            redis_pool_->Del(result.value());
        }
    }

    std::string test_db_path_;
    std::shared_ptr<sqlite::SqliteConnectionPool> sqlite_pool_;
    std::shared_ptr<RedisConnectionPool> redis_pool_;
    std::string user_uuid_;
};

TEST_F(ReloadBatchCycleTest, CyclicReloadDoesNotEmptyIndex) {
    cache_vector::VectorIndexManager manager(user_uuid_, redis_pool_, sqlite_pool_, MAX_CACHE_RECORDS);

    for (int batch = 0; batch < 3; ++batch) {
        for (int i = 0; i < MAX_CACHE_RECORDS; ++i) {
            CacheRecord record;
            record.embedding.resize(agent::semantic_cache::kExpectedEmbeddingDim, 0.1f * batch);
            record.input = "batch_" + std::to_string(batch) + "_record_" + std::to_string(i);
            record.response = "response_" + std::to_string(i);
            auto status = manager.AddRecord(record);
            ASSERT_TRUE(status.ok()) << status.message();
        }
    }

    for (int round = 0; round < 6; ++round) {
        std::vector<float> query(agent::semantic_cache::kExpectedEmbeddingDim, 0.5f);
        auto result = manager.Search(query, 5);
        ASSERT_TRUE(result.ok()) << "Round " << round << " failed: " << result.status().message();
        ASSERT_FALSE(result.value().empty()) << "Round " << round << " returned empty results";
    }

    EXPECT_GT(manager.CurrentSize(), 0);
}

TEST_F(ReloadBatchCycleTest, RebuildFromRedisWhenIndexEmpty) {
    cache_vector::VectorIndexManager manager(user_uuid_, redis_pool_, sqlite_pool_);

    for (int batch = 0; batch < 2; ++batch) {
        for (int i = 0; i < MAX_CACHE_RECORDS; ++i) {
            CacheRecord record;
            record.embedding.resize(agent::semantic_cache::kExpectedEmbeddingDim, 0.2f);
            record.input = "rebuild_test_" + std::to_string(batch) + "_" + std::to_string(i);
            record.response = "resp";
            manager.AddRecord(record);
        }
    }

    std::string new_db = (std::filesystem::temp_directory_path() /
        ("test_rebuild_fresh_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".db")).string();
    std::filesystem::remove(new_db);
    sqlite::SqliteConnectionPoolOptions fresh_options;
    fresh_options.path = new_db;
    fresh_options.read_connection_count = 2;
    fresh_options.write_connection_count = 1;
    fresh_options.busy_timeout_ms = 1000;
    fresh_options.enable_wal = true;
    auto fresh_pool = std::make_shared<sqlite::SqliteConnectionPool>(std::move(fresh_options));
    ASSERT_TRUE(fresh_pool->Start().ok());

    {
        cache_vector::VectorIndexManager fresh_manager(
            user_uuid_, redis_pool_, fresh_pool);

        std::vector<float> query(agent::semantic_cache::kExpectedEmbeddingDim, 0.3f);
        auto result = fresh_manager.Search(query, 3);
        ASSERT_TRUE(result.ok()) << result.status().message();
        ASSERT_FALSE(result.value().empty());
    }
    fresh_pool->Close();

    std::error_code ec;
    std::filesystem::remove(new_db, ec);
}
