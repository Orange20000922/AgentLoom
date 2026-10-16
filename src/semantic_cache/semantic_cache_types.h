#pragma once

#include "result.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent::semantic_cache {

enum class CacheScope {
    Global,
    Tenant,
    User,
    Session,
    Persona,
};

enum class AnswerType {
    Generic,
    Knowledge,
    Tutoring,
    Authority,
    Personalized,
};

struct CacheLookupRequest {
    std::string text;
    CacheScope scope = CacheScope::Global;
    AnswerType answer_type = AnswerType::Generic;

    std::string tenant_id;
    std::string user_id;
    std::string session_id;

    std::string subject;
    std::string grade;
    std::string topic;
    std::string persona_id;

    std::vector<std::string> recent_turns;
    // 与 recent_turns 按位置对应；缺失时兼容旧调用方并回退到文本匹配。
    std::vector<std::string> recent_turn_ids;
    bool has_image_reference = false;

    std::unordered_map<std::string, std::string> extra;
};

struct CacheLookupResult {
    bool hit = false;
    std::int64_t entry_id = 0;
    float similarity_score = 0.0f;
    std::string payload;
    std::string source_entry_fingerprint;
    std::chrono::system_clock::time_point retrieved_at{};
};

struct CacheStoreRequest {
    CacheLookupRequest origin;
    std::string response_payload;
    AnswerType answer_type = AnswerType::Generic;
    float quality_score = 1.0f;
    std::optional<std::chrono::seconds> ttl;
};

}  // namespace agent::semantic_cache
