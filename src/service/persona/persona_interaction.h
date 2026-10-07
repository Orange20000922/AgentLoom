#pragma once

#include "logger_adapter.h"
#include "persona_runtime.h"
#include "result.h"
#include "session_manager.h"

#include <functional>
#include <string>
#include <string_view>

namespace agent::service::persona {

struct PersonaSessionQuery {
    std::string session_id;
    std::string trace_id;
    std::string trusted_user_uuid;
};

struct ClosePersonaSessionRequest {
    std::string session_id;
    std::string trace_id;
    std::string trusted_user_uuid;
    std::string reason = "client_close";
};

struct PersonaSessionReclamationRequest {
    std::string session_id;
    std::string trace_id;
    std::string trusted_user_uuid;
    std::chrono::steady_clock::time_point retain_until{};
};

struct PersonaTurnRequest {
    ChatRequest turn;
    std::string trusted_user_uuid;
};

struct PersonaInteractionSnapshot {
    std::size_t session_count = 0;
    SessionThreadPoolStats pools;
};

class IPersonaInteraction {
public:
    using TurnCallback = std::function<void(core::Result<ChatResponse>)>;

    virtual ~IPersonaInteraction() = default;

    virtual core::Result<SessionSnapshot> EnsureSession(CreateSessionRequest request) = 0;
    virtual core::Result<SessionSnapshot> CreateSession(CreateSessionRequest request) = 0;
    virtual core::Result<SessionSnapshot> GetSession(PersonaSessionQuery query) const = 0;
    virtual core::Result<SessionSnapshot> CloseSession(ClosePersonaSessionRequest request) = 0;
    virtual core::Status HoldReclamationUntil(PersonaSessionReclamationRequest request) = 0;
    virtual core::Status ReleaseReclamationHold(PersonaSessionQuery query) = 0;
    virtual core::Status SubmitTurn(PersonaTurnRequest request, TurnCallback callback) = 0;
    /// 请求取消当前异步 Turn；trusted_user_uuid 校验 owner，trace_id 标识取消请求。
    /// 返回 OK 表示请求已接纳，原 Turn callback 在 Provider 收口后恰好返回一次 Cancelled。
    virtual core::Status CancelTurn(PersonaSessionQuery query) = 0;
    virtual PersonaInteractionSnapshot SystemSnapshot() const = 0;
};

/// SessionManager 与 PersonaRuntime 的 transport-neutral 用例门面，不持有第二份 Session 状态。
class PersonaInteraction final : public IPersonaInteraction {
public:
    PersonaInteraction(SessionManager& sessions,
                       PersonaRuntime& runtime,
                       core::LoggerAdapter logger =
                           core::LoggerAdapter::ForModule("persona-interaction"));

    core::Result<SessionSnapshot> EnsureSession(CreateSessionRequest request) override;
    core::Result<SessionSnapshot> CreateSession(CreateSessionRequest request) override;
    core::Result<SessionSnapshot> GetSession(PersonaSessionQuery query) const override;
    core::Result<SessionSnapshot> CloseSession(ClosePersonaSessionRequest request) override;
    core::Status HoldReclamationUntil(PersonaSessionReclamationRequest request) override;
    core::Status ReleaseReclamationHold(PersonaSessionQuery query) override;
    core::Status SubmitTurn(PersonaTurnRequest request, TurnCallback callback) override;
    core::Status CancelTurn(PersonaSessionQuery query) override;
    PersonaInteractionSnapshot SystemSnapshot() const override;

private:
    core::Status ValidateOwner(const SessionSnapshot& snapshot,
                               std::string_view trusted_user_uuid) const;

    SessionManager& sessions_;
    PersonaRuntime& runtime_;
    core::LoggerAdapter logger_;
};

}
