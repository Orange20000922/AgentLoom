#include "../../src/semantic_cache/l0_memory_cache_adapter.h"

#include <gtest/gtest.h>
#include <chrono>

namespace {
using namespace agent::semantic_cache;

storage::CacheRecord Record(std::string id, std::string text) {
    storage::CacheRecord record;
    record.input = std::move(text);
    record.response = "response";
    record.extra_metadata["turn_id"] = std::move(id);
    record.metadata.tenant_id = "tenant";
    record.metadata.user_id = "user";
    record.metadata.session_id = "session";
    return record;
}

TEST(L0RecallContextTest, ExcludesRecentIdsAndDeduplicatesOverlappingNeighbors) {
    CacheLookupRequest request;
    request.tenant_id = "tenant";
    request.user_id = "user";
    request.session_id = "session";
    request.recent_turn_ids = {"session:recent"};
    auto recent = Record("session:recent", "recent-hidden");
    auto first = Record("session:1", "first");
    auto second = Record("session:2", "second");
    auto neighbor = Record("session:3", "shared-neighbor");
    auto foreign = Record("foreign:1", "foreign-hidden");
    foreign.metadata.session_id = "foreign";
    auto result = BuildL0RecallContext(request,
        {{recent, 1.0f, {}}, {first, 0.99f, {neighbor, recent, foreign}},
         {second, 0.98f, {neighbor}}, {neighbor, 0.97f, {}}}, {}, 1000);
    ASSERT_TRUE(result.ok()) << result.status().message();
    const auto& payload = result.value().payload;
    EXPECT_EQ(payload.find("recent-hidden"), std::string::npos);
    EXPECT_EQ(payload.find("foreign-hidden"), std::string::npos);
    auto position = payload.find("shared-neighbor");
    ASSERT_NE(position, std::string::npos);
    EXPECT_EQ(payload.find("shared-neighbor", position + 1), std::string::npos);
    EXPECT_NE(payload.find("first"), std::string::npos);
    EXPECT_NE(payload.find("second"), std::string::npos);
}

TEST(L0RecallContextTest, FallsBackToCompleteTurnTextForLegacyHistory) {
    CacheLookupRequest request;
    request.recent_turns = {"用户: legacy\n助手: response"};
    auto result = BuildL0RecallContext(request,
        {{Record("new-record-id", "legacy"), 1.0f, {}}}, {}, 1000);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().hit);
    EXPECT_TRUE(result.value().payload.empty());
}

TEST(L0RecallContextTest, RejectsOversizedRecallWithoutReturningPartialKnowledge) {
    auto record = Record("session:1", std::string(1024, 'x'));
    L0MemoryCacheAdapterOptions options;
    options.max_payload_bytes = 128;
    auto result = BuildL0RecallContext({}, {{record, 1.0f, {}}}, options, 1000);
    EXPECT_EQ(result.status().code(), core::ErrorCode::ResourceExhausted);
}

TEST(L0RecallContextTest, BoundsPayloadUnderRepeatedOverlappingRecall) {
    std::vector<cache_vector::SearchWithContextResult> hits;
    for (std::size_t i = 0; i < 64; ++i) {
        cache_vector::SearchWithContextResult hit;
        hit.hit = Record("session:" + std::to_string(i), "question-" + std::to_string(i));
        hit.score = 1.0f;
        for (std::size_t j = 1; j <= 12; ++j) {
            const auto neighbor = (i + j) % 64;
            hit.context.push_back(Record("session:" + std::to_string(neighbor),
                                         "question-" + std::to_string(neighbor)));
        }
        hits.push_back(std::move(hit));
    }
    L0MemoryCacheAdapterOptions options;
    options.top_k = 64;
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 200; ++iteration) {
        auto result = BuildL0RecallContext({}, hits, options, 1000);
        ASSERT_TRUE(result.ok());
        const auto& payload = result.value().payload;
        EXPECT_LE(payload.size(), options.max_payload_bytes);
        std::size_t count = 0;
        for (auto offset = payload.find("Q: "); offset != std::string::npos;
             offset = payload.find("Q: ", offset + 3)) ++count;
        EXPECT_EQ(count, 64u);
    }
    // 记录合成装配压力的耗时；不把本地微基准当成生产吞吐/尾延迟验收。
    RecordProperty("iterations", 200);
    RecordProperty("elapsed_us", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()));
}
}
