#include "../../src/memory/long_term_memory_compressor.h"
#include <gtest/gtest.h>

namespace agent::memory {
// 来源编解码不依赖 Redis 或真实模型，单独纳入 CI。
TEST(LongTermMemoryCompressorCodecTest, RejectsUnstructuredOrOversizedOutput) {
    auto object = ParseL3CompressionFacts(R"({"fact":"not-an-array"})", 4, 64);
    EXPECT_FALSE(object.ok());
    EXPECT_EQ(object.status().code(), core::ErrorCode::InvalidArgument);

    auto non_string = ParseL3CompressionFacts(R"(["ok", 1])", 4, 64);
    EXPECT_FALSE(non_string.ok());
    EXPECT_EQ(non_string.status().code(), core::ErrorCode::InvalidArgument);

    auto too_many = ParseL3CompressionFacts(R"(["a","b","c"])", 2, 64);
    EXPECT_FALSE(too_many.ok());
    EXPECT_EQ(too_many.status().code(), core::ErrorCode::ResourceExhausted);

    auto too_large = ParseL3CompressionFacts(R"(["12345"])", 4, 4);
    EXPECT_FALSE(too_large.ok());
    EXPECT_EQ(too_large.status().code(), core::ErrorCode::InvalidArgument);
}

TEST(LongTermMemoryCompressorCodecTest, PreservesRolesAndRejectsAssistantEvidenceAsUserStatement) {
    storage::CacheRecord record;
    record.input = "I may prefer tea";
    record.response = "You should try coffee";
    record.extra_metadata["turn_id"] = "turn-1";
    auto user = ParseL3FactsWithSources(
        R"([{"text":"possibly likes tea","source_role":"user_statement","uncertainty":0.7,"record_index":0,"evidence":"may prefer tea"}])",
        {record}, 4, 512);
    ASSERT_TRUE(user.ok());
    EXPECT_EQ(user.value()[0].source_record_ids, std::vector<std::string>{"turn-1"});
    EXPECT_DOUBLE_EQ(user.value()[0].uncertainty, 0.7);
    auto invalid = ParseL3FactsWithSources(
        R"([{"text":"likes coffee","source_role":"user_statement","uncertainty":0,"record_index":0,"evidence":"try coffee"}])",
        {record}, 4, 512);
    EXPECT_EQ(invalid.status().code(), core::ErrorCode::DataLoss);
    auto suggestion = ParseL3FactsWithSources(
        R"([{"text":"try coffee","source_role":"model_suggestion","uncertainty":0.5,"record_index":0,"evidence":"try coffee"}])",
        {record}, 4, 512);
    ASSERT_TRUE(suggestion.ok());
    EXPECT_EQ(suggestion.value()[0].source_role, "model_suggestion");
}


}
