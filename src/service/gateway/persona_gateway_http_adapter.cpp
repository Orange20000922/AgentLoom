#include "persona_gateway_http_adapter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace agent::service::gateway {
namespace {

using Json = nlohmann::json;

std::string ErrorCodeName(core::ErrorCode code) {
    switch (code) {
    case core::ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case core::ErrorCode::NotFound: return "NOT_FOUND";
    case core::ErrorCode::AlreadyExists: return "ALREADY_EXISTS";
    case core::ErrorCode::PermissionDenied: return "PERMISSION_DENIED";
    case core::ErrorCode::FailedPrecondition: return "FAILED_PRECONDITION";
    case core::ErrorCode::ResourceExhausted: return "RESOURCE_EXHAUSTED";
    case core::ErrorCode::Unavailable: return "UNAVAILABLE";
    case core::ErrorCode::Timeout: return "TIMEOUT";
    case core::ErrorCode::InternalError: return "INTERNAL_ERROR";
    case core::ErrorCode::DataLoss: return "DATA_LOSS";
    default: return "UNKNOWN";
    }
}

::net::http::status HttpStatusFor(core::ErrorCode code) {
    switch (code) {
    case core::ErrorCode::InvalidArgument: return ::net::http::status::bad_request;
    case core::ErrorCode::NotFound: return ::net::http::status::not_found;
    case core::ErrorCode::AlreadyExists: return ::net::http::status::conflict;
    case core::ErrorCode::PermissionDenied: return ::net::http::status::forbidden;
    case core::ErrorCode::FailedPrecondition: return ::net::http::status::conflict;
    case core::ErrorCode::ResourceExhausted: return ::net::http::status::too_many_requests;
    case core::ErrorCode::Unavailable: return ::net::http::status::service_unavailable;
    case core::ErrorCode::Timeout: return ::net::http::status::gateway_timeout;
    default: return ::net::http::status::internal_server_error;
    }
}

std::string HeaderValue(const ::net::BeastHttpRequest& req, ::net::http::field field) {
    auto it = req.find(field);
    if (it == req.end()) {
        return {};
    }
    return std::string(it->value());
}

std::string HeaderValue(const ::net::BeastHttpRequest& req, std::string_view field) {
    auto it = req.find(field);
    if (it == req.end()) {
        return {};
    }
    return std::string(it->value());
}

std::filesystem::path PathFromUtf8(std::string_view path) {
#ifdef _WIN32
    std::u8string utf8;
    utf8.reserve(path.size());
    for (const auto ch : path) {
        utf8.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(std::move(utf8));
#else
    return std::filesystem::path(std::string(path));
#endif
}

std::string TraceFrom(const ::net::BeastHttpRequest& req, const Json* body = nullptr) {
    auto trace = HeaderValue(req, "X-Trace-Id");
    if (trace.empty()) {
        trace = HeaderValue(req, "X-Request-Id");
    }
    if (trace.empty() && body && body->is_object() && body->contains("traceId")) {
        trace = body->value("traceId", "");
    }
    if (trace.empty()) {
        trace = core::GenerateTraceId();
    }
    return trace;
}

Json ErrorEnvelope(std::string trace_id, const core::Status& status) {
    return Json{
        {"ok", false},
        {"traceId", trace_id},
        {"error", {
            {"code", ErrorCodeName(status.code())},
            {"message", status.message()},
        }},
    };
}

Json EmotionToJson(const persona::EmotionAnalysis& emotion) {
    return Json{
        {"primary", emotion.emotion.primary},
        {"intensity", emotion.emotion.intensity},
        {"behavior", emotion.behavior},
        {"tone", emotion.tone},
    };
}

std::string StatusName(persona::SessionStatus status) {
    switch (status) {
    case persona::SessionStatus::Creating: return "creating";
    case persona::SessionStatus::Active: return "active";
    case persona::SessionStatus::Disconnected: return "disconnected";
    case persona::SessionStatus::Closing: return "closing";
    case persona::SessionStatus::Closed: return "closed";
    }
    return "unknown";
}

Json MetricsToJson(const persona::SessionMetrics& metrics) {
    const auto avg = metrics.request_count == 0
        ? 0
        : metrics.total_latency.count() / static_cast<long long>(metrics.request_count);
    return Json{
        {"turnCount", metrics.turn_count},
        {"requestCount", metrics.request_count},
        {"failedRequestCount", metrics.failed_request_count},
        {"lastLatencyMs", metrics.last_latency.count()},
        {"totalLatencyMs", metrics.total_latency.count()},
        {"avgLatencyMs", avg},
    };
}

Json SessionToJson(const persona::SessionSnapshot& session) {
    return Json{
        {"sessionId", session.session_id},
        {"userUuid", session.user_uuid},
        {"personaId", session.persona_id},
        {"status", StatusName(session.status)},
        {"closeReason", session.close_reason},
        {"recentTurnCount", session.recent_turn_count},
        {"emotion", {
            {"valence", session.emotion_state.valence},
            {"arousal", session.emotion_state.arousal},
            {"primary", session.emotion_state.last_emotion},
            {"sustainedLabel", session.emotion_state.sustained_label},
        }},
        {"metrics", MetricsToJson(session.metrics)},
    };
}

Json SessionEnvelope(const SessionGatewayResponse& response) {
    return Json{
        {"ok", true},
        {"traceId", response.trace_id},
        {"sessionId", response.session_id},
        {"latencyMs", response.latency.count()},
        {"data", SessionToJson(response.session)},
    };
}

Json PersonalityToJson(const persona::PersonalityConfig& personality) {
    return Json{
        {"name", personality.name},
        {"description", personality.description},
        {"traits", personality.traits},
        {"openness", personality.openness},
        {"extraversion", personality.extraversion},
        {"humorTendency", personality.humor_tendency},
        {"empathyLevel", personality.empathy_level},
        {"curiosityLevel", personality.curiosity_level},
        {"formality", personality.formality},
    };
}

Json EmotionPromptConfigToJson(const std::optional<persona::EmotionPromptConfig>& config) {
    if (!config) {
        return Json(nullptr);
    }
    return Json{
        {"emotionMap", config->emotion_map},
        {"emotionReliability", config->emotion_reliability},
        {"confidenceThresholds", config->confidence_thresholds},
        {"intensityLevels", config->intensity_levels},
    };
}

Json PersonaMetadataToJson(const PersonaMetadataRecord& record) {
    return Json{
        {"tenantId", record.tenant_id},
        {"userUuid", record.user_uuid},
        {"personaId", record.persona_id},
        {"personality", PersonalityToJson(record.personality)},
        {"emotionPrompts", EmotionPromptConfigToJson(record.emotion_prompt_config)},
        {"emotionState", {
            {"alpha", record.emotion_state_config.alpha},
            {"beta", record.emotion_state_config.beta},
            {"gamma", record.emotion_state_config.gamma},
            {"delta", record.emotion_state_config.delta},
            {"baselineValence", record.emotion_state_config.baseline_valence},
            {"baselineArousal", record.emotion_state_config.baseline_arousal},
            {"kappa", record.emotion_state_config.kappa},
            {"negativityBias", record.emotion_state_config.negativity_bias},
            {"noiseSigma", record.emotion_state_config.noise_sigma},
            {"injectionThreshold", record.emotion_state_config.injection_threshold},
            {"saveIntervalTurns", record.emotion_state_config.save_interval_turns},
            {"persistToL4", record.emotion_state_config.persist_to_l4},
        }},
    };
}

Json PersonaMetadataEnvelope(const PersonaMetadataGatewayResponse& response) {
    return Json{
        {"ok", true},
        {"traceId", response.trace_id},
        {"latencyMs", response.latency.count()},
        {"data", PersonaMetadataToJson(response.persona)},
    };
}

Json ChatEnvelope(const ChatGatewayResponse& response) {
    return Json{
        {"ok", true},
        {"traceId", response.trace_id},
        {"sessionId", response.session_id},
        {"latencyMs", response.latency.count()},
        {"data", {
            {"personaId", response.persona_id},
            {"turnIndex", response.turn_index},
            {"reply", {{"role", "assistant"}, {"content", response.content}}},
            {"userEmotion", EmotionToJson(response.user_emotion)},
            {"aiEmotion", EmotionToJson(response.ai_emotion)},
            {"memory", {{"l0Hit", response.l0_hit}, {"l3Hit", response.l3_hit}}},
            {"answerCache", {
                {"enabled", response.answer_cache.enabled},
                {"hit", response.answer_cache.hit},
                {"bypassed", response.answer_cache.bypassed},
                {"source", response.answer_cache.source},
                {"cacheKey", response.answer_cache.cache_key},
                {"similarityScore", response.answer_cache.similarity_score},
            }},
            {"pipelineLatency", {
                {"computeQueueWaitMs", response.pipeline_latency.compute_queue_wait.count()},
                {"computeStageMs", response.pipeline_latency.compute_stage.count()},
                {"ioQueueWaitMs", response.pipeline_latency.io_queue_wait.count()},
                {"ioStageMs", response.pipeline_latency.io_stage.count()},
                {"memoryContextMs", response.pipeline_latency.memory_context.count()},
                {"answerCacheMs", response.pipeline_latency.answer_cache.count()},
                {"promptBuildMs", response.pipeline_latency.prompt_build.count()},
                {"llmTotalMs", response.pipeline_latency.llm_total.count()},
                {"callbackToResponseMs", response.pipeline_latency.callback_to_response.count()},
                {"totalMs", response.pipeline_latency.total.count()},
            }},
        }},
    };
}

Json ClassroomEnvelope(const ClassroomGatewayResponse& response) {
    return Json{
        {"ok", true},
        {"traceId", response.trace_id},
        {"sessionId", response.session_id},
        {"latencyMs", response.latency.count()},
        {"data", {
            {"classroomId", response.classroom_id},
            {"speakerPersonaId", response.speaker_persona_id},
            {"content", response.content},
            {"shouldSpeak", response.should_speak},
            {"turnIndex", response.turn_index},
            {"userEmotion", EmotionToJson(response.user_emotion)},
            {"aiEmotion", EmotionToJson(response.ai_emotion)},
        }},
    };
}

Json ReportEnvelope(const TrainingReportGatewayResponse& response) {
    return Json{
        {"ok", true},
        {"traceId", response.trace_id},
        {"sessionId", response.session_id},
        {"latencyMs", response.latency.count()},
        {"data", {
            {"generatedAt", response.generated_at},
            {"totalTurns", response.total_turns},
            {"summary", response.summary},
            {"evaluation", response.evaluation},
            {"metrics", MetricsToJson(response.metrics)},
            {"schemaVersion", "training_report.v1"},
        }},
    };
}

Json ThreadPoolStatsToJson(const core::ThreadPoolStats& stats) {
    return Json{
        {"workerCount", stats.worker_count},
        {"queuedTasks", stats.queued_tasks},
        {"activeWorkers", stats.active_workers},
        {"submittedTasks", stats.submitted_tasks},
        {"completedTasks", stats.completed_tasks},
        {"failedTasks", stats.failed_tasks},
        {"rejectedTasks", stats.rejected_tasks},
        {"scheduler", {
            {"activeKeys", stats.scheduler.active_keys},
            {"readyKeys", stats.scheduler.ready_keys},
            {"queuedTasks", stats.scheduler.queued_tasks},
            {"runningTasks", stats.scheduler.running_tasks},
            {"rejectedTasks", stats.scheduler.rejected_tasks},
            {"rejectedGlobal", stats.scheduler.rejected_global},
            {"rejectedPerKey", stats.scheduler.rejected_per_key},
            {"rejectedPerFairnessKey", stats.scheduler.rejected_per_fairness_key},
            {"rejectedPerTenant", stats.scheduler.rejected_per_tenant},
            {"maxLaneDepth", stats.scheduler.max_lane_depth},
        }},
    };
}

Json SystemStatsEnvelope(const SystemStatsGatewayResponse& response) {
    return Json{
        {"ok", true},
        {"traceId", response.trace_id},
        {"latencyMs", response.latency.count()},
        {"data", {
            {"sessionCount", response.session_count},
            {"pools", {
                {"compute", ThreadPoolStatsToJson(response.pools.compute)},
                {"io", ThreadPoolStatsToJson(response.pools.io)},
                {"llm", ThreadPoolStatsToJson(response.pools.llm)},
            }},
        }},
    };
}

Json DocumentAnalyzeEnvelope(const document::DocumentAnalyzeResponse& response) {
    return Json{
        {"ok", true},
        {"traceId", response.trace_id},
        {"documentId", response.document_id},
        {"latencyMs", response.latency.total.count()},
        {"data", response.result},
        {"pipelineLatency", {
            {"computeQueueWaitMs", response.latency.compute_queue_wait.count()},
            {"computeStageMs", response.latency.compute_stage.count()},
            {"totalMs", response.latency.total.count()},
        }},
    };
}

Json DocumentMetadataEnvelope(std::string trace_id, const document::DocumentMetadataRecord& record) {
    return Json{
        {"ok", true},
        {"traceId", std::move(trace_id)},
        {"documentId", record.document_id},
        {"data", {
            {"documentId", record.document_id},
            {"contentHash", record.content_hash},
            {"ownerUserUuid", record.owner_user_uuid},
            {"sessionId", record.session_id},
            {"fileName", record.file_name},
            {"fileType", record.file_type},
            {"uploadedAtMs", record.uploaded_at_ms},
            {"lastAnalyzedAtMs", record.last_analyzed_at_ms},
            {"lastAccessedAtMs", record.last_accessed_at_ms},
            {"analysisStatus", record.analysis_status},
            {"analysisTraceId", record.analysis_trace_id},
            {"sizeBytes", record.size_bytes},
            {"schemaVersion", record.schema_version},
        }},
    };
}

std::string SkillStateName(persona::SkillSessionState state) {
    switch (state) {
    case persona::SkillSessionState::Idle: return "idle";
    case persona::SkillSessionState::Starting: return "starting";
    case persona::SkillSessionState::Ready: return "ready";
    case persona::SkillSessionState::Running: return "running";
    case persona::SkillSessionState::WaitingInput: return "waiting_input";
    case persona::SkillSessionState::Closing: return "closing";
    case persona::SkillSessionState::Closed: return "closed";
    case persona::SkillSessionState::Failed: return "failed";
    case persona::SkillSessionState::Expired: return "expired";
    }
    return "unknown";
}

Json SkillObservationToJson(const persona::SkillObservation& observation) {
    Json metadata = Json::object();
    if (!observation.metadata_json.empty()) {
        try {
            metadata = Json::parse(observation.metadata_json);
        } catch (const Json::exception&) {
            metadata = observation.metadata_json;
        }
    }
    return Json{
        {"executionId", observation.execution_id},
        {"skillId", observation.skill_id},
        {"sessionId", observation.session_id},
        {"traceId", observation.trace_id},
        {"summary", observation.summary},
        {"confidence", observation.confidence},
        {"stale", observation.stale},
        {"shouldInjectPrompt", observation.should_inject_prompt},
        {"source", observation.source},
        {"metadata", std::move(metadata)},
    };
}

Json SkillSessionSnapshotToJson(const persona::SkillSessionSnapshot& snapshot) {
    Json observations = Json::array();
    for (const auto& observation : snapshot.recent_observations) {
        observations.push_back(SkillObservationToJson(observation));
    }
    return Json{
        {"executionId", snapshot.execution_id},
        {"skillId", snapshot.skill_id},
        {"sessionId", snapshot.session_id},
        {"userUuid", snapshot.user_uuid},
        {"personaId", snapshot.persona_id},
        {"traceId", snapshot.trace_id},
        {"state", SkillStateName(snapshot.state)},
        {"statusText", snapshot.status_text},
        {"lastObservation", snapshot.last_observation},
        {"lastError", snapshot.last_error},
        {"closeReason", snapshot.close_reason},
        {"maxDurationMs", snapshot.max_duration.count()},
        {"recentObservations", std::move(observations)},
    };
}

Json SkillSessionEnvelope(std::string trace_id, const persona::SkillSessionSnapshot& snapshot) {
    return Json{
        {"ok", true},
        {"traceId", std::move(trace_id)},
        {"data", SkillSessionSnapshotToJson(snapshot)},
    };
}

Json SkillSessionStatusEnvelope(std::string trace_id,
                                const std::optional<persona::SkillSessionSnapshot>& snapshot) {
    return Json{
        {"ok", true},
        {"traceId", std::move(trace_id)},
        {"data", snapshot ? SkillSessionSnapshotToJson(*snapshot) : Json(nullptr)},
    };
}

core::Status EnsureSkillSessionOwner(const std::optional<persona::SkillSessionSnapshot>& snapshot,
                                     const AuthIdentity& identity) {
    if (!identity.authenticated) {
        return core::Status::Error(core::ErrorCode::PermissionDenied, "authenticated account is required");
    }
    if (!snapshot) {
        return core::Status::Ok();
    }
    return snapshot->user_uuid == identity.user_uuid
        ? core::Status::Ok()
        : core::Status::Error(core::ErrorCode::PermissionDenied, "skill session does not belong to authenticated user");
}

core::Status EnsureDocumentUploadOwner(const DocumentUploadSession& session,
                                       const AuthIdentity& identity,
                                       std::uint64_t connection_id) {
    if (session.connection_id != connection_id) {
        return core::Status::Error(
            core::ErrorCode::PermissionDenied,
            "upload session does not belong to websocket connection");
    }
    if (session.owner_user_uuid != identity.user_uuid) {
        return core::Status::Error(
            core::ErrorCode::PermissionDenied,
            "upload session does not belong to authenticated user");
    }
    return core::Status::Ok();
}

core::Result<Json> ParseJsonBody(const ::net::BeastHttpRequest& req) {
    if (req.body().empty()) {
        return Json::object();
    }
    try {
        return Json::parse(req.body());
    } catch (const Json::exception& e) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, e.what());
    }
}

