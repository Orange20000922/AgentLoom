#pragma once

#include "result.h"
#include "llm_client.h"
#include "isemantic_cache.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent::document {

class IDocumentEmbeddingProvider {
public:
    virtual ~IDocumentEmbeddingProvider() = default;
    /// @param text 待编码的 UTF-8 文本，仅在调用期间借用。
    /// @return 维度稳定且可用于当前索引的归一化向量。
    virtual core::Result<std::vector<float>> EmbedText(std::string_view text) = 0;
};

struct DocumentBlock {
    std::string id;
    std::string text;
    std::string source;
    std::uint64_t order = 0;
    std::string kind = "paragraph";
    std::optional<int> page;
    std::optional<int> slide;
    std::optional<int> paragraph_index;
    std::optional<int> heading_level;
    std::string numbering;
    std::string style;
    std::optional<double> font_size;
    bool bold = false;
    std::optional<int> bullet_level;
    double confidence = 0.5;
    std::map<std::string, std::string> metadata;
};

struct DocumentExtractOptions {
    std::size_t max_zip_entries = 4096;
    std::size_t max_xml_entry_bytes = 16 * 1024 * 1024;
    std::size_t max_archive_entry_bytes = 64 * 1024 * 1024;
    std::size_t max_total_uncompressed_bytes = 64 * 1024 * 1024;
    double max_compression_ratio = 100.0;
    bool reject_macros = true;
    bool reject_embedded_packages = true;
    bool reject_active_content = true;
};

struct DocumentAnalysisOptions {
    DocumentExtractOptions extract;
    std::size_t max_chunk_slices = 5;
    bool enable_embedding_clustering = true;
    double chunk_similarity_threshold = 0.72;
    bool enable_llm_chunk_fallback = false;
    std::string chunk_llm_model;
    int chunk_llm_max_tokens = 700;
};

struct ChunkSlice {
    std::string title;
    std::string summary;
    std::string text;
    std::string kind = "paragraph";
    double confidence = 0.5;
};

struct ChunkTrunk {
    std::string chunk_id;
    std::vector<std::string> block_ids;
    std::string text;
    std::string title;
    std::string summary;
    std::vector<ChunkSlice> slices;
    std::uint64_t order = 0;
    std::optional<int> page;
    std::optional<int> slide;
    double confidence = 0.5;
    std::string source = "local";
    bool reused = false;
    std::map<std::string, std::string> metadata;
};

struct DocumentChunkBuildMetrics {
    std::size_t group_count = 0;
    std::size_t embedding_request_count = 0;
    std::uint64_t embedding_ms = 0;
    std::size_t llm_cache_lookup_count = 0;
    std::size_t llm_cache_hit_count = 0;
    std::uint64_t llm_cache_lookup_ms = 0;
    std::size_t llm_cache_store_count = 0;
    std::uint64_t llm_cache_store_ms = 0;
    std::size_t semantic_cache_lookup_count = 0;
    std::size_t semantic_cache_hit_count = 0;
    std::uint64_t semantic_cache_lookup_ms = 0;
    std::size_t semantic_cache_store_count = 0;
    std::uint64_t semantic_cache_store_ms = 0;
    std::size_t llm_direct_count = 0;
    std::uint64_t llm_direct_ms = 0;
    std::vector<std::uint64_t> embedding_sample_ms;
    std::vector<std::size_t> embedding_sample_bytes;
};

struct DocumentLlmChunkCacheKey {
    std::string prompt_version;
    std::string model;
    std::string text_hash;
};

class IDocumentLlmChunkCache {
public:
    virtual ~IDocumentLlmChunkCache() = default;
    /// 使用 prompt/model/text 指纹查询结构化 chunk。
    virtual core::Result<ChunkTrunk> Lookup(const DocumentLlmChunkCacheKey& key) = 0;
    /// 保存成功生成的 chunk；实现负责 TTL、容量和损坏 payload 处理。
    virtual core::Status Store(const DocumentLlmChunkCacheKey& key, const ChunkTrunk& chunk) = 0;
};

std::string CleanText(std::string_view text);
std::optional<int> SafeInt(std::string_view text);
std::optional<int> DetectNumberingLevel(std::string_view text);
std::optional<int> HeadingLevelFromStyle(std::string_view style);
std::optional<int> InferHeadingLevel(const DocumentBlock& block);
std::string DocumentBlockId(std::string_view prefix, std::uint64_t order);
std::vector<ChunkTrunk> BuildLocalChunks(const std::vector<DocumentBlock>& blocks,
                                         const DocumentAnalysisOptions& options = {},
                                         std::shared_ptr<llm::ILlmClient> llm_client = {},
                                         std::shared_ptr<IDocumentEmbeddingProvider> embedding_provider = {},
                                         std::shared_ptr<IDocumentLlmChunkCache> llm_chunk_cache = {},
                                         std::shared_ptr<semantic_cache::ISemanticCache> semantic_cache = {},
                                         DocumentChunkBuildMetrics* metrics = nullptr);
nlohmann::json BuildMindmap(const std::vector<DocumentBlock>& blocks,
                            std::string_view file_name,
                            const std::vector<ChunkTrunk>& chunks = {},
                            std::shared_ptr<IDocumentEmbeddingProvider> embedding_provider = {});
nlohmann::json BuildDiagnosis(const std::vector<DocumentBlock>& blocks,
                              const nlohmann::json& mindmap,
                              const std::vector<ChunkTrunk>& chunks = {},
                              const nlohmann::json& knowledge_coverage = {});
/// 解析并分析本地 OOXML 文档。
/// @param path UTF-8 语义的本地文件路径；调用方负责授权和根目录约束。
/// @param file_name 对外展示名称；为空时从 path 推导。
/// @param options ZIP 安全限制、chunk 和可选 LLM 策略。
/// @return mindmap、diagnosis、chunks 和元数据组成的 JSON。
core::Result<nlohmann::json> AnalyzeDocument(const std::filesystem::path& path,
                                             std::string_view file_name = {},
                                             const DocumentAnalysisOptions& options = {},
                                             std::shared_ptr<llm::ILlmClient> llm_client = {},
                                             std::shared_ptr<IDocumentEmbeddingProvider> embedding_provider = {},
                                             std::shared_ptr<IDocumentLlmChunkCache> llm_chunk_cache = {},
                                             std::shared_ptr<semantic_cache::ISemanticCache> semantic_cache = {});

} // namespace agent::document
