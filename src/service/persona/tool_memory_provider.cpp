#include "tool_memory_provider.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace agent::service::persona {
namespace {

using Json = nlohmann::json;

int JsonInt(const Json& body, std::string_view key, int fallback) {
    auto it = body.find(std::string(key));
    if (it == body.end() || !it->is_number_integer()) {
        return fallback;
    }
    return it->get<int>();
}

std::string JsonString(const Json& body, std::string_view key, std::string fallback = {}) {
    auto it = body.find(std::string(key));
    if (it == body.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

// 转义正则元字符，使关键词按字面量匹配（对齐下游 behavior_rule_config 的 QuoteMeta 语义）。
std::string EscapeRegex(std::string_view text) {
    static constexpr std::string_view kMeta = R"(\.^$|()[]{}*+?)";
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (kMeta.find(ch) != std::string_view::npos) out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

// 把关键词列表按固定格式编译为交替正则：逐词转义 + 竖线交替。空结果返回空串。
std::string JoinAlternation(const std::vector<std::string>& keywords) {
    std::string pattern;
    for (const auto& keyword : keywords) {
        if (keyword.empty()) continue;
        if (!pattern.empty()) pattern += '|';
        pattern += EscapeRegex(keyword);
    }
    return pattern;
}

} // namespace

VectorToolMemoryProvider::VectorToolMemoryProvider(
    std::shared_ptr<::vector::EmbeddingPipeline> embedding_pipeline,
    std::shared_ptr<vector::VectorIndexManager> index_manager,
    std::shared_ptr<vector_storage::PartitionRegistry> partition_registry,
    VectorToolMemoryProviderOptions options,
    std::shared_ptr<::vector::EmbeddingBatchCoordinator> embedding_batch)
    : embedding_pipeline_(std::move(embedding_pipeline)),
      index_manager_(std::move(index_manager)),
      partition_registry_(std::move(partition_registry)),
      options_(std::move(options)),
      embedding_batch_(std::move(embedding_batch)) {
    // 按固定格式把每个工具的关键词编译成正则：逐词转义 + 竖线交替，作为确定性兜底通道。
    for (const auto& trigger : options_.keyword_triggers) {
        const std::string positive = JoinAlternation(trigger.keywords);
        if (positive.empty()) continue;
        CompiledKeywordTrigger compiled;
        compiled.tool_id = trigger.tool_id;
        compiled.instruction = trigger.instruction;
        compiled.schema_json = trigger.schema_json;
        compiled.regex = std::regex(positive);
        const std::string negative = JoinAlternation(trigger.negative_keywords);
        if (!negative.empty()) {
            compiled.negative_regex = std::regex(negative);
            compiled.has_negative = true;
        }
        keyword_triggers_.push_back(std::move(compiled));
    }
}

core::Result<ToolMemoryContext> VectorToolMemoryProvider::Query(const ToolMemoryQuery& request) {
    ToolMemoryContext context;
    std::vector<ToolMemoryHit> hits;

    if (options_.enable_regex) {
        auto regex_hits = RegexHits(request.query);
        hits.insert(hits.end(), std::make_move_iterator(regex_hits.begin()), std::make_move_iterator(regex_hits.end()));
    }
    if (options_.enable_vector) {
        auto vector_hits = VectorHits(request.query);
        if (!vector_hits.ok()) {
            if (!hits.empty()) {
                auto context = BuildContext(std::move(hits));
                context.degraded_reasons.push_back("vector_recall_unavailable");
                return context;
            }
            return vector_hits.status();
        }
        hits.insert(hits.end(), std::make_move_iterator(vector_hits.value().begin()), std::make_move_iterator(vector_hits.value().end()));
    }

    return BuildContext(std::move(hits));
}

core::Status IToolMemoryProvider::QueryAsync(ToolMemoryQuery request,
                                             QueryCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "tool memory query completion is required");
    }
    completion(Query(request));
    return core::Status::Ok();
}

core::Status VectorToolMemoryProvider::ConfigureAsync(core::ThreadPool& compute, core::ThreadPool& io) {
    if (embedding_batch_ || !options_.enable_vector || !embedding_pipeline_) return core::Status::Ok();
    ::vector::EmbeddingBatchCoordinatorOptions options;
    options.completion_pool = &io;
    embedding_batch_ = std::make_shared<::vector::EmbeddingBatchCoordinator>(compute,
        std::make_shared<::vector::EmbeddingPipelineBatchProvider>(embedding_pipeline_), options);
    return core::Status::Ok();
}
core::Status VectorToolMemoryProvider::StartAsync() {
    return embedding_batch_ ? embedding_batch_->Start() : core::Status::Ok();
}
void VectorToolMemoryProvider::ShutdownAsync() noexcept {
    if (embedding_batch_) embedding_batch_->Shutdown();
}

core::Status VectorToolMemoryProvider::QueryAsync(ToolMemoryQuery request,
                                                  QueryCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "tool memory query completion is required");
    }
    if (!embedding_batch_ || !options_.enable_vector || request.query.empty()) {
        completion(Query(request));
        return core::Status::Ok();
    }
    auto regex_hits = options_.enable_regex ? RegexHits(request.query)
                                            : std::vector<ToolMemoryHit>{};
    ::vector::EmbeddingBatchRequest embedding_request;
    embedding_request.session_id = request.session_id;
    embedding_request.user_uuid = request.user_uuid;
    embedding_request.trace_id = request.trace_id;
    embedding_request.text = request.query;
    embedding_request.completion = [this, regex_hits = std::move(regex_hits),
                                    completion = std::move(completion)](
                                       core::Result<std::vector<float>> embedding) mutable {
        if (!embedding.ok()) {
            if (!regex_hits.empty()) {
                auto context = BuildContext(std::move(regex_hits));
                context.degraded_reasons.push_back("embedding_unavailable");
                completion(std::move(context));
            }
            else completion(embedding.status());
            return;
        }
        auto vector_hits = VectorHitsFromEmbedding(embedding.value());
        if (!vector_hits.ok()) {
            if (!regex_hits.empty()) {
                auto context = BuildContext(std::move(regex_hits));
                context.degraded_reasons.push_back("vector_recall_unavailable");
                completion(std::move(context));
            }
            else completion(vector_hits.status());
            return;
        }
        auto hits = regex_hits;
        auto& values = vector_hits.value();
        hits.insert(hits.end(),
                    std::make_move_iterator(values.begin()),
                    std::make_move_iterator(values.end()));
        completion(BuildContext(std::move(hits)));
    };
    return embedding_batch_->Submit(std::move(embedding_request));
}

