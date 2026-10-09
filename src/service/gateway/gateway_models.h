#pragma once

#include "persona_runtime.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent::service::gateway {

struct GatewayError {
    std::string code;
    std::string message;
};

struct GatewayEnvelopeBase {
    bool ok = true;
    std::string trace_id;
    std::string session_id;
    std::chrono::milliseconds latency{0};
    std::optional<GatewayError> error;
};

struct CreateSessionGatewayRequest {
    std::string trace_id;
    std::string session_id;
    std::string user_uuid = "local_user";
    std::string tenant_id = "default";
    std::string classroom_id;
    std::string persona_id;
    persona::PersonalityConfig personality;
    std::optional<persona::EmotionPromptConfig> emotion_prompt_config;
    persona::EmotionStateConfig emotion_state_config;
    std::vector<std::string> context_ids;
    std::vector<std::string> context_patterns;
    std::string proactive_level = "off";
    bool default_persona = false;
};

struct PersonaMetadataRecord {
    std::string tenant_id = "default";
    std::string user_uuid;
    std::string persona_id;
    persona::PersonalityConfig personality;
    std::optional<persona::EmotionPromptConfig> emotion_prompt_config;
    persona::EmotionStateConfig emotion_state_config;
};

struct PersonaMetadataGatewayRequest {
    std::string trace_id;
    std::string tenant_id = "default";
    std::string user_uuid;
    std::string persona_id;
    persona::PersonalityConfig personality;
    std::optional<persona::EmotionPromptConfig> emotion_prompt_config;
    persona::EmotionStateConfig emotion_state_config;
};

struct PersonaMetadataGatewayResponse : GatewayEnvelopeBase {
    PersonaMetadataRecord persona;
};

struct SessionGatewayResponse : GatewayEnvelopeBase {
    persona::SessionSnapshot session;
};

struct CloseSessionGatewayRequest {
    std::string trace_id;
    std::string session_id;
    std::string authenticated_user_uuid;
    std::string reason = "client_close";
};

struct ChatGatewayRequest {
    std::string trace_id;
    std::string session_id;
    std::string authenticated_user_uuid;
    std::string persona_id;
    std::string mode = "chat";
    std::string message;
    std::string model;
    bool stream = false;
    llm::LlmEventSink event_sink;
};

struct ChatGatewayResponse : GatewayEnvelopeBase {
    std::string persona_id;
    std::string content;
    persona::EmotionAnalysis user_emotion;
    persona::EmotionAnalysis ai_emotion;
    bool l0_hit = false;
    bool l3_hit = false;
    std::uint64_t turn_index = 0;
    persona::AnswerCacheInfo answer_cache;
    persona::ChatLatencyBreakdown pipeline_latency;
};

struct ClassroomMessageGatewayRequest {
    std::string trace_id;
    std::string session_id;
    std::string authenticated_user_uuid;
    std::string classroom_id;
    std::string target_persona_id;
    std::string context_id;
    std::string message;
    bool broadcast = false;
    std::string model;
};

struct ClassroomProactiveGatewayRequest {
    std::string trace_id;
    std::string session_id;
    std::string authenticated_user_uuid;
    std::string classroom_id;
    std::string persona_id;
    std::string context_id;
    std::string model;
};

struct ClassroomPollGatewayRequest {
    std::string trace_id;
    std::string authenticated_user_uuid;
    std::string classroom_id;
    std::string persona_id;
    std::string context_id;
    bool system_event = false;
    std::string system_event_content;
    std::string model;
};

struct ClassroomGatewayResponse : GatewayEnvelopeBase {
    std::string classroom_id;
    std::string speaker_persona_id;
    std::string content;
    bool should_speak = true;
    persona::EmotionAnalysis user_emotion;
    persona::EmotionAnalysis ai_emotion;
    std::uint64_t turn_index = 0;
};

struct TrainingReportGatewayRequest {
    std::string trace_id;
    std::string session_id;
    std::string authenticated_user_uuid;
    bool include_raw_turns = true;
};

struct TrainingReportGatewayResponse : GatewayEnvelopeBase {
    std::string generated_at;
    std::uint64_t total_turns = 0;
    std::string summary;
    nlohmann::json evaluation = nlohmann::json::object();
    persona::SessionMetrics metrics;
};

struct SystemStatsGatewayResponse : GatewayEnvelopeBase {
    std::size_t session_count = 0;
    persona::SessionThreadPoolStats pools;
};

} // namespace agent::service::gateway
