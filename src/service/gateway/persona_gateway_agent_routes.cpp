#include "persona_gateway_route_helpers.h"
#include "chat_sse_routes.h"

namespace agent::service::gateway {
namespace {

using namespace route_detail;

DECLARE_HTTP_ROUTE(AuthMeRoute, ::net::http::verb::get, "api", "auth", "me") {
    Json body{
        {"ok", true},
        {"traceId", context.trace_id},
        {"data", {
            {"authenticated", context.identity.authenticated},
            {"userUuid", context.identity.user_uuid},
            {"tenantId", context.identity.tenant_id},
            {"subject", context.identity.subject},
        }},
    };
    SendJson(context.request, ::net::http::status::ok, body, context.trace_id);
}

DECLARE_PUBLIC_HTTP_ROUTE(AuthRegisterRoute, ::net::http::verb::post, "api", "auth", "register") {
    if (!context.auth_registration) {
        const auto status = core::Status::Error(core::ErrorCode::FailedPrecondition, "auth registration service is not configured");
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
        return;
    }
    if (!context.enable_dev_registration) {
        const auto status = core::Status::Error(
            core::ErrorCode::PermissionDenied,
            "development auth registration is disabled");
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
        return;
    }

    AuthRegistrationRequest req;
    req.user_uuid = context.body.value("userUuid", context.body.value("user_uuid", std::string{}));
    req.tenant_id = context.body.value("tenantId", context.body.value("tenant_id", std::string{"default"}));
    req.subject = context.body.value("subject", std::string{});
    const auto ttl_seconds = context.body.value("ttlSeconds", context.body.value("ttl_seconds", 0));
    if (ttl_seconds > 0) {
        req.ttl = std::chrono::seconds(ttl_seconds);
    }

    auto result = context.auth_registration->Register(req);
    if (!result.ok()) {
        SendJson(context.request, HttpStatusFor(result.status().code()), ErrorEnvelope(context.trace_id, result.status()), context.trace_id);
        return;
    }

    SendAuthRegistrationResult(context.request, context.trace_id, result.value());
}

DECLARE_PUBLIC_HTTP_ROUTE(AuthSignupRoute, ::net::http::verb::post, "api", "auth", "signup") {
    if (!context.auth_registration) {
        const auto status = core::Status::Error(core::ErrorCode::FailedPrecondition, "auth registration service is not configured");
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
        return;
    }

    AuthRegistrationRequest req;
    req.username = context.body.value(
        "username",
        context.body.value("email", std::string{}));
    req.password = context.body.value("password", std::string{});
    const auto ttl_seconds = context.body.value("ttlSeconds", context.body.value("ttl_seconds", 0));
    if (ttl_seconds > 0) {
        req.ttl = std::chrono::seconds(ttl_seconds);
    }

    auto result = context.auth_registration->Register(req);
    if (!result.ok()) {
        SendJson(context.request, HttpStatusFor(result.status().code()), ErrorEnvelope(context.trace_id, result.status()), context.trace_id);
        return;
    }

    SendAuthRegistrationResult(context.request, context.trace_id, result.value());
}

DECLARE_PUBLIC_HTTP_ROUTE(AuthLoginRoute, ::net::http::verb::post, "api", "auth", "login") {
    if (!context.auth_registration) {
        const auto status = core::Status::Error(core::ErrorCode::FailedPrecondition, "auth registration service is not configured");
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
        return;
    }

    AuthLoginRequest req;
    req.username = context.body.value(
        "username",
        context.body.value("email", std::string{}));
    req.password = context.body.value("password", std::string{});
    const auto ttl_seconds = context.body.value("ttlSeconds", context.body.value("ttl_seconds", 0));
    if (ttl_seconds > 0) {
        req.ttl = std::chrono::seconds(ttl_seconds);
    }

    auto result = context.auth_registration->Login(req);
    if (!result.ok()) {
        SendJson(context.request, HttpStatusFor(result.status().code()), ErrorEnvelope(context.trace_id, result.status()), context.trace_id);
        return;
    }

    SendAuthRegistrationResult(context.request, context.trace_id, result.value());
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(UpsertPersonaMetadataRoute, ::net::http::verb::post, "api", "persona") {
    PersonaMetadataGatewayRequest req;
    req.trace_id = context.trace_id;
    req.tenant_id = context.identity.tenant_id.empty() ? "default" : context.identity.tenant_id;
    req.user_uuid = context.identity.user_uuid;
    req.persona_id = context.body.value("personaId", context.body.value("persona_id", std::string{}));
    req.personality = PersonalityFromJson(context.body);
    req.emotion_prompt_config = EmotionPromptConfigFromJson(context.body);
    req.emotion_state_config = EmotionStateConfigFromJson(context.body);
    SendResult(
        context.request,
        context.service.UpsertPersonaMetadata(std::move(req)),
        context.trace_id,
        PersonaMetadataEnvelope);
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(GetPersonaMetadataRoute, ::net::http::verb::get, "api", "persona", "{personaId}") {
    SendResult(
        context.request,
        context.service.GetPersonaMetadata(
            context.identity.tenant_id.empty() ? "default" : context.identity.tenant_id,
            context.identity.user_uuid,
            context.path_params.at("personaId"),
            context.trace_id),
        context.trace_id,
        PersonaMetadataEnvelope);
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(ListPersonaMetadataByAccountRoute, ::net::http::verb::get, "api", "personas") {
    SendResult(
        context.request,
        context.service.ListPersonaMetadataByAccount(
            context.identity.tenant_id.empty() ? "default" : context.identity.tenant_id,
            context.identity.user_uuid,
            context.trace_id),
        context.trace_id,
        [&context](const std::vector<PersonaMetadataRecord>& records) {
            Json data = Json::array();
            for (const auto& record : records) {
                data.push_back(PersonaMetadataToJson(record));
            }
            return Json{
                {"ok", true},
                {"traceId", context.trace_id},
                {"data", std::move(data)},
            };
        });
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(CreateSessionRoute, ::net::http::verb::post, "api", "session", "create") {
    CreateSessionGatewayRequest req;
    req.trace_id = context.trace_id;
    req.session_id = context.body.value("sessionId", std::string{});
    req.user_uuid = context.identity.user_uuid;
    req.tenant_id = context.identity.tenant_id;
    req.classroom_id = context.body.value("classroomId", std::string{});
    req.persona_id = context.body.value("personaId", std::string{});
    req.context_ids = context.body.value("contextIds", std::vector<std::string>{});
    req.context_patterns = context.body.value("contextPatterns", std::vector<std::string>{});
    req.proactive_level = context.body.value("proactiveLevel", std::string{"off"});
    req.default_persona = context.body.value("defaultPersona", false);
    SendResult(context.request, context.service.CreateSession(std::move(req)), context.trace_id, SessionEnvelope);
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(CloseSessionRoute, ::net::http::verb::post, "api", "session", "close") {
    CloseSessionGatewayRequest req;
    req.trace_id = context.trace_id;
    req.session_id = context.body.value("sessionId", std::string{});
    req.authenticated_user_uuid = context.identity.user_uuid;
    req.reason = context.body.value("reason", std::string{"client_close"});
    SendResult(context.request, context.service.CloseSession(std::move(req)), context.trace_id, SessionEnvelope);
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(GetSessionRoute, ::net::http::verb::get, "api", "session", "{sessionId}") {
    SendResult(
        context.request,
        context.service.GetSession(
            context.path_params.at("sessionId"),
            context.trace_id,
            context.identity.user_uuid),
        context.trace_id,
        SessionEnvelope);
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(GetSessionEmotionRoute, ::net::http::verb::get, "api", "session", "{sessionId}", "emotion") {
    SendResult(
        context.request,
        context.service.GetSession(
            context.path_params.at("sessionId"),
            context.trace_id,
            context.identity.user_uuid),
        context.trace_id,
        [](const SessionGatewayResponse& r) {
            return Json{
                {"ok", true},
                {"traceId", r.trace_id},
                {"sessionId", r.session_id},
                {"latencyMs", r.latency.count()},
                {"data", SessionToJson(r.session)["emotion"]},
            };
        });
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(GetSessionMetricsRoute, ::net::http::verb::get, "api", "session", "{sessionId}", "metrics") {
    SendResult(
        context.request,
        context.service.GetSession(
            context.path_params.at("sessionId"),
            context.trace_id,
            context.identity.user_uuid),
        context.trace_id,
        [](const SessionGatewayResponse& r) {
            return Json{
                {"ok", true},
                {"traceId", r.trace_id},
                {"sessionId", r.session_id},
                {"latencyMs", r.latency.count()},
                {"data", SessionToJson(r.session)["metrics"]},
            };
        });
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(ChatMessageRoute, ::net::http::verb::post, "api", "chat", "message") {
    ChatGatewayRequest req;
    req.trace_id = context.trace_id;
    req.session_id = context.body.value("sessionId", std::string{});
    req.authenticated_user_uuid = context.identity.user_uuid;
    req.persona_id = context.body.value("personaId", std::string{});
    req.mode = context.body.value("mode", std::string{"chat"});
    req.message = context.body.value("message", std::string{});
    req.model = context.body.value("model", std::string{});
    req.stream = context.body.value("stream", false);
    if (req.stream) {
        HandleChatSse(context, std::move(req));
        return;
    }
    auto trace_id = context.trace_id;
    auto request = context.request;
    auto status = context.service.SubmitChat(
        std::move(req),
        [request = std::move(request), trace_id](core::Result<ChatGatewayResponse> result) mutable {
            SendResult(request, std::move(result), trace_id, ChatEnvelope);
        });
    if (!status.ok()) {
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(trace_id, status), trace_id);
    }
}

DECLARE_HTTP_ROUTE(SystemStatsRoute, ::net::http::verb::get, "api", "system", "stats") {
    SendResult(context.request, context.service.SystemStats(context.trace_id), context.trace_id, SystemStatsEnvelope);
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(SkillSessionStartRoute, ::net::http::verb::post, "api", "skill", "session", "start") {
    if (!context.skill_session_manager) {
        const auto status = core::Status::Error(core::ErrorCode::FailedPrecondition, "skill session manager is not configured");
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
        return;
    }
    persona::SkillSessionStartRequest req;
    req.execution_id = context.body.value("executionId", context.body.value("execution_id", std::string{}));
    req.trace_id = context.trace_id;
    req.skill_id = context.body.value("skillId", context.body.value("skill_id", std::string{}));
    req.session_id = context.body.value("sessionId", context.body.value("session_id", std::string{}));
    req.user_uuid = context.identity.user_uuid;
    req.persona_id = context.body.value("personaId", context.body.value("persona_id", std::string{}));
    req.source = context.body.value("source", std::string{"http"});
    req.reason = context.body.value("reason", std::string{});
    if (context.body.contains("arguments")) {
        req.arguments_json = context.body["arguments"].dump(-1, ' ', false, Json::error_handler_t::replace);
    } else {
        req.arguments_json = context.body.value("argumentsJson", context.body.value("arguments_json", std::string{"{}"}));
    }
    const auto max_duration_ms = context.body.value("maxDurationMs", context.body.value("max_duration_ms", std::int64_t{0}));
    if (max_duration_ms > 0) {
        req.max_duration = std::chrono::milliseconds(max_duration_ms);
    }
    auto result = context.stateful_skill_router
        ? context.stateful_skill_router->Start(req)
        : context.skill_session_manager->Start(req);
    SendResult(context.request, std::move(result), context.trace_id, [&trace_id = context.trace_id](const auto& snapshot) {
        return SkillSessionEnvelope(trace_id, snapshot);
    });
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(SkillSessionStopRoute, ::net::http::verb::post, "api", "skill", "session", "stop") {
    if (!context.skill_session_manager) {
        const auto status = core::Status::Error(core::ErrorCode::FailedPrecondition, "skill session manager is not configured");
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
        return;
    }
    persona::SkillSessionStopRequest req;
    req.execution_id = context.body.value("executionId", context.body.value("execution_id", std::string{}));
    req.trace_id = context.trace_id;
    req.skill_id = context.body.value("skillId", context.body.value("skill_id", std::string{}));
    req.session_id = context.body.value("sessionId", context.body.value("session_id", std::string{}));
    req.authenticated_user_uuid = context.identity.user_uuid;
    req.source = context.body.value("source", std::string{"http"});
    req.reason = context.body.value("reason", std::string{"client_stop"});
    req.summarize = context.body.value("summarize", true);
    req.write_l3 = context.body.value("writeL3", context.body.value("write_l3", false));
    auto result = context.stateful_skill_router
        ? context.stateful_skill_router->Stop(req)
        : context.skill_session_manager->Stop(req);
    SendResult(context.request, std::move(result), context.trace_id, [&trace_id = context.trace_id](const auto& snapshot) {
        return SkillSessionEnvelope(trace_id, snapshot);
    });
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(SkillSessionStatusRoute, ::net::http::verb::post, "api", "skill", "session", "status") {
    if (!context.skill_session_manager) {
        const auto status = core::Status::Error(core::ErrorCode::FailedPrecondition, "skill session manager is not configured");
        SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
        return;
    }
    const auto skill_id = context.body.value("skillId", context.body.value("skill_id", std::string{}));
    const auto session_id = context.body.value("sessionId", context.body.value("session_id", std::string{}));
    auto result = context.skill_session_manager->Get(session_id, skill_id);
    if (result.ok()) {
        auto owner = EnsureSkillSessionOwner(result.value(), context.identity);
        if (!owner.ok()) {
            SendJson(context.request, HttpStatusFor(owner.code()), ErrorEnvelope(context.trace_id, owner), context.trace_id);
            return;
        }
    }
    SendResult(context.request, std::move(result), context.trace_id, [&trace_id = context.trace_id](const auto& snapshot) {
        return SkillSessionStatusEnvelope(trace_id, snapshot);
    });
}

class HealthRoute final : public IHttpRoute {
public:
    static constexpr std::string_view kRouteName = "HealthRoute";
    ::net::http::verb Method() const noexcept override { return ::net::http::verb::get; }
    std::vector<std::string_view> Pattern() const override { return {"api", "health"}; }
    bool RequiresAuth() const noexcept override { return false; }

    void Handle(HttpRouteContext& context) const override {
        auto stats = context.service.SystemStats(context.trace_id);
        if (!stats.ok()) {
            SendJson(context.request, HttpStatusFor(stats.status().code()), ErrorEnvelope(context.trace_id, stats.status()), context.trace_id);
            return;
        }

        Json body{
            {"ok", true},
            {"traceId", context.trace_id},
            {"latencyMs", stats.value().latency.count()},
            {"data", {
                {"status", "ok"},
                {"sessionCount", stats.value().session_count},
                {"pools", {
                    {"compute", ThreadPoolStatsToJson(stats.value().pools.compute)},
                    {"io", ThreadPoolStatsToJson(stats.value().pools.io)},
                    {"llm", ThreadPoolStatsToJson(stats.value().pools.llm)},
                }},
            }},
        };
        SendJson(context.request, ::net::http::status::ok, body, context.trace_id);
    }
};
static const HttpRouteRegistrar<HealthRoute> g_HealthRoute_registrar;

DECLARE_AUTHENTICATED_WS_ROUTE(ChatMessageWsRoute, "chat.message") {
    const auto payload = context.body.value("payload", Json::object());
    ChatGatewayRequest req;
    req.trace_id = context.trace_id;
    req.session_id = payload.value("sessionId", std::string{});
    req.authenticated_user_uuid = context.identity.user_uuid;
    req.persona_id = payload.value("personaId", std::string{});
    req.mode = payload.value("mode", std::string{"ws_chat"});
    req.message = payload.value("message", std::string{});
    req.model = payload.value("model", std::string{});
    auto ws_request = context.request;
    auto status = context.service.SubmitChat(
        std::move(req),
        [request = std::move(ws_request), trace_id = context.trace_id](core::Result<ChatGatewayResponse> result) mutable {
            Json out = result.ok()
                ? Json{{"type", "chat.final"}, {"payload", ChatEnvelope(result.value())}}
                : Json{{"type", "error"}, {"payload", ErrorEnvelope(trace_id, result.status())}};
            request->Send(TextFrame(request->memory_pool(), out.dump()));
        });
    if (!status.ok()) {
        Json out{{"type", "error"}, {"payload", ErrorEnvelope(context.trace_id, status)}};
        context.request->Send(TextFrame(context.request->memory_pool(), out.dump()));
    }
}

DECLARE_AUTHENTICATED_WS_ROUTE(SessionCloseWsRoute, "session.close") {
    const auto payload = context.body.value("payload", Json::object());
    CloseSessionGatewayRequest req;
    req.trace_id = context.trace_id;
    req.session_id = payload.value("sessionId", std::string{});
    req.authenticated_user_uuid = context.identity.user_uuid;
    req.reason = payload.value("reason", std::string{"client_close"});
    auto result = context.service.CloseSession(std::move(req));
    Json out = result.ok()
        ? Json{{"type", "session.closed"}, {"payload", SessionEnvelope(result.value())}}
        : Json{{"type", "error"}, {"payload", ErrorEnvelope(context.trace_id, result.status())}};
    context.request->Send(TextFrame(context.request->memory_pool(), out.dump()));
}

DECLARE_AUTHENTICATED_WS_ROUTE(SkillSessionStartWsRoute, "skill.session.start") {
    if (!context.skill_session_manager) {
        SendWsError(
            context.request,
            context.trace_id,
            core::Status::Error(core::ErrorCode::FailedPrecondition, "skill session manager is not configured"));
        return;
    }
    const auto payload = context.body.value("payload", Json::object());
    persona::SkillSessionStartRequest req;
    req.execution_id = payload.value("executionId", payload.value("execution_id", std::string{}));
    req.trace_id = context.trace_id;
    req.skill_id = payload.value("skillId", payload.value("skill_id", std::string{}));
    req.session_id = payload.value("sessionId", payload.value("session_id", std::string{}));
    req.user_uuid = context.identity.user_uuid;
    req.persona_id = payload.value("personaId", payload.value("persona_id", std::string{}));
    req.source = payload.value("source", std::string{"websocket"});
    req.reason = payload.value("reason", std::string{});
    if (payload.contains("arguments")) {
        req.arguments_json = payload["arguments"].dump(-1, ' ', false, Json::error_handler_t::replace);
    } else {
        req.arguments_json = payload.value("argumentsJson", payload.value("arguments_json", std::string{"{}"}));
    }
    const auto max_duration_ms = payload.value("maxDurationMs", payload.value("max_duration_ms", std::int64_t{0}));
    if (max_duration_ms > 0) {
        req.max_duration = std::chrono::milliseconds(max_duration_ms);
    }
    auto result = context.stateful_skill_router
        ? context.stateful_skill_router->Start(req)
        : context.skill_session_manager->Start(req);
    Json out = result.ok()
        ? Json{{"type", "skill.session.started"}, {"payload", SkillSessionEnvelope(context.trace_id, result.value())}}
        : Json{{"type", "error"}, {"payload", ErrorEnvelope(context.trace_id, result.status())}};
    context.request->Send(TextFrame(context.request->memory_pool(), out.dump()));
}

DECLARE_AUTHENTICATED_WS_ROUTE(SkillSessionStopWsRoute, "skill.session.stop") {
    if (!context.skill_session_manager) {
        SendWsError(
            context.request,
            context.trace_id,
            core::Status::Error(core::ErrorCode::FailedPrecondition, "skill session manager is not configured"));
        return;
    }
    const auto payload = context.body.value("payload", Json::object());
    persona::SkillSessionStopRequest req;
    req.execution_id = payload.value("executionId", payload.value("execution_id", std::string{}));
    req.trace_id = context.trace_id;
    req.skill_id = payload.value("skillId", payload.value("skill_id", std::string{}));
    req.session_id = payload.value("sessionId", payload.value("session_id", std::string{}));
    req.authenticated_user_uuid = context.identity.user_uuid;
    req.source = payload.value("source", std::string{"websocket"});
    req.reason = payload.value("reason", std::string{"client_stop"});
    req.summarize = payload.value("summarize", true);
    req.write_l3 = payload.value("writeL3", payload.value("write_l3", false));
    auto result = context.stateful_skill_router
        ? context.stateful_skill_router->Stop(req)
        : context.skill_session_manager->Stop(req);
    Json out = result.ok()
        ? Json{{"type", "skill.session.stopped"}, {"payload", SkillSessionEnvelope(context.trace_id, result.value())}}
        : Json{{"type", "error"}, {"payload", ErrorEnvelope(context.trace_id, result.status())}};
    context.request->Send(TextFrame(context.request->memory_pool(), out.dump()));
}

DECLARE_AUTHENTICATED_WS_ROUTE(SkillSessionStatusWsRoute, "skill.session.status") {
    if (!context.skill_session_manager) {
        SendWsError(
            context.request,
            context.trace_id,
            core::Status::Error(core::ErrorCode::FailedPrecondition, "skill session manager is not configured"));
        return;
    }
    const auto payload = context.body.value("payload", Json::object());
    const auto skill_id = payload.value("skillId", payload.value("skill_id", std::string{}));
    const auto session_id = payload.value("sessionId", payload.value("session_id", std::string{}));
    auto result = context.skill_session_manager->Get(session_id, skill_id);
    if (result.ok()) {
        auto owner = EnsureSkillSessionOwner(result.value(), context.identity);
        if (!owner.ok()) {
            SendWsError(context.request, context.trace_id, owner);
            return;
        }
    }
    Json out = result.ok()
        ? Json{{"type", "skill.session.status"}, {"payload", SkillSessionStatusEnvelope(context.trace_id, result.value())}}
        : Json{{"type", "error"}, {"payload", ErrorEnvelope(context.trace_id, result.status())}};
    context.request->Send(TextFrame(context.request->memory_pool(), out.dump()));
}

} // namespace
} // namespace agent::service::gateway