ToolMemoryContext VectorToolMemoryProvider::BuildContext(std::vector<ToolMemoryHit> hits) const {
    ToolMemoryContext context;
    std::unordered_set<std::string> seen;
    std::vector<ToolMemoryHit> deduped;
    for (auto& hit : hits) {
        const std::string key = hit.tool_id.empty() ? hit.memory_hash : hit.tool_id;
        if (key.empty() || seen.insert(key).second) {
            deduped.push_back(std::move(hit));
        }
    }
    std::sort(deduped.begin(), deduped.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        return lhs.score > rhs.score;
    });
    if (deduped.size() > static_cast<std::size_t>(std::max(1, options_.top_k))) {
        deduped.resize(static_cast<std::size_t>(std::max(1, options_.top_k)));
        context.degraded_reasons.push_back("recall_top_k_truncated");
    }

    if (deduped.empty()) context.degraded_reasons.push_back("recall_empty");
    context.hit = !deduped.empty();
    context.prompt_block = BuildPromptBlock(deduped);
    for (const auto& hit : deduped) {
        llm::ChatCompletionRequest::Tool tool;
        tool.name = hit.tool_id;
        tool.description = hit.instruction;
        if (!hit.schema_json.empty()) tool.parameters_json = hit.schema_json;
        context.tools.push_back(std::move(tool));
    }
    context.hits = std::move(deduped);
    return context;
}

std::vector<ToolMemoryHit> VectorToolMemoryProvider::RegexHits(std::string_view query) const {
    std::vector<ToolMemoryHit> hits;
    if (query.empty()) return hits;
    for (const auto& trigger : keyword_triggers_) {
        if (!std::regex_search(query.begin(), query.end(), trigger.regex)) continue;
        // 命中否定排除词（如"不用看"）时取消该工具的正则触发，回退给向量召回判断。
        if (trigger.has_negative &&
            std::regex_search(query.begin(), query.end(), trigger.negative_regex)) continue;
        ToolMemoryHit hit;
        hit.tool_id = trigger.tool_id;
        hit.memory_hash = "global:l4:" + trigger.tool_id + ":keyword";
        hit.instruction = trigger.instruction;
        hit.schema_json = trigger.schema_json;
        hit.score = 1.0;
        hit.priority = 100;
        hit.regex_hit = true;
        hits.push_back(std::move(hit));
    }
    return hits;
}