std::vector<std::string> SplitPath(std::string_view target) {
    const auto q = target.find('?');
    if (q != std::string_view::npos) {
        target = target.substr(0, q);
    }
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < target.size()) {
        while (start < target.size() && target[start] == '/') {
            ++start;
        }
        auto end = target.find('/', start);
        if (end == std::string_view::npos) {
            end = target.size();
        }
        if (end > start) {
            parts.emplace_back(target.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

void SendJson(const std::shared_ptr<::net::IHttpRequest>& request,
              ::net::http::status status,
              const Json& body,
              std::string_view trace_id) {
    auto response = ::net::HttpResponse::Json(
        status,
        body.dump(-1, ' ', false, Json::error_handler_t::replace)).message;
    response.set("X-Trace-Id", trace_id);
    request->Respond(std::move(response));
}

void SendJsonWithHeaders(const std::shared_ptr<::net::IHttpRequest>& request,
                         ::net::http::status status,
                         const Json& body,
                         std::string_view trace_id,
                         const std::vector<std::pair<std::string, std::string>>& headers) {
    auto response = ::net::HttpResponse::Json(
        status,
        body.dump(-1, ' ', false, Json::error_handler_t::replace)).message;
    response.set("X-Trace-Id", trace_id);
    for (const auto& [key, value] : headers) {
        response.set(key, value);
    }
    request->Respond(std::move(response));
}

std::int64_t ToUnixSeconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

void SendAuthRegistrationResult(const std::shared_ptr<::net::IHttpRequest>& request,
                                std::string_view trace_id,
                                const AuthRegistrationResult& value) {
    Json body{
        {"ok", true},
        {"traceId", trace_id},
        {"data", {
            {"authenticated", value.identity.authenticated},
            {"userUuid", value.identity.user_uuid},
            {"tenantId", value.identity.tenant_id},
            {"subject", value.identity.subject},
            {"tokenId", value.identity.token_id},
            {"issuedAt", ToUnixSeconds(value.issued_at)},
            {"expiresAt", ToUnixSeconds(value.identity.expires_at)},
            {"token", value.token},
        }},
    };
    SendJsonWithHeaders(
        request,
        ::net::http::status::ok,
        body,
        trace_id,
        {{"Set-Cookie", value.cookie_header}});
}

template <typename T, typename Fn>
void SendResult(const std::shared_ptr<::net::IHttpRequest>& request,
                core::Result<T> result,
                std::string trace_id,
                Fn serializer) {
    if (!result.ok()) {
        SendJson(request, HttpStatusFor(result.status().code()), ErrorEnvelope(trace_id, result.status()), trace_id);
        return;
    }
    auto body = serializer(result.value());
    SendJson(request, ::net::http::status::ok, body, trace_id);
}

persona::PersonalityConfig PersonalityFromJson(const Json& body) {
    persona::PersonalityConfig personality;
    const auto persona_obj = body.value("personality", Json::object());
    personality.name = persona_obj.value("name", body.value("personaId", std::string{}));
    personality.description = persona_obj.value("description", std::string{});
    personality.traits = persona_obj.value("traits", std::vector<std::string>{});
    personality.openness = persona_obj.value("openness", personality.openness);
    personality.extraversion = persona_obj.value("extraversion", personality.extraversion);
    personality.humor_tendency = persona_obj.value("humorTendency", personality.humor_tendency);
    personality.empathy_level = persona_obj.value("empathyLevel", personality.empathy_level);
    personality.curiosity_level = persona_obj.value("curiosityLevel", personality.curiosity_level);
    personality.formality = persona_obj.value("formality", personality.formality);
    return personality;
}

std::optional<persona::EmotionPromptConfig> EmotionPromptConfigFromJson(const Json& body) {
    const auto it = body.find("emotionPrompts");
    if (it == body.end() || !it->is_object()) {
        return std::nullopt;
    }

    persona::EmotionPromptConfig config;
    config.emotion_map = it->value("emotionMap", std::map<std::string, std::string>{});
    if (config.emotion_map.empty()) {
        config.emotion_map = it->value("emotion_map", std::map<std::string, std::string>{});
    }
    config.emotion_reliability = it->value("emotionReliability", std::map<std::string, double>{});
    if (config.emotion_reliability.empty()) {
        config.emotion_reliability = it->value("emotion_reliability", std::map<std::string, double>{});
    }
    config.confidence_thresholds = it->value("confidenceThresholds", config.confidence_thresholds);
    if (!it->contains("confidenceThresholds")) {
        config.confidence_thresholds = it->value("confidence_thresholds", config.confidence_thresholds);
    }
    config.intensity_levels = it->value("intensityLevels", config.intensity_levels);
    if (!it->contains("intensityLevels")) {
        config.intensity_levels = it->value("intensity_levels", config.intensity_levels);
    }
    return config;
}

persona::EmotionStateConfig EmotionStateConfigFromJson(const Json& body) {
    persona::EmotionStateConfig config;
    const auto it = body.find("emotionState");
    if (it == body.end() || !it->is_object()) {
        return config;
    }
    config.alpha = it->value("alpha", config.alpha);
    config.beta = it->value("beta", config.beta);
    config.gamma = it->value("gamma", config.gamma);
    config.delta = it->value("delta", config.delta);
    config.baseline_valence = it->value("baselineValence", it->value("baseline_valence", config.baseline_valence));
    config.baseline_arousal = it->value("baselineArousal", it->value("baseline_arousal", config.baseline_arousal));
    config.kappa = it->value("kappa", config.kappa);
    config.negativity_bias = it->value("negativityBias", it->value("negativity_bias", config.negativity_bias));
    config.noise_sigma = it->value("noiseSigma", it->value("noise_sigma", config.noise_sigma));
    config.injection_threshold = it->value("injectionThreshold", it->value("injection_threshold", config.injection_threshold));
    config.save_interval_turns = it->value("saveIntervalTurns", it->value("save_interval_turns", config.save_interval_turns));
    config.persist_to_l4 = it->value("persistToL4", it->value("persist_to_l4", config.persist_to_l4));
    return config;
}

std::string MessagePayloadToString(const ::net::WebSocketMessage& message) {
    std::string out;
    for (const auto& fragment : message.fragments) {
        out.append(fragment.view());
    }
    return out;
}

int Base64Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

core::Result<std::string> Base64Decode(std::string_view input) {
    std::string out;
    out.reserve(input.size() * 3 / 4);
    int value = 0;
    int bits = -8;
    bool padding = false;
    for (char ch : input) {
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            continue;
        }
        if (ch == '=') {
            padding = true;
            continue;
        }
        if (padding) {
            return core::Status::Error(core::ErrorCode::InvalidArgument, "invalid base64 padding");
        }
        const int decoded = Base64Value(ch);
        if (decoded < 0) {
            return core::Status::Error(core::ErrorCode::InvalidArgument, "invalid base64 character");
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

::net::WebSocketFrame TextFrame(core::RawMemoryPool& pool, std::string_view text) {
    ::net::WebSocketFrame frame;
    frame.kind = ::net::WebSocketMessageKind::Text;
    auto copied = ::net::SharedBuffer::Copy(pool, text);
    if (copied.ok()) {
        frame.payload = std::move(copied).value();
    }
    return frame;
}

void SendWsError(const std::shared_ptr<::net::IWebSocketStreamRequest>& request,
                 std::string_view trace_id,
                 const core::Status& status) {
    Json out{{"type", "error"}, {"payload", ErrorEnvelope(std::string(trace_id), status)}};
    request->Send(TextFrame(request->memory_pool(), out.dump()));
}

std::filesystem::path UploadTempPath(std::string_view upload_id) {
    std::string file_name = "agent_document_upload_";
    for (char ch : upload_id) {
        const auto safe = std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
        file_name.push_back(safe ? ch : '_');
    }
    file_name += ".tmp";
    return std::filesystem::temp_directory_path() / file_name;
}

core::Status WriteUploadChunk(const std::filesystem::path& path,
                              std::string_view data,
                              std::uint64_t offset) {
    std::fstream file;
    if (offset == 0) {
        file.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    } else {
        file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    }
    if (!file) {
        return core::Status::Error(core::ErrorCode::InternalError, "failed to open upload temp file");
    }
    file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return core::Status::Error(core::ErrorCode::InternalError, "failed to seek upload temp file");
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file) {
        return core::Status::Error(core::ErrorCode::InternalError, "failed to write upload chunk");
    }
    return core::Status::Ok();
}

void RemoveFileQuietly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

core::Status AppendBinaryUploadFrame(
    std::unordered_map<std::string, DocumentUploadSession>& uploads,
    std::uint64_t connection_id,
    const ::net::WebSocketMessage& message,
    std::string* upload_id,
    std::uint64_t* received_size,
    std::uint64_t* expected_size) {
    auto it = std::find_if(
        uploads.begin(),
        uploads.end(),
        [connection_id](const auto& entry) {
            return entry.second.binary_mode && entry.second.connection_id == connection_id;
        });
    if (it == uploads.end()) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "no active binary upload for websocket connection");
    }

    auto& session = it->second;
    if (session.connection_id != connection_id) {
        return core::Status::Error(
            core::ErrorCode::PermissionDenied,
            "upload session does not belong to websocket connection");
    }
    if (message.fragments.empty()) {
        return core::Status::Ok();
    }
    std::uint64_t frame_bytes = 0;
    for (const auto& fragment : message.fragments) {
        frame_bytes += fragment.size();
    }
    if (session.received_size + frame_bytes > session.expected_size) {
        const auto temp_path = session.temp_path;
        uploads.erase(it);
        RemoveFileQuietly(temp_path);
        return core::Status::Error(core::ErrorCode::ResourceExhausted, "binary upload exceeds declared totalBytes");
    }

    auto offset = session.received_size;
    for (const auto& fragment : message.fragments) {
        auto status = WriteUploadChunk(session.temp_path, fragment.view(), offset);
        if (!status.ok()) {
            return status;
        }
        offset += fragment.size();
    }
    session.received_size += frame_bytes;
    if (upload_id) {
        *upload_id = session.upload_id;
    }
    if (received_size) {
        *received_size = session.received_size;
    }
    if (expected_size) {
        *expected_size = session.expected_size;
    }
    return core::Status::Ok();
}

} // namespace

PersonaGatewayHttpAdapter::PersonaGatewayHttpAdapter(PersonaGatewayService& service,
                                                     std::shared_ptr<IGatewayAuthenticator> authenticator,
                                                     std::shared_ptr<IAuthRegistrationService> auth_registration,
                                                     std::shared_ptr<document::DocumentAnalysisService> document_service,
                                                     std::shared_ptr<llm::ILlmClient> llm_client,
                                                     std::shared_ptr<document::IDocumentEmbeddingProvider> embedding_provider,
                                                     std::shared_ptr<document::IDocumentLlmChunkCache> llm_chunk_cache,
                                                     std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache,
                                                     std::shared_ptr<persona::ISkillSessionManager> skill_session_manager,
                                                     bool enable_dev_registration,
                                                     std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router)
    : PersonaGatewayHttpAdapter(service,
                                std::move(authenticator),
                                std::move(auth_registration),
                                std::move(document_service),
                                std::move(llm_client),
                                std::move(embedding_provider),
                                std::move(llm_chunk_cache),
                                std::move(document_semantic_cache),
                                std::move(skill_session_manager),
                                PersonaGatewayHttpAdapterOptions{
                                    .enable_dev_registration = enable_dev_registration,
                                    .enable_path_register_test_endpoint = false,
                                    .enable_path_analyze_test_endpoint = false},
                                std::move(stateful_skill_router)) {}

PersonaGatewayHttpAdapter::PersonaGatewayHttpAdapter(PersonaGatewayService& service,
                                                     std::shared_ptr<IGatewayAuthenticator> authenticator,
                                                     std::shared_ptr<IAuthRegistrationService> auth_registration,
                                                     std::shared_ptr<document::DocumentAnalysisService> document_service,
                                                     std::shared_ptr<llm::ILlmClient> llm_client,
                                                     std::shared_ptr<document::IDocumentEmbeddingProvider> embedding_provider,
                                                     std::shared_ptr<document::IDocumentLlmChunkCache> llm_chunk_cache,
                                                     std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache,
                                                     std::shared_ptr<persona::ISkillSessionManager> skill_session_manager,
                                                     PersonaGatewayHttpAdapterOptions options,
                                                     std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router)
    : service_(service),
      authenticator_(std::move(authenticator)),
      auth_registration_(std::move(auth_registration)),
      document_service_(std::move(document_service)),
      llm_client_(std::move(llm_client)),
      embedding_provider_(std::move(embedding_provider)),
      llm_chunk_cache_(std::move(llm_chunk_cache)),
      document_semantic_cache_(std::move(document_semantic_cache)),
      skill_session_manager_(std::move(skill_session_manager)),
      stateful_skill_router_(std::move(stateful_skill_router)),
      options_(options) {}

bool PersonaGatewayHttpAdapter::IsApiRequest(std::string_view target) noexcept {
    const auto q = target.find('?');
    if (q != std::string_view::npos) {
        target = target.substr(0, q);
    }
    return target == "/api" || target.starts_with("/api/") ||
           target == "/v1/chat/completions" || target == "/v1/systemone";
}

void PersonaGatewayHttpAdapter::CleanupUnfinishedDocumentUploadsForConnection(std::uint64_t connection_id) {
    std::vector<std::filesystem::path> temp_paths;
    {
        std::lock_guard lock(document_upload_mutex_);
        for (auto it = document_uploads_.begin(); it != document_uploads_.end();) {
            if (it->second.connection_id != connection_id) {
                ++it;
                continue;
            }
            temp_paths.push_back(it->second.temp_path);
            it = document_uploads_.erase(it);
        }
    }

    for (const auto& temp_path : temp_paths) {
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
    }
}

void PersonaGatewayHttpAdapter::HandleHttp(std::shared_ptr<::net::IHttpRequest> request) {
    const auto& msg = request->message();
    if (!IsApiRequest(msg.target())) {
        const auto trace_id = TraceFrom(msg);
        const auto status = core::Status::Error(core::ErrorCode::NotFound, "route not found");
        SendJson(request, ::net::http::status::not_found, ErrorEnvelope(trace_id, status), trace_id);
        return;
    }

    const auto parts = SplitPath(msg.target());
    if (!parts.empty() && parts.front() == "v1" && !options_.enable_llm_dev_routes) {
        const auto trace = TraceFrom(msg);
        SendJson(request, ::net::http::status::not_found,
                 ErrorEnvelope(trace, core::Status::Error(core::ErrorCode::NotFound, "route not found")), trace);
        return;
    }
    auto parsed = ParseJsonBody(msg);
    const Json* parsed_body = parsed.ok() ? &parsed.value() : nullptr;
    const auto trace_id = TraceFrom(msg, parsed_body);
    if (!parsed.ok()) {
        SendJson(request, ::net::http::status::bad_request, ErrorEnvelope(trace_id, parsed.status()), trace_id);
        return;
    }
    const auto& body = parsed.value();
    auto routes = HttpRouteRegistry::Instance().CreateRoutes();
    std::unordered_map<std::string, std::string> params;
    for (const auto& route : routes) {
        if (!route->Matches(msg.method(), parts, params)) {
            continue;
        }
        AuthIdentity identity;
        if (authenticator_ && route->RequiresAuth()) {
            auto auth = authenticator_->Authenticate(msg);
            if (!auth.ok()) {
                SendJson(request, HttpStatusFor(auth.status().code()), ErrorEnvelope(trace_id, auth.status()), trace_id);
                return;
            }
            identity = std::move(auth).value();
        }
        if (route->RequiresAuthenticatedIdentity() && !identity.authenticated) {
            const auto status = core::Status::Error(core::ErrorCode::PermissionDenied, "authenticated account is required");
            SendJson(request, HttpStatusFor(status.code()), ErrorEnvelope(trace_id, status), trace_id);
            return;
        }
        HttpRouteContext context{
            service_,
            document_service_,
            llm_client_,
            embedding_provider_,
            llm_chunk_cache_,
            document_semantic_cache_,
            skill_session_manager_,
            stateful_skill_router_,
            auth_registration_,
            options_.enable_dev_registration,
            options_.enable_path_register_test_endpoint,
            options_.enable_path_analyze_test_endpoint,
            std::move(request),
            msg,
            body,
            identity,
            trace_id,
            parts,
            std::move(params),
            options_.forward_jev,
        };
        route->Handle(context);
        return;
    }

    const auto status = core::Status::Error(core::ErrorCode::NotFound, "route not found");
    SendJson(request, ::net::http::status::not_found, ErrorEnvelope(trace_id, status), trace_id);
}

void PersonaGatewayHttpAdapter::HandleWebSocket(std::shared_ptr<::net::IWebSocketStreamRequest> request) {
    if (!request->message().ok()) {
        auto error = ErrorEnvelope(core::GenerateTraceId(), request->message().status);
        request->Send(TextFrame(request->memory_pool(), error.dump()));
        return;
    }

    if (request->message().kind == ::net::WebSocketMessageKind::Binary) {
        std::string upload_id;
        std::uint64_t received = 0;
        std::uint64_t total = 0;
        core::Status status;
        {
            std::lock_guard lock(document_upload_mutex_);
            status = AppendBinaryUploadFrame(
                document_uploads_,
                request->connection().connection_id,
                request->message(),
                &upload_id,
                &received,
                &total);
        }
        if (!status.ok()) {
            SendWsError(request, core::GenerateTraceId(), status);
            return;
        }
        Json out{
            {"type", "document.upload.chunk_ack"},
            {"payload", {
                {"ok", true},
                {"traceId", core::GenerateTraceId()},
                {"uploadId", upload_id},
                {"mode", "binary"},
                {"receivedBytes", received},
                {"totalBytes", total},
                {"finalFragment", request->message().final_fragment},
            }},
        };
        request->Send(TextFrame(request->memory_pool(), out.dump()));
        return;
    }

    const auto text = MessagePayloadToString(request->message());
    Json body;
    try {
        body = Json::parse(text);
    } catch (const Json::exception& e) {
        auto error = ErrorEnvelope(core::GenerateTraceId(), core::Status::Error(core::ErrorCode::InvalidArgument, e.what()));
        request->Send(TextFrame(request->memory_pool(), error.dump()));
        return;
    }

    const auto type = body.value("type", std::string{});
    const auto trace_id = body.value("traceId", core::GenerateTraceId());
    auto routes = WsRouteRegistry::Instance().CreateRoutes();
    for (const auto& route : routes) {
        if (!route->Matches(type)) {
            continue;
        }
        AuthIdentity identity;
        if (authenticator_ && route->RequiresAuth()) {
            auto auth = authenticator_->Authenticate(request->handshake_request());
            if (!auth.ok()) {
                Json out{{"type", "error"}, {"payload", ErrorEnvelope(trace_id, auth.status())}};
                request->Send(TextFrame(request->memory_pool(), out.dump()));
                return;
            }
            identity = std::move(auth).value();
        }
        if (route->RequiresAuthenticatedIdentity() && !identity.authenticated) {
            const auto status = core::Status::Error(core::ErrorCode::PermissionDenied, "authenticated account is required");
            Json out{{"type", "error"}, {"payload", ErrorEnvelope(trace_id, status)}};
            request->Send(TextFrame(request->memory_pool(), out.dump()));
            return;
        }
        WsRouteContext context{
            service_,
            document_service_,
            llm_client_,
            embedding_provider_,
            llm_chunk_cache_,
            document_semantic_cache_,
            skill_session_manager_,
            stateful_skill_router_,
            std::move(request),
            body,
            identity,
            trace_id,
            document_upload_mutex_,
            document_uploads_,
        };
        route->Handle(context);
        return;
    }

    auto error = ErrorEnvelope(trace_id, core::Status::Error(core::ErrorCode::InvalidArgument, "unknown websocket message type"));
    request->Send(TextFrame(request->memory_pool(), Json{{"type", "error"}, {"payload", error}}.dump()));
}

} // namespace agent::service::gateway
