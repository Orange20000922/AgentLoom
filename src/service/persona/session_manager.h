#pragma once

#include "logger_adapter.h"
#include "persona_algorithm.h"
#include "result.h"
#include "thread_pool.h"
#include "trace_context.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agent::service::persona {

enum class SessionStatus {
    Creating,
    Active,
    Disconnected,
    Closing,
    Closed,
};

struct SessionMetrics {
    std::uint64_t turn_count = 0;
    std::uint64_t request_count = 0;
    std::uint64_t failed_request_count = 0;
    std::chrono::milliseconds total_latency{0};
    std::chrono::milliseconds last_latency{0};
};

struct ConversationTurn {
    std::string user_input;
    std::string emotion = "neutral";
    double intensity = 0.0;
    std::string behavior;
    std::string tone;
    std::string response;
    std::string timestamp;
    std::string context_id;
    std::string persona_id;
    std::optional<double> valence;
    std::optional<double> arousal;
    // 稳定回合标识，用于 L0 与近期历史跨来源去重；旧持久化记录可为空并回退到内容匹配。
    std::string turn_id;
};

struct SessionOptions {
    std::chrono::minutes idle_timeout{15};
    std::size_t max_recent_turns = 20;
    std::size_t max_active_sessions = 1024;
};

struct SessionAdmissionRequest {
    std::string tenant_id = "default";
    std::string user_uuid;
    std::size_t requested_sessions = 1;
    std::string reason;
};

/// 账户、租户或权限级 session 配额的扩展点。
/// 全局 active session 硬上限由 SessionManager 固有保证，本策略暂不参与默认创建流程。
class ISessionAdmissionPolicy {
public:
    virtual ~ISessionAdmissionPolicy() = default;

    virtual core::Status Check(const SessionAdmissionRequest& request) const = 0;
};

struct CreateSessionRequest {
    std::string tenant_id = "default";
    std::string user_uuid;
    std::string persona_id;
    std::string session_id;
    std::string trace_id;
    PersonalityConfig personality;
    std::optional<EmotionPromptConfig> emotion_prompt_config;
    EmotionStateConfig emotion_state_config;
    bool time_awareness = true;
};

struct SessionSnapshot {
    std::string session_id;
    std::string tenant_id = "default";
    std::string user_uuid;
    std::string persona_id;
    std::string last_trace_id;
    SessionStatus status = SessionStatus::Active;
    std::string close_reason;
    std::chrono::steady_clock::time_point created_at{};
    std::chrono::steady_clock::time_point last_active{};
    std::size_t recent_turn_count = 0;
    EmotionState emotion_state;
    SessionMetrics metrics;
};

struct DispatchOptions {
    std::string session_id;
    std::string trace_id;
    std::string span_id;
    std::string user_uuid;
    std::string module;
    std::string operation;
};

struct SessionThreadPoolStats {
    core::ThreadPoolStats compute;
    core::ThreadPoolStats io;
    /// 完整 Persona Turn 的执行池；未注入专用池时与 io 指向同一组统计。
    core::ThreadPoolStats llm;
};

struct SessionState {
    std::string session_id;
    std::string tenant_id = "default";
    std::string user_uuid;
    std::string persona_id;
    std::string last_trace_id;
    SessionStatus status = SessionStatus::Creating;
    std::string close_reason;
    std::chrono::steady_clock::time_point created_at{};
    std::chrono::steady_clock::time_point last_active{};
    std::optional<std::chrono::steady_clock::time_point> reclamation_hold_until;
    std::deque<ConversationTurn> recent_history;
    std::shared_ptr<const PersonalityConfig> personality;
    EmotionStateTracker emotion_state;
    std::shared_ptr<const PromptBuilder> prompt_builder;
    SessionMetrics metrics;
    std::size_t max_recent_turns = 20;
};

struct SessionTurnSnapshot {
    std::uint64_t sequence = 0;
    SessionState state;
};

struct SessionTurnCommit {
    std::string trace_id;
    ConversationTurn turn;
    EmotionStateTracker emotion_state;
    std::chrono::milliseconds latency{0};
};

struct SessionTurnReceipt {
    std::uint64_t sequence = 0;
    std::uint64_t turn_index = 0;
};

class ISessionManager {
public:
    virtual ~ISessionManager() = default;

    /// 创建并注册 session；request 可按值移动较大的 personality 配置。
    virtual core::Result<SessionSnapshot> CreateSession(CreateSessionRequest request) = 0;
    /// 获取线程安全快照，不暴露内部可变 SessionState。
    virtual core::Result<SessionSnapshot> GetSessionSnapshot(std::string_view session_id) const = 0;
    /// 幂等关闭 session，并触发已注册的关闭回调。
    virtual core::Status CloseSession(std::string_view session_id, std::string_view trace_id = {}) = 0;
    /// 更新最后活动时间；不存在的 session 返回失败 Status。
    virtual core::Status TouchSession(std::string_view session_id, std::string_view trace_id = {}) = 0;
    /// 在指定单调时钟时间前阻止 idle cleanup 回收；不改变 SessionStatus。
    virtual core::Status HoldReclamationUntil(
        std::string_view session_id,
        std::chrono::steady_clock::time_point retain_until,
        std::string_view trace_id = {}) = 0;
    /// 解除 idle cleanup hold；重复解除保持幂等。
    virtual core::Status ReleaseReclamationHold(
        std::string_view session_id,
        std::string_view trace_id = {}) = 0;
    virtual std::vector<SessionSnapshot> CleanupExpired() = 0;
    virtual std::size_t SessionCount() const = 0;
};