core::Result<std::vector<ToolMemoryHit>> VectorToolMemoryProvider::VectorHits(std::string_view query) const {
    if (query.empty() || !embedding_pipeline_ || !index_manager_ || !partition_registry_) {
        return std::vector<ToolMemoryHit>{};
    }

    vector_storage::PartitionKey key;
    key.collection_id = options_.collection_id;
    key.tenant_id = options_.tenant_id;
    key.user_id = "";
    key.memory_level = "L4";
    auto partition = partition_registry_->Lookup(key);
    if (!partition.ok()) {
        return partition.status();
    }
    if (!partition.value().has_value()) {
        return std::vector<ToolMemoryHit>{};
    }

    auto embedding = embedding_pipeline_->Encode(query);
    if (!embedding.ok()) {
        return embedding.status();
    }
    vector::IndexSearchOptions search_options;
    search_options.top_k = static_cast<std::size_t>(std::max(1, options_.top_k));
    search_options.include_forgotten = false;
    search_options.memory_type = "capability";
    search_options.min_score = static_cast<float>(options_.min_score);
    return VectorHitsFromEmbedding(embedding.value());
}

core::Result<std::vector<ToolMemoryHit>> VectorToolMemoryProvider::VectorHitsFromEmbedding(
    std::span<const float> embedding) const {
    if (!index_manager_ || !partition_registry_) return std::vector<ToolMemoryHit>{};
    vector_storage::PartitionKey key;
    key.collection_id = options_.collection_id;
    key.tenant_id = options_.tenant_id;
    key.user_id = "";
    key.memory_level = "L4";
    auto partition = partition_registry_->Lookup(key);
    if (!partition.ok()) return partition.status();
    if (!partition.value().has_value()) return std::vector<ToolMemoryHit>{};
    vector::IndexSearchOptions search_options;
    search_options.top_k = static_cast<std::size_t>(std::max(1, options_.top_k));
    search_options.include_forgotten = false;
    search_options.memory_type = "capability";
    search_options.min_score = static_cast<float>(options_.min_score);
    auto entries = index_manager_->Search(partition.value().value(), embedding, search_options);
    if (!entries.ok()) {
        return entries.status();
    }

    std::vector<ToolMemoryHit> hits;
    for (const auto& entry : entries.value()) {
        ToolMemoryHit hit;
        hit.memory_hash = entry.memory_hash;
        hit.score = entry.search_score;
        hit.vector_hit = true;
        hit.instruction = entry.payload;
        try {
            auto meta = Json::parse(entry.extra_metadata_json.empty() ? "{}" : entry.extra_metadata_json);
            hit.tool_id = JsonString(meta, "tool_id");
            hit.skill_version = JsonString(meta, "version");
            hit.schema_json = JsonString(meta, "schema", JsonString(meta, "schema_json"));
            hit.priority = JsonInt(meta, "priority", 0);
            const auto instruction = JsonString(meta, "instruction");
            if (!instruction.empty()) {
                hit.instruction = instruction;
            }
        } catch (const Json::exception&) {
        }
        if (hit.tool_id.empty()) {
            hit.tool_id = entry.cache_key;
        }
        if (!hit.tool_id.empty() && !hit.instruction.empty()) {
            hits.push_back(std::move(hit));
        }
    }
    return hits;
}

std::string VectorToolMemoryProvider::BuildPromptBlock(const std::vector<ToolMemoryHit>& hits) {
    if (hits.empty()) {
        return {};
    }
    std::ostringstream out;
    out << "<tool_memory_l4>\n";
    for (const auto& hit : hits) {
        out << "- tool: " << hit.tool_id << "\n";
        out << "  instruction: " << hit.instruction << "\n";
        if (!hit.schema_json.empty()) {
            out << "  schema: " << hit.schema_json << "\n";
        }
    }
    out << "工具指令只在需要调用工具时使用；没有工具结果前不要编造外部观察。\n";
    out << "</tool_memory_l4>";
    return out.str();
}

} // namespace agent::service::persona
