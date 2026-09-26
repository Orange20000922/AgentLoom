#include "l0_batch_metadata_store.h"
#include "semantic_cache_pipeline.h"
#include "redis_connection_pool.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <array>
#include <vector>

namespace {

using agent::semantic_cache::L0ActiveBatchState;
using agent::semantic_cache::L0SessionKey;
using agent::semantic_cache::L0SessionKeyHash;
using agent::semantic_cache::SqliteL0SessionBatchMetadataStore;
using agent::semantic_cache::BuildL0BatchKey;
using agent::semantic_cache::BuildL0BatchScanPattern;
using agent::semantic_cache::BuildL0OwnerBatchScanPattern;
using storage::sqlite::SqliteConnectionPool;
using storage::sqlite::SqliteConnectionPoolOptions;

std::filesystem::path TestPath() {
    const auto path = std::filesystem::temp_directory_path() / "agent_l0_session_metadata_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return path;
}

TEST(L0BatchMetadataStoreTest, KeepsActiveAndTimestampMetadataSessionScoped) {
    const auto path = TestPath();
    auto pool = std::make_shared<SqliteConnectionPool>(SqliteConnectionPoolOptions{
        .path = path.string(),
        .read_connection_count = 2,
        .write_connection_count = 1,
        .busy_timeout_ms = 1000,
        .enable_wal = true,
    });
    ASSERT_TRUE(pool->Start().ok());

    SqliteL0SessionBatchMetadataStore store(pool);
    ASSERT_TRUE(store.EnsureSchema().ok());
    const L0SessionKey first{.tenant_id = "tenant-a", .user_id = "user-a", .session_id = "session-a"};
    const L0SessionKey second{.tenant_id = "tenant-a", .user_id = "user-a", .session_id = "session-b"};

    ASSERT_TRUE(store.SaveActiveBatch(first, {.timestamp = 11, .count = 3}).ok());
    ASSERT_TRUE(store.SaveActiveBatch(second, {.timestamp = 22, .count = 5}).ok());
    ASSERT_TRUE(store.ReplaceTimestampIndex(first, std::array<std::int64_t, 2>{1, 2}).ok());
    ASSERT_TRUE(store.ReplaceTimestampIndex(second, std::array<std::int64_t, 1>{9}).ok());

    auto first_active = store.LoadActiveBatch(first);
    ASSERT_TRUE(first_active.ok());
    ASSERT_TRUE(first_active.value().has_value());
    EXPECT_EQ(first_active.value()->timestamp, 11);
    EXPECT_EQ(first_active.value()->count, 3u);

    auto second_active = store.LoadActiveBatch(second);
    ASSERT_TRUE(second_active.ok());
    ASSERT_TRUE(second_active.value().has_value());
    EXPECT_EQ(second_active.value()->timestamp, 22);

    auto first_index = store.LoadTimestampIndex(first);
    ASSERT_TRUE(first_index.ok());
    EXPECT_EQ(first_index.value(), (std::vector<std::int64_t>{1, 2}));
    auto second_index = store.LoadTimestampIndex(second);
    ASSERT_TRUE(second_index.ok());
    EXPECT_EQ(second_index.value(), (std::vector<std::int64_t>{9}));

    ASSERT_TRUE(store.ClearActiveBatch(first).ok());
    auto cleared = store.LoadActiveBatch(first);
    ASSERT_TRUE(cleared.ok());
    EXPECT_FALSE(cleared.value().has_value());
    EXPECT_TRUE(store.LoadActiveBatch(second).value().has_value());
    pool->Close();
}

TEST(L0BatchKeyTest, UsesCompleteOwnerAndUnixMillisecondSuffix) {
    const L0SessionKey key{
        .tenant_id = "tenant-a",
        .user_id = "user-a",
        .session_id = "session-a",
    };

    EXPECT_EQ(BuildL0BatchKey(key, 1'800'000'000'123),
              "cache:v2:batch:tenant-a:user-a:session-a:1800000000123");
    EXPECT_EQ(BuildL0BatchScanPattern(key),
              "cache:v2:batch:tenant-a:user-a:session-a:*");
    EXPECT_EQ(BuildL0OwnerBatchScanPattern("tenant-a", "user-a"),
              "cache:v2:batch:tenant-a:user-a:*");
    EXPECT_NE(L0SessionKeyHash{}(key),
              L0SessionKeyHash{}(L0SessionKey{"tenant-b", "user-a", "session-a"}));
}

TEST(L0BatchMetadataStoreTest, RedisV2BatchesRemainIsolatedAcrossSessions) {
    const auto path = TestPath();
    auto sqlite_pool = std::make_shared<SqliteConnectionPool>(SqliteConnectionPoolOptions{
        .path = path.string(),
        .read_connection_count = 2,
        .write_connection_count = 1,
        .busy_timeout_ms = 1000,
        .enable_wal = true,
    });
    ASSERT_TRUE(sqlite_pool->Start().ok());
    auto metadata = std::make_shared<SqliteL0SessionBatchMetadataStore>(sqlite_pool);
    ASSERT_TRUE(metadata->EnsureSchema().ok());

    auto redis = std::make_shared<agent::semantic_cache::RedisConnectionPool>(
        agent::semantic_cache::RedisPoolOptions{
            .host = "127.0.0.1",
            .port = "5000",
            .pool_size = 2,
        });
    const auto redis_status = redis->Start();
    if (!redis_status.ok()) {
        // Start() 内部执行 Redis PING；服务未启动时跳过外部依赖测试。
        GTEST_SKIP() << "Redis PING failed: " << redis_status.message();
    }

    const std::string tenant = "l0-v2-test-tenant";
    const std::string user = "l0-v2-test-user";
    const std::string prefix = "cache:v2:batch:" + tenant + ":" + user + ":";
    auto old_keys = redis->Scan(prefix + "*");
    if (old_keys.ok() && !old_keys.value().empty()) {
        ASSERT_TRUE(redis->Del(old_keys.value()).ok());
    }

    agent::semantic_cache::cache_vector::VectorIndexManager first(
        {.tenant_id = tenant, .user_id = user, .session_id = "session-a"},
        redis,
        metadata,
        100);
    agent::semantic_cache::cache_vector::VectorIndexManager second(
        {.tenant_id = tenant, .user_id = user, .session_id = "session-b"},
        redis,
        metadata,
        100);
    agent::semantic_cache::cache_vector::VectorIndexManager other_tenant(
        {.tenant_id = "l0-v2-other-tenant", .user_id = user, .session_id = "session-a"},
        redis,
        metadata,
        100);

    storage::CacheRecord first_record;
    first_record.embedding.assign(agent::semantic_cache::kExpectedEmbeddingDim, 0.1f);
    first_record.input = "session-a-input";
    first_record.response = "session-a-response";
    first_record.metadata.tenant_id = tenant;
    first_record.metadata.user_id = user;
    first_record.metadata.session_id = "session-a";
    storage::CacheRecord second_record = first_record;
    second_record.input = "session-b-input";
    second_record.response = "session-b-response";
    second_record.metadata.session_id = "session-b";
    storage::CacheRecord other_tenant_record = first_record;
    other_tenant_record.input = "other-tenant-input";
    other_tenant_record.response = "other-tenant-response";
    other_tenant_record.metadata.tenant_id = "l0-v2-other-tenant";

    ASSERT_TRUE(first.AddRecord(first_record).ok());
    ASSERT_TRUE(second.AddRecord(second_record).ok());
    ASSERT_TRUE(other_tenant.AddRecord(other_tenant_record).ok());
    const std::vector<float> query(agent::semantic_cache::kExpectedEmbeddingDim, 0.1f);

    auto first_result = first.Search(query, 1);
    ASSERT_TRUE(first_result.ok()) << first_result.status().message();
    ASSERT_EQ(first_result.value().size(), 1u);
    EXPECT_EQ(first_result.value()[0].metadata.session_id, "session-a");

    auto second_result = second.Search(query, 1);
    ASSERT_TRUE(second_result.ok()) << second_result.status().message();
    ASSERT_EQ(second_result.value().size(), 1u);
    EXPECT_EQ(second_result.value()[0].metadata.session_id, "session-b");

    auto other_tenant_result = other_tenant.Search(query, 1);
    ASSERT_TRUE(other_tenant_result.ok()) << other_tenant_result.status().message();
    ASSERT_EQ(other_tenant_result.value().size(), 1u);
    EXPECT_EQ(other_tenant_result.value()[0].metadata.tenant_id, "l0-v2-other-tenant");

    {
        agent::semantic_cache::cache_vector::VectorIndexManager restarted(
            {.tenant_id = tenant, .user_id = user, .session_id = "session-a"},
            redis,
            metadata,
            100);
        auto restored = restarted.Search(query, 1);
        ASSERT_TRUE(restored.ok()) << restored.status().message();
        ASSERT_EQ(restored.value().size(), 1u);
        EXPECT_EQ(restored.value()[0].metadata.session_id, "session-a");
    }

    auto keys = redis->Scan(prefix + "*");
    if (keys.ok() && !keys.value().empty()) {
        ASSERT_TRUE(redis->Del(keys.value()).ok());
    }
    auto other_keys = redis->Scan("cache:v2:batch:l0-v2-other-tenant:" + user + ":*");
    if (other_keys.ok() && !other_keys.value().empty()) {
        ASSERT_TRUE(redis->Del(other_keys.value()).ok());
    }
    redis->Shutdown();
    sqlite_pool->Close();
}

}