class SessionManager final : public ISessionManager {
public:
    using SessionTask = std::function<core::Status(SessionState&, core::ThreadPoolContext&)>;
    using SessionTurnTask = std::function<core::Status(
        const SessionTurnSnapshot&,
        SessionTurnCommit&,
        core::ThreadPoolContext&)>;
    using SessionTurnCompletion = std::function<void(core::Result<SessionTurnReceipt>)>;
    using SessionTurnAsyncFinish = std::function<void(core::Result<SessionTurnCommit>)>;
    using SessionTurnAsyncTask = std::function<core::Status(
        const SessionTurnSnapshot&,
        core::ThreadPoolContext&,
        SessionTurnAsyncFinish)>;
    using SessionClosedCallback = std::function<void(const SessionSnapshot&)>;

    /// @param compute_pool 借用的计算线程池，生命周期必须长于 SessionManager。
    /// @param io_pool 借用的 IO 线程池，生命周期必须长于 SessionManager。
    /// @param turn_pool 可选的完整 Persona Turn 池，生命周期必须长于 SessionManager；为空时使用 io_pool。
    /// @param options 空闲超时和最近回合容量。
    SessionManager(core::ThreadPool& compute_pool,
                   core::ThreadPool& io_pool,
                   SessionOptions options = {},
                   core::LoggerAdapter logger = core::LoggerAdapter::ForModule("service"),
                   core::ThreadPool* turn_pool = nullptr);
    ~SessionManager();

    core::Result<SessionSnapshot> CreateSession(CreateSessionRequest request) override;
    core::Result<SessionSnapshot> GetSessionSnapshot(std::string_view session_id) const override;
    core::Status CloseSession(std::string_view session_id, std::string_view trace_id = {}) override;
    core::Status TouchSession(std::string_view session_id, std::string_view trace_id = {}) override;
    core::Status HoldReclamationUntil(
        std::string_view session_id,
        std::chrono::steady_clock::time_point retain_until,
        std::string_view trace_id = {}) override;
    core::Status ReleaseReclamationHold(
        std::string_view session_id,
        std::string_view trace_id = {}) override;
    std::vector<SessionSnapshot> CleanupExpired() override;
    std::size_t SessionCount() const override;
    /// 阻止新 admission，并将所有 Session 置为 Closing；已在途 Turn 完成后再通知关闭。
    void Shutdown();

    core::Status AddTurn(std::string_view session_id,
                         ConversationTurn turn,
                         std::string_view trace_id = {});
    core::Status RecordRequestMetrics(std::string_view session_id,
                                      std::chrono::milliseconds latency,
                                      bool success,
                                      std::string_view trace_id = {});

    /// 向 compute pool 提交持有目标 session 锁的任务。
    core::Status SubmitCompute(DispatchOptions options, SessionTask task);
    /// 向 IO pool 提交持有目标 session 锁的任务。
    core::Status SubmitIo(DispatchOptions options, SessionTask task);
    /// 仅供 SessionTask 在已持有目标 SessionSlot 锁时转投 IO，避免 admission 重复加锁。
    core::Status SubmitIoFromSessionTask(const SessionState& locked_session,
                                         DispatchOptions options,
                                         SessionTask task);
    /// 以单 Session lane 串行执行完整 Turn；业务执行锁外运行，仅 snapshot/commit 持短锁。
    core::Status SubmitTurn(DispatchOptions options,
                            SessionTurnTask task,
                            SessionTurnCompletion completion);
    /// 真异步 Turn：task 发起操作后返回，finish 可在任意线程调用一次；
    /// 仅支持能将 concurrency_key 串行保持到 deferred Complete 的 scheduler。
    core::Status SubmitTurnAsync(DispatchOptions options,
                                 SessionTurnAsyncTask task,
                                 SessionTurnCompletion completion);
    SessionThreadPoolStats PoolStats() const;
    /// 设置 session 关闭通知；callback 在内部资源移除后调用，不应执行长时间阻塞操作。
    void SetSessionClosedCallback(SessionClosedCallback callback);

private:
    struct TrustedDispatchIdentity {
        std::string tenant_id;
        std::string user_uuid;
    };

    struct SessionSlot {
        mutable std::mutex mutex;
        // 仅用于同一 Session 的 Turn 顺序，不保护 SessionState；慢 IO 不会阻塞 Snapshot/Close。
        mutable std::mutex turn_gate;
        SessionState state;
        std::uint64_t next_turn_sequence = 1;
        std::size_t outstanding_turns = 0;
    };

    class TurnOperation;
    struct ResourceAccounting {
        std::atomic<std::size_t> resident_sessions{0};
    };

    core::Status Submit(core::ThreadPool& pool,
                        std::string_view pool_role,
                        DispatchOptions options,
                        SessionTask task,
                        std::optional<TrustedDispatchIdentity> trusted_identity = std::nullopt);
    core::Result<std::shared_ptr<SessionSlot>> FindSlot(std::string_view session_id) const;
    void NotifySessionClosed(const SessionSnapshot& snapshot) const;
    static SessionSnapshot SnapshotLocked(const SessionState& state);
    core::TraceContext MakeTrace(DispatchOptions options) const;
    std::string MakeSessionId();

    core::ThreadPool& compute_pool_;
    core::ThreadPool& io_pool_;
    // 可选的完整 Turn 执行池；为空时兼容使用 IO pool。
    core::ThreadPool* turn_pool_ = nullptr;
    SessionOptions options_;
    core::LoggerAdapter logger_;
    SessionClosedCallback session_closed_callback_;
    mutable std::shared_mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<SessionSlot>> sessions_;
    std::size_t active_session_count_ = 0;
    std::shared_ptr<ResourceAccounting> resource_accounting_ =
        std::make_shared<ResourceAccounting>();
};

} // namespace agent::service::persona
