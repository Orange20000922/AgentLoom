#pragma once

#include "result.h"
#include "vector_index_manager.h"
#include "../../storage/vector/vector_metadata.h"
#include "../../storage/vector/vector_partition_registry.h"
#include "embedding_pipeline.h"
#include "embedding_batch_coordinator.h"
#include "../../llm/llm_client.h"

#include <memory>
#include <functional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agent::service::persona {

struct ToolMemoryQuery {
    std::string session_id;
    std::string user_uuid;
    std::string persona_id;
    std::string trace_id;
    std::string query;
};

struct ToolMemoryHit {
    std::string tool_id;
    std::string memory_hash;
    std::string instruction;
    std::string schema_json;
    double score = 0.0;
    int priority = 0;
    bool regex_hit = false;
    bool vector_hit = false;
};

struct ToolMemoryContext {
    bool hit = false;
    std::vector<ToolMemoryHit> hits;
    std::string prompt_block;
    std::vector<llm::ChatCompletionRequest::Tool> tools;
};

class IToolMemoryProvider {
public:
    using QueryCompletion = std::function<void(core::Result<ToolMemoryContext>)>;
    virtual ~IToolMemoryProvider() = default;
    /// 查询与本轮文本相关的工具记忆。
    /// @param request session/user/persona scope、trace 和 UTF-8 查询文本。
    /// @return 命中列表及可注入 prompt block；provider 必须执行租户和用户隔离。
    virtual core::Result<ToolMemoryContext> Query(const ToolMemoryQuery& request) = 0;
    /// 异步查询工具记忆；默认兼容实现会同步调用 Query，生产 Vector provider 可使用批处理器。
    virtual core::Status QueryAsync(ToolMemoryQuery request, QueryCompletion completion);
};

// 工具关键词触发器：由工具配置的关键词编译成正则，作为向量召回的确定性兜底通道。
struct ToolKeywordTrigger {
    std::string tool_id;
    std::string instruction;
    std::string schema_json;
    std::vector<std::string> keywords;
    std::vector<std::string> negative_keywords;  // 命中即取消该工具的正则触发（否定排除）
};

struct VectorToolMemoryProviderOptions {
    std::int64_t collection_id = 0;
    std::string tenant_id;
    int top_k = 3;
    double min_score = 0.78;
    bool enable_regex = true;
    bool enable_vector = true;
    std::vector<ToolKeywordTrigger> keyword_triggers;
};

class VectorToolMemoryProvider final : public IToolMemoryProvider {
public:
    VectorToolMemoryProvider(std::shared_ptr<::vector::EmbeddingPipeline> embedding_pipeline,
                             std::shared_ptr<vector::VectorIndexManager> index_manager,
                             std::shared_ptr<vector_storage::PartitionRegistry> partition_registry,
                             VectorToolMemoryProviderOptions options = {},
                             std::shared_ptr<::vector::EmbeddingBatchCoordinator> embedding_batch = nullptr);

    core::Result<ToolMemoryContext> Query(const ToolMemoryQuery& request) override;
    core::Status QueryAsync(ToolMemoryQuery request, QueryCompletion completion) override;

private:
    struct CompiledKeywordTrigger {
        std::string tool_id;
        std::string instruction;
        std::string schema_json;
        std::regex regex;
        std::regex negative_regex;  // 仅当 has_negative 为真时参与判断
        bool has_negative = false;
    };

    static std::string BuildPromptBlock(const std::vector<ToolMemoryHit>& hits);
    std::vector<ToolMemoryHit> RegexHits(std::string_view query) const;
    core::Result<std::vector<ToolMemoryHit>> VectorHits(std::string_view query) const;
    core::Result<std::vector<ToolMemoryHit>> VectorHitsFromEmbedding(
        std::span<const float> embedding) const;
    ToolMemoryContext BuildContext(std::vector<ToolMemoryHit> hits) const;

    std::shared_ptr<::vector::EmbeddingPipeline> embedding_pipeline_;
    std::shared_ptr<vector::VectorIndexManager> index_manager_;
    std::shared_ptr<vector_storage::PartitionRegistry> partition_registry_;
    VectorToolMemoryProviderOptions options_;
    std::shared_ptr<::vector::EmbeddingBatchCoordinator> embedding_batch_;
    std::vector<CompiledKeywordTrigger> keyword_triggers_;
};

} // namespace agent::service::persona
