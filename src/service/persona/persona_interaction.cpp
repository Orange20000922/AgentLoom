#include "persona_interaction.h"

#include "trace_context.h"

#include <utility>

namespace agent::service::persona {

PersonaInteraction::PersonaInteraction(SessionManager& sessions,
                                       PersonaRuntime& runtime,
                                       core::LoggerAdapter logger)
    : sessions_(sessions), runtime_(runtime), logger_(std::move(logger)) {}

core::Result<SessionSnapshot> PersonaInteraction::EnsureSession(
    CreateSessionRequest request) {
    if (!request.session_id.empty()) {
        auto existing = sessions_.GetSessionSnapshot(request.session_id);
        if (existing.ok()) {
            const auto owner_status = ValidateOwner(existing.value(), request.user_uuid);
            if (!owner_status.ok()) {
                return owner_status;
            }
            if (!request.persona_id.empty() &&
                existing.value().persona_id != request.persona_id) {
                return core::Status::Error(core::ErrorCode::AlreadyExists,
                                           "session_id belongs to another persona");
            }
            return std::move(existing).value();
        }
        if (existing.status().code() != core::ErrorCode::NotFound) {
            return existing.status();
        }
    }
    return CreateSession(std::move(request));
}

core::Result<SessionSnapshot> PersonaInteraction::CreateSession(
    CreateSessionRequest request) {
    if (request.trace_id.empty()) {
        request.trace_id = core::GenerateTraceId();
    }
    auto result = sessions_.CreateSession(std::move(request));
    if (!result.ok()) {
        logger_.error("[persona-interaction] create session failed: {}",
                      result.status().message());
    }
    return result;
}

core::Result<SessionSnapshot> PersonaInteraction::GetSession(
    PersonaSessionQuery query) const {
    if (query.session_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "session_id is required");
    }
    auto snapshot = sessions_.GetSessionSnapshot(query.session_id);
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    const auto owner_status = ValidateOwner(snapshot.value(), query.trusted_user_uuid);
    if (!owner_status.ok()) {
        return owner_status;
    }
    return std::move(snapshot).value();
}

core::Result<SessionSnapshot> PersonaInteraction::CloseSession(
    ClosePersonaSessionRequest request) {
    auto snapshot = GetSession(PersonaSessionQuery{
        request.session_id,
        request.trace_id,
        request.trusted_user_uuid,
    });
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    auto status = sessions_.CloseSession(request.session_id, request.trace_id);
    if (!status.ok()) {
        logger_.error("[persona-interaction] close session={} failed: {}",
                      request.session_id, status.message());
        return status;
    }
    auto closed = std::move(snapshot).value();
    closed.status = SessionStatus::Closed;
    closed.close_reason = request.reason.empty() ? "client_close" : std::move(request.reason);
    closed.last_trace_id = std::move(request.trace_id);
    return closed;
}

core::Status PersonaInteraction::HoldReclamationUntil(
    PersonaSessionReclamationRequest request) {
    auto snapshot = GetSession(PersonaSessionQuery{
        request.session_id,
        request.trace_id,
        request.trusted_user_uuid,
    });
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    return sessions_.HoldReclamationUntil(
        request.session_id,
        request.retain_until,
        request.trace_id);
}

core::Status PersonaInteraction::ReleaseReclamationHold(PersonaSessionQuery query) {
    auto snapshot = GetSession(query);
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    return sessions_.ReleaseReclamationHold(query.session_id, query.trace_id);
}

core::Status PersonaInteraction::SubmitTurn(PersonaTurnRequest request,
                                            TurnCallback callback) {
    if (!callback) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "turn callback is required");
    }
    if (request.turn.trace_id.empty()) {
        request.turn.trace_id = core::GenerateTraceId();
    }
    const auto snapshot = GetSession(PersonaSessionQuery{
        request.turn.session_id,
        request.turn.trace_id,
        request.trusted_user_uuid,
    });
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    if (snapshot.value().status != SessionStatus::Active) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "session is not active");
    }
    auto status = runtime_.SubmitChat(std::move(request.turn), std::move(callback));
    if (!status.ok()) {
        logger_.error("[persona-interaction] submit turn failed: {}", status.message());
    }
    return status;
}

core::Status PersonaInteraction::CancelTurn(PersonaSessionQuery query) {
    auto snapshot = GetSession(query);
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    // query.trace_id 属于取消请求的日志链路，不作为原 Turn 的匹配条件。
    auto status = runtime_.CancelAsyncTurn(query.session_id);
    if (!status.ok() && status.code() != core::ErrorCode::NotFound) {
        logger_.warn("[persona-interaction] cancel turn session={} failed: {}",
                     query.session_id, status.message());
    }
    return status;
}

PersonaInteractionSnapshot PersonaInteraction::SystemSnapshot() const {
    return PersonaInteractionSnapshot{
        sessions_.SessionCount(),
        sessions_.PoolStats(),
    };
}

core::Status PersonaInteraction::ValidateOwner(
    const SessionSnapshot& snapshot,
    std::string_view trusted_user_uuid) const {
    if (!trusted_user_uuid.empty() && snapshot.user_uuid != trusted_user_uuid) {
        return core::Status::Error(core::ErrorCode::PermissionDenied,
                                   "session does not belong to authenticated account");
    }
    return core::Status::Ok();
}

}
