#include "long_term_memory_compressor.h"
#include "../core/text_validation.h"
#include <nlohmann/json.hpp>
#include <cmath>

namespace agent::memory {
core::Result<std::vector<L3Fact>> ParseL3FactsWithSources(std::string_view output,
    const std::vector<storage::CacheRecord>& records, std::size_t max_facts, std::size_t max_bytes) {
    try {
        const auto body = nlohmann::json::parse(output);
        if (!body.is_array() || body.size() > max_facts || max_facts == 0 || max_bytes == 0)
            return core::Status::Error(core::ErrorCode::ResourceExhausted, "L3 fact batch exceeds bounds");
        std::vector<L3Fact> result;
        for (const auto& item : body) {
            L3Fact fact;
            fact.text = item.at("text").get<std::string>();
            fact.source_role = item.at("source_role").get<std::string>();
            fact.uncertainty = item.at("uncertainty").get<double>();
            const auto source = item.at("record_index").get<std::size_t>();
            const auto evidence = item.at("evidence").get<std::string>();
            if (fact.text.empty() || fact.text.size() > max_bytes || !core::IsValidUtf8(fact.text) ||
                !std::isfinite(fact.uncertainty) || fact.uncertainty < 0 || fact.uncertainty > 1 ||
                source >= records.size() || evidence.empty() || evidence.size() > 4096 || !core::IsValidUtf8(evidence))
                return core::Status::Error(core::ErrorCode::DataLoss, "invalid L3 fact source or bounds");
            const auto& record = records[source];
            const bool user = fact.source_role == "user_statement";
            const bool model = fact.source_role == "model_suggestion";
            const bool hypothesis = fact.source_role == "hypothesis";
            if ((!user && !model && !hypothesis) ||
                (user && record.input.find(evidence) == std::string::npos) ||
                (model && record.response.find(evidence) == std::string::npos) ||
                (hypothesis && record.input.find(evidence) == std::string::npos && record.response.find(evidence) == std::string::npos))
                return core::Status::Error(core::ErrorCode::DataLoss, "L3 evidence does not match source role");
            const auto id = record.extra_metadata.find("turn_id");
            // 旧 L0 没有 turn_id 时保留真实 Session/时间定位，不伪造用户角色。
            fact.source_record_ids.push_back(id != record.extra_metadata.end() ? id->second :
                record.metadata.session_id + ":" + std::to_string(record.metadata.created_at_ms));
            fact.evidence = evidence;
            result.push_back(std::move(fact));
        }
        return result;
    } catch (const std::exception& error) {
        return core::Status::Error(core::ErrorCode::DataLoss, std::string("invalid sourced L3 facts: ") + error.what());
    }
}
}
