#include "session_manager.h"

#include <atomic>
#include <sstream>
#include <utility>

namespace agent::service::persona {
namespace {

std::string NonEmptyOrGeneratedTrace(std::string trace_id) {
    if (!trace_id.empty()) {
        return trace_id;
    }
    if (auto current = core::CurrentTraceId(); current != "-") {
        return std::string(current);
    }
    return core::GenerateTraceId();
}

std::string TaskName(std::string_view role, const DispatchOptions& options) {
    std::string name;
    name.reserve(role.size() + options.module.size() + options.operation.size() + 3);
    name.append(role);
    name.push_back(':');
    name.append(options.module.empty() ? "session" : options.module);
    name.push_back(':');
    name.append(options.operation.empty() ? "task" : options.operation);
    return name;
}

} // namespace

class SessionManager::TurnOperation final {
public:
    TurnOperation(std::shared_ptr<SessionSlot> slot,
                  SessionTurnCompletion completion,
                  SessionClosedCallback closed_callback,
                  std::shared_ptr<ResourceAccounting> resource_accounting,
                  core::LoggerAdapter logger,
                  std::string trace_id = {})
        : slot_(std::move(slot)),
          completion_(std::move(completion)),
          closed_callback_(std::move(closed_callback)),
          resource_accounting_(std::move(resource_accounting)),
          logger_(std::move(logger)),
          trace_id_(std::move(trace_id)) {}

    // 排队取消与 worker 启动互斥：同一 CAS 决定 Turn 被丢弃还是进入异步执行流。
    bool TryStart() noexcept {
        auto expected = Phase::Queued;
        return phase_.compare_exchange_strong(expected, Phase::Started, std::memory_order_acq_rel);
    }
    bool TryCancelQueued() noexcept {
        auto expected = Phase::Queued;
        return phase_.compare_exchange_strong(expected, Phase::Cancelled, std::memory_order_acq_rel);
    }
    bool queued() const noexcept { return phase_.load(std::memory_order_acquire) == Phase::Queued; }
    const std::string& trace_id() const noexcept { return trace_id_; }

    ~TurnOperation() {
        if (!finished_.load(std::memory_order_acquire)) {
            Finish(core::Status::Error(core::ErrorCode::Unavailable,
                                       "session turn was discarded during shutdown"), true);
        }
    }

    void Finish(core::Result<SessionTurnReceipt> result, bool notify_completion) noexcept {
        if (finished_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        std::optional<SessionSnapshot> closed_snapshot;
        {
            std::lock_guard lock(slot_->mutex);
            if (slot_->outstanding_turns > 0) {
                --slot_->outstanding_turns;
            }
            if (slot_->state.status == SessionStatus::Closing &&
                slot_->outstanding_turns == 0) {
                slot_->state.status = SessionStatus::Closed;
                closed_snapshot = SessionManager::SnapshotLocked(slot_->state);
            }
        }
        if (notify_completion) {
            try {
                completion_(std::move(result));
            } catch (const std::exception& exception) {
                logger_.error("[session] turn completion exception: {}", exception.what());
            } catch (...) {
                logger_.error("[session] turn completion exception: unknown");
            }
        }
        if (closed_snapshot) {
            resource_accounting_->resident_sessions.fetch_sub(1, std::memory_order_acq_rel);
            if (closed_callback_) {
                closed_callback_(*closed_snapshot);
            }
            logger_.info("[trace={}] [session] closed session={} user={} persona={}",
                         closed_snapshot->last_trace_id.empty() ? "-" : closed_snapshot->last_trace_id,
                         closed_snapshot->session_id,
                         closed_snapshot->user_uuid,
                         closed_snapshot->persona_id);
        }
    }

private:
    enum class Phase { Queued, Started, Cancelled };

    std::shared_ptr<SessionSlot> slot_;
    SessionTurnCompletion completion_;
    SessionClosedCallback closed_callback_;
    std::shared_ptr<ResourceAccounting> resource_accounting_;
    core::LoggerAdapter logger_;
    std::string trace_id_;
    std::atomic<Phase> phase_{Phase::Queued};
    std::atomic<bool> finished_{false};
};

SessionManager::SessionManager(core::ThreadPool& compute_pool,
                               core::ThreadPool& io_pool,
                               SessionOptions options,
                               core::LoggerAdapter logger,
                               core::ThreadPool* turn_pool)
    : compute_pool_(compute_pool),
      io_pool_(io_pool),
      turn_pool_(turn_pool),
      options_(options),
      logger_(std::move(logger)) {}

SessionManager::~SessionManager() {
    Shutdown();
}

core::Result<SessionSnapshot> SessionManager::CreateSession(CreateSessionRequest request) {
    if (request.user_uuid.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "user_uuid is required");
    }
    if (request.persona_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "persona_id is required");
    }
    if (request.personality.name.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "personality.name is required");
    }

    core::TraceContext trace;
    trace.trace_id = NonEmptyOrGeneratedTrace(std::move(request.trace_id));
    trace.user_uuid = request.user_uuid;
    core::TraceScope trace_scope(trace);

    auto slot = std::make_shared<SessionSlot>();
    {
        std::lock_guard lock(slot->mutex);
        auto now = std::chrono::steady_clock::now();
        slot->state.session_id = request.session_id.empty() ? MakeSessionId() : std::move(request.session_id);
        slot->state.tenant_id = request.tenant_id.empty() ? "default" : std::move(request.tenant_id);
        slot->state.user_uuid = std::move(request.user_uuid);
        slot->state.persona_id = std::move(request.persona_id);
        slot->state.last_trace_id = trace.trace_id;
        slot->state.status = SessionStatus::Active;
        slot->state.created_at = now;
        slot->state.last_active = now;
        slot->state.emotion_state = EmotionStateTracker(request.emotion_state_config);
        slot->state.personality = std::make_shared<const PersonalityConfig>(request.personality);
        slot->state.prompt_builder = std::make_shared<const PromptBuilder>(
            std::move(request.personality),
            std::move(request.emotion_prompt_config),
            request.time_awareness);
        slot->state.max_recent_turns = options_.max_recent_turns;
    }

    SessionSnapshot snapshot;
    {
        std::unique_lock sessions_lock(sessions_mutex_);
        const auto id = slot->state.session_id;
        if (sessions_.find(id) != sessions_.end()) {
            return core::Status::Error(core::ErrorCode::AlreadyExists, "session already exists");
        }
        const auto resident_sessions =
            resource_accounting_->resident_sessions.load(std::memory_order_acquire);
        if (resident_sessions >= options_.max_active_sessions) {
            logger_.warn("[trace={}] [session] active session limit reached active={} limit={}",
                         core::CurrentTraceId(),
                         resident_sessions,
                         options_.max_active_sessions);
            return core::Status::Error(core::ErrorCode::ResourceExhausted,
                                       "active session limit reached");
        }
        {
            std::lock_guard slot_lock(slot->mutex);
            snapshot = SnapshotLocked(slot->state);
        }
        sessions_[id] = std::move(slot);
        ++active_session_count_;
        resource_accounting_->resident_sessions.fetch_add(1, std::memory_order_release);
    }

    logger_.info("[trace={}] [session] created session={} user={} persona={}",
                 core::CurrentTraceId(),
                 snapshot.session_id,
                 snapshot.user_uuid,
                 snapshot.persona_id);
    return snapshot;
}

core::Result<SessionSnapshot> SessionManager::GetSessionSnapshot(std::string_view session_id) const {
    auto slot = FindSlot(session_id);
    if (!slot.ok()) {
        return slot.status();
    }
    std::lock_guard lock(slot.value()->mutex);
    return SnapshotLocked(slot.value()->state);
}

core::Status SessionManager::CloseSession(std::string_view session_id, std::string_view trace_id) {
    core::TraceContext trace;
    trace.trace_id = NonEmptyOrGeneratedTrace(std::string(trace_id));
    core::TraceScope trace_scope(trace);

    std::shared_ptr<SessionSlot> removed;
    std::optional<SessionSnapshot> closed_snapshot;
    {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(std::string(session_id));
        if (it == sessions_.end()) {
            return core::Status::Error(core::ErrorCode::NotFound, "session not found");
        }
        removed = std::move(it->second);
        {
            std::lock_guard slot_lock(removed->mutex);
            if (removed->state.status == SessionStatus::Closing ||
                removed->state.status == SessionStatus::Closed) {
                return core::Status::Ok();
            }
            removed->state.status = SessionStatus::Closing;
            removed->state.close_reason = "client_close";
            removed->state.last_trace_id = trace.trace_id;
            if (active_session_count_ > 0) {
                --active_session_count_;
            }
            if (removed->outstanding_turns == 0) {
                removed->state.status = SessionStatus::Closed;
                closed_snapshot = SnapshotLocked(removed->state);
            }
        }
        sessions_.erase(it);
    }
    if (closed_snapshot) {
        resource_accounting_->resident_sessions.fetch_sub(1, std::memory_order_acq_rel);
        NotifySessionClosed(*closed_snapshot);
    } else {
        logger_.info("[trace={}] [session] closing session={} outstanding_turns={}",
                     core::CurrentTraceId(), session_id, removed->outstanding_turns);
    }
    return core::Status::Ok();
}

core::Status SessionManager::TouchSession(std::string_view session_id, std::string_view trace_id) {
    auto slot = FindSlot(session_id);
    if (!slot.ok()) {
        return slot.status();
    }
    const auto trace = NonEmptyOrGeneratedTrace(std::string(trace_id));
    {
        std::lock_guard lock(slot.value()->mutex);
        if (slot.value()->state.status != SessionStatus::Active) {
            return core::Status::Error(core::ErrorCode::FailedPrecondition, "session is not active");
        }
        slot.value()->state.last_active = std::chrono::steady_clock::now();
        slot.value()->state.last_trace_id = trace;
    }
    logger_.debug("[trace={}] [session] touched session={}", trace, session_id);
    return core::Status::Ok();
}

core::Status SessionManager::HoldReclamationUntil(
    std::string_view session_id,
    std::chrono::steady_clock::time_point retain_until,
    std::string_view trace_id) {
    if (retain_until <= std::chrono::steady_clock::now()) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "session reclamation hold must expire in the future");
    }
    auto slot = FindSlot(session_id);
    if (!slot.ok()) {
        return slot.status();
    }
    const auto trace = NonEmptyOrGeneratedTrace(std::string(trace_id));
    {
        std::lock_guard lock(slot.value()->mutex);
        if (slot.value()->state.status != SessionStatus::Active) {
            return core::Status::Error(
                core::ErrorCode::FailedPrecondition,
                "session is not active");
        }
        slot.value()->state.reclamation_hold_until = retain_until;
        slot.value()->state.last_trace_id = trace;
    }
    logger_.debug("[trace={}] [session] reclamation held session={}", trace, session_id);
    return core::Status::Ok();
}

core::Status SessionManager::ReleaseReclamationHold(
    std::string_view session_id,
    std::string_view trace_id) {
    auto slot = FindSlot(session_id);
    if (!slot.ok()) {
        return slot.status();
    }
    const auto trace = NonEmptyOrGeneratedTrace(std::string(trace_id));
    {
        std::lock_guard lock(slot.value()->mutex);
        if (slot.value()->state.status != SessionStatus::Active) {
            return core::Status::Error(
                core::ErrorCode::FailedPrecondition,
                "session is not active");
        }
        slot.value()->state.reclamation_hold_until.reset();
        slot.value()->state.last_trace_id = trace;
    }
    logger_.debug("[trace={}] [session] reclamation hold released session={}", trace, session_id);
    return core::Status::Ok();
}

std::vector<SessionSnapshot> SessionManager::CleanupExpired() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<SessionSlot>> expired;
    {
        std::unique_lock sessions_lock(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            bool remove = false;
            {
                std::lock_guard slot_lock(it->second->mutex);
                const auto& hold_until = it->second->state.reclamation_hold_until;
                const bool held = hold_until && now < *hold_until;
                remove = it->second->state.status == SessionStatus::Active && !held &&
                         now - it->second->state.last_active > options_.idle_timeout;
                if (remove) {
                    it->second->state.status = SessionStatus::Closing;
                    it->second->state.close_reason = "idle_timeout";
                }
            }
            if (remove) {
                expired.push_back(std::move(it->second));
                if (active_session_count_ > 0) {
                    --active_session_count_;
                }
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<SessionSnapshot> snapshots;
    snapshots.reserve(expired.size());
    for (const auto& slot : expired) {
        std::optional<SessionSnapshot> snapshot;
        {
            std::lock_guard lock(slot->mutex);
            if (slot->outstanding_turns == 0) {
                slot->state.status = SessionStatus::Closed;
                snapshot = SnapshotLocked(slot->state);
            }
        }
        if (snapshot) {
            resource_accounting_->resident_sessions.fetch_sub(1, std::memory_order_acq_rel);
            snapshots.push_back(*snapshot);
            NotifySessionClosed(*snapshot);
        } else {
            logger_.info("[trace={}] [session] expiring session={} outstanding_turns={}",
                         slot->state.last_trace_id.empty() ? "-" : slot->state.last_trace_id,
                         slot->state.session_id,
                         slot->outstanding_turns);
        }
    }
    return snapshots;
}

std::size_t SessionManager::SessionCount() const {
    std::shared_lock lock(sessions_mutex_);
    return active_session_count_;
}

void SessionManager::Shutdown() {
    std::vector<std::shared_ptr<SessionSlot>> closing;
    {
        std::unique_lock lock(sessions_mutex_);
        closing.reserve(sessions_.size());
        for (auto& [_, slot] : sessions_) {
            closing.push_back(std::move(slot));
        }
        sessions_.clear();
        active_session_count_ = 0;
    }

    std::vector<SessionSnapshot> closed;
    for (const auto& slot : closing) {
        std::lock_guard lock(slot->mutex);
        if (slot->state.status == SessionStatus::Closed) {
            continue;
        }
        slot->state.status = SessionStatus::Closing;
        if (slot->state.close_reason.empty()) {
            slot->state.close_reason = "manager_shutdown";
        }
        if (slot->outstanding_turns == 0) {
            slot->state.status = SessionStatus::Closed;
            closed.push_back(SnapshotLocked(slot->state));
        }
    }
    for (const auto& snapshot : closed) {
        resource_accounting_->resident_sessions.fetch_sub(1, std::memory_order_acq_rel);
        NotifySessionClosed(snapshot);
    }
}

core::Status SessionManager::AddTurn(std::string_view session_id,
                                     ConversationTurn turn,
                                     std::string_view trace_id) {
    auto slot = FindSlot(session_id);
    if (!slot.ok()) {
        return slot.status();
    }

    std::lock_guard lock(slot.value()->mutex);
    auto& state = slot.value()->state;
    if (state.status != SessionStatus::Active) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "session is not active");
    }
    state.last_active = std::chrono::steady_clock::now();
    state.last_trace_id = NonEmptyOrGeneratedTrace(std::string(trace_id));
    state.recent_history.push_back(std::move(turn));
    while (state.recent_history.size() > state.max_recent_turns) {
        state.recent_history.pop_front();
    }
    return core::Status::Ok();
}

core::Status SessionManager::RecordRequestMetrics(std::string_view session_id,
                                                  std::chrono::milliseconds latency,
                                                  bool success,
                                                  std::string_view trace_id) {
    auto slot = FindSlot(session_id);
    if (!slot.ok()) {
        return slot.status();
    }
    std::lock_guard lock(slot.value()->mutex);
    auto& metrics = slot.value()->state.metrics;
    ++metrics.request_count;
    if (!success) {
        ++metrics.failed_request_count;
    }
    metrics.last_latency = latency;
    metrics.total_latency += latency;
    if (!trace_id.empty()) {
        slot.value()->state.last_trace_id = std::string(trace_id);
    }
    return core::Status::Ok();
}

core::Status SessionManager::SubmitCompute(DispatchOptions options, SessionTask task) {
    return Submit(compute_pool_, "compute", std::move(options), std::move(task));
}

core::Status SessionManager::SubmitIo(DispatchOptions options, SessionTask task) {
    return Submit(io_pool_, "io", std::move(options), std::move(task));
}

core::Status SessionManager::SubmitIoFromSessionTask(const SessionState& locked_session,
                                                     DispatchOptions options,
                                                     SessionTask task) {
    if (options.session_id != locked_session.session_id) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "dispatch session_id does not match locked session");
    }
    if (locked_session.status != SessionStatus::Active) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "session is not active");
    }
    TrustedDispatchIdentity identity{
        locked_session.tenant_id,
        locked_session.user_uuid,
    };
    return Submit(io_pool_,
                  "io",
                  std::move(options),
                  std::move(task),
                  std::move(identity));
}

core::Status SessionManager::SubmitTurn(DispatchOptions options,
                                        SessionTurnTask task,
                                        SessionTurnCompletion completion) {
    if (!task || !completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "session turn task and completion are required");
    }
    if (options.session_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "session_id is required");
    }

    auto slot_result = FindSlot(options.session_id);
    if (!slot_result.ok()) {
        return slot_result.status();
    }
    auto slot = std::move(slot_result).value();
    std::string tenant_id;
    std::string user_uuid;
    {
        std::lock_guard lock(slot->mutex);
        if (slot->state.status != SessionStatus::Active) {
            return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                       "session is not active");
        }
        tenant_id = slot->state.tenant_id;
        user_uuid = slot->state.user_uuid;
        ++slot->outstanding_turns;
    }

    const auto session_id = options.session_id;
    const auto trace = MakeTrace(options);
    const auto trace_id = trace.trace_id;
    const auto module = options.module.empty() ? std::string("session") : options.module;
    const auto operation = options.operation.empty() ? std::string("turn") : options.operation;
    core::ThreadPoolTaskMetadata metadata;
    metadata.concurrency_key = session_id;
    metadata.tenant_key = tenant_id;
    if (!user_uuid.empty()) {
        metadata.fairness_key = tenant_id + ":" + user_uuid;
    }
    metadata.execution_id = options.span_id;

    auto operation_state = std::make_shared<TurnOperation>(
        slot,
        std::move(completion),
        session_closed_callback_,
        resource_accounting_,
        logger_);
    auto& execution_pool = turn_pool_ ? *turn_pool_ : io_pool_;
    auto status = execution_pool.Submit(
        [this, slot, operation_state, session_id, trace_id, user_uuid, module, operation,
         task = std::move(task)](
            core::ThreadPoolContext& context) mutable -> core::Status {
            core::TraceContext task_trace;
            task_trace.trace_id = trace_id;
            task_trace.user_uuid = user_uuid;
            core::TraceScope trace_scope(task_trace);

            // 外部 pool 可能是普通 FIFO；manager 自身 gate 保证跨 pool/调度器的完整 Turn 保序。
            std::unique_lock turn_lock(slot->turn_gate);

            std::optional<SessionTurnSnapshot> snapshot;
            core::Status turn_status = core::Status::Ok();
            {
                std::lock_guard lock(slot->mutex);
                if (slot->state.status != SessionStatus::Active) {
                    turn_status = core::Status::Error(
                        core::ErrorCode::Cancelled,
                        "session closed before queued turn started");
                } else {
                    slot->state.last_active = std::chrono::steady_clock::now();
                    slot->state.last_trace_id = trace_id;
                    snapshot.emplace(SessionTurnSnapshot{
                        slot->next_turn_sequence++,
                        slot->state});
                }
            }

            SessionTurnCommit commit;
            SessionTurnReceipt receipt;
            if (snapshot) {
                turn_status = task(*snapshot, commit, context);
                if (turn_status.ok()) {
                    std::lock_guard lock(slot->mutex);
                    if (slot->state.status != SessionStatus::Active &&
                        slot->state.status != SessionStatus::Closing) {
                        turn_status = core::Status::Error(
                            core::ErrorCode::Cancelled,
                            "session turn commit was fenced");
                    } else {
                        slot->state.emotion_state = std::move(commit.emotion_state);
                        slot->state.last_active = std::chrono::steady_clock::now();
                        slot->state.last_trace_id = commit.trace_id.empty() ? trace_id : commit.trace_id;
                        slot->state.recent_history.push_back(std::move(commit.turn));
                        ++slot->state.metrics.turn_count;
                        ++slot->state.metrics.request_count;
                        slot->state.metrics.last_latency = commit.latency;
                        slot->state.metrics.total_latency += commit.latency;
                        while (slot->state.recent_history.size() > slot->state.max_recent_turns) {
                            slot->state.recent_history.pop_front();
                        }
                        receipt.sequence = snapshot->sequence;
                        receipt.turn_index = slot->state.metrics.turn_count;
                    }
                }
            }

            // Turn 状态已经完成 snapshot/commit 后释放 lane；callback/WS 背压不应阻塞下一 Turn。
            turn_lock.unlock();

            if (!turn_status.ok()) {
                logger_.warn("[trace={}] [session] turn failed module={} op={} session={} reason={}",
                             core::CurrentTraceId(), module, operation, session_id,
                             turn_status.message());
            }

            if (turn_status.ok()) {
                operation_state->Finish(receipt, true);
            } else {
                operation_state->Finish(turn_status, true);
            }
            return turn_status;
        },
        {},
        "io:" + module + ":" + operation,
        std::move(metadata));

    if (!status.ok()) {
        operation_state->Finish(status, true);
    }
    return status;
}

core::Status SessionManager::SubmitTurnAsync(DispatchOptions options,
                                             SessionTurnAsyncTask task,
                                             SessionTurnCompletion completion) {
    if (!task || !completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "async session turn task and completion are required");
    }
    if (options.session_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "session_id is required");
    }
    auto& execution_pool = turn_pool_ ? *turn_pool_ : io_pool_;
    if (!execution_pool.serializes_concurrency_key_until_completion()) {
        return core::Status::Error(
            core::ErrorCode::FailedPrecondition,
            "async session turns require a scheduler that serializes concurrency_key until completion");
    }

    auto slot_result = FindSlot(options.session_id);
    if (!slot_result.ok()) {
        return slot_result.status();
    }
    auto slot = std::move(slot_result).value();
    std::string tenant_id;
    std::string user_uuid;
    {
        std::lock_guard lock(slot->mutex);
        if (slot->state.status != SessionStatus::Active) {
            return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                       "session is not active");
        }
        tenant_id = slot->state.tenant_id;
        user_uuid = slot->state.user_uuid;
        ++slot->outstanding_turns;
    }

    const auto session_id = options.session_id;
    const auto trace = MakeTrace(options);
    const auto trace_id = trace.trace_id;
    const auto module = options.module.empty() ? std::string("session") : options.module;
    const auto operation = options.operation.empty() ? std::string("async_turn") : options.operation;
    core::ThreadPoolTaskMetadata metadata;
    metadata.concurrency_key = session_id;
    metadata.tenant_key = tenant_id;
    if (!user_uuid.empty()) {
        metadata.fairness_key = tenant_id + ":" + user_uuid;
    }
    metadata.execution_id = options.span_id;

    auto operation_state = std::make_shared<TurnOperation>(
        slot,
        std::move(completion),
        session_closed_callback_,
        resource_accounting_,
        logger_,
        trace_id);
    {
        // 仅登记弱引用；已启动或已释放的条目在登记时顺带清理，长度受 outstanding_turns 约束。
        std::lock_guard lock(slot->mutex);
        std::erase_if(slot->queued_turns, [](const auto& weak) {
            auto queued = weak.lock();
            return !queued || !queued->queued();
        });
        slot->queued_turns.push_back(operation_state);
    }
    auto status = execution_pool.Submit(
        [this, slot, operation_state, session_id, trace_id, user_uuid, module, operation,
         task = std::move(task)](core::ThreadPoolContext& context) mutable -> core::Status {
            if (!operation_state->TryStart()) {
                // 排队期间已被取消并完成收尾：直接出队丢弃，不进入异步执行流。
                return core::Status::Error(core::ErrorCode::Cancelled, "queued async turn was cancelled");
            }
            core::TraceContext task_trace;
            task_trace.trace_id = trace_id;
            task_trace.user_uuid = user_uuid;
            core::TraceScope trace_scope(task_trace);

            std::optional<SessionTurnSnapshot> snapshot;
            {
                std::lock_guard lock(slot->mutex);
                if (slot->state.status == SessionStatus::Active) {
                    slot->state.last_active = std::chrono::steady_clock::now();
                    slot->state.last_trace_id = trace_id;
                    snapshot.emplace(SessionTurnSnapshot{
                        slot->next_turn_sequence++,
                        slot->state});
                }
            }
            if (!snapshot) {
                auto failure = core::Status::Error(
                    core::ErrorCode::Cancelled,
                    "session closed before queued async turn started");
                operation_state->Finish(failure, true);
                return failure;
            }

            auto deferred_result = context.DeferCompletion();
            if (!deferred_result.ok()) {
                operation_state->Finish(deferred_result.status(), true);
                return deferred_result.status();
            }

            struct AsyncCompletionState {
                ~AsyncCompletionState() {
                    if (!finished.exchange(true, std::memory_order_acq_rel)) {
                        auto status = core::Status::Error(
                            core::ErrorCode::Cancelled,
                            "async session turn finish callback was released");
                        deferred.Complete(status);
                        operation->Finish(status, true);
                    }
                }

                std::atomic<bool> finished{false};
                core::DeferredTaskCompletion deferred;
                std::shared_ptr<TurnOperation> operation;
            };
            auto completion_state = std::make_shared<AsyncCompletionState>();
            completion_state->deferred = std::move(deferred_result).value();
            completion_state->operation = operation_state;
            const auto sequence = snapshot->sequence;

            SessionTurnAsyncFinish finish =
                [this, slot, operation_state, completion_state, sequence, trace_id,
                 session_id, module, operation](core::Result<SessionTurnCommit> result) mutable {
                    if (completion_state->finished.exchange(true, std::memory_order_acq_rel)) {
                        return;
                    }

                    core::Status turn_status = result.ok()
                        ? core::Status::Ok()
                        : result.status();
                    SessionTurnReceipt receipt;
                    if (result.ok()) {
                        auto commit = std::move(result).value();
                        std::lock_guard lock(slot->mutex);
                        if (slot->state.status != SessionStatus::Active &&
                            slot->state.status != SessionStatus::Closing) {
                            turn_status = core::Status::Error(
                                core::ErrorCode::Cancelled,
                                "async session turn commit was fenced");
                        } else {
                            slot->state.emotion_state = std::move(commit.emotion_state);
                            slot->state.last_active = std::chrono::steady_clock::now();
                            slot->state.last_trace_id = commit.trace_id.empty()
                                ? trace_id
                                : commit.trace_id;
                            slot->state.recent_history.push_back(std::move(commit.turn));
                            ++slot->state.metrics.turn_count;
                            ++slot->state.metrics.request_count;
                            slot->state.metrics.last_latency = commit.latency;
                            slot->state.metrics.total_latency += commit.latency;
                            while (slot->state.recent_history.size() > slot->state.max_recent_turns) {
                                slot->state.recent_history.pop_front();
                            }
                            receipt.sequence = sequence;
                            receipt.turn_index = slot->state.metrics.turn_count;
                        }
                    }

                    if (!turn_status.ok()) {
                        logger_.warn(
                            "[trace={}] [session] async turn failed module={} op={} session={} reason={}",
                            trace_id, module, operation, session_id, turn_status.message());
                    }

                    // 先释放 scheduler lane/quota，再执行可能带 WebSocket 背压的业务 callback。
                    completion_state->deferred.Complete(turn_status);
                    if (turn_status.ok()) {
                        operation_state->Finish(receipt, true);
                    } else {
                        operation_state->Finish(turn_status, true);
                    }
                };

            try {
                auto start_status = task(*snapshot, context, finish);
                if (!start_status.ok()) {
                    finish(start_status);
                }
            } catch (const std::exception& exception) {
                finish(core::Status::Error(core::ErrorCode::InternalError, exception.what()));
            } catch (...) {
                finish(core::Status::Error(core::ErrorCode::Unknown,
                                           "async session turn task threw an unknown exception"));
            }

            // 最终状态由 finish 负责；worker 此刻立即返回池中处理其他 Session。
            return core::Status::Ok();
        },
        {},
        "llm:" + module + ":" + operation,
        std::move(metadata));

    if (!status.ok()) {
        static_cast<void>(operation_state->TryCancelQueued());
        operation_state->Finish(status, true);
    }
    return status;
}

core::Status SessionManager::CancelQueuedTurn(std::string_view session_id, std::string_view trace_id) {
    if (session_id.empty() || trace_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "session_id and trace_id are required to cancel a queued turn");
    }
    auto slot_result = FindSlot(session_id);
    if (!slot_result.ok()) {
        return slot_result.status();
    }
    auto slot = std::move(slot_result).value();
    std::shared_ptr<TurnOperation> cancelled;
    {
        std::lock_guard lock(slot->mutex);
        for (auto it = slot->queued_turns.begin(); it != slot->queued_turns.end(); ++it) {
            auto queued = it->lock();
            if (queued && queued->trace_id() == trace_id && queued->TryCancelQueued()) {
                cancelled = std::move(queued);
                slot->queued_turns.erase(it);
                break;
            }
        }
    }
    if (!cancelled) {
        return core::Status::Error(core::ErrorCode::NotFound, "no queued turn matches trace_id");
    }
    // Finish 会重新获取 slot 锁并执行业务 callback，必须在锁外调用。
    cancelled->Finish(core::Status::Error(core::ErrorCode::Cancelled, "queued async turn was cancelled"), true);
    return core::Status::Ok();
}

SessionThreadPoolStats SessionManager::PoolStats() const {
    SessionThreadPoolStats stats;
    stats.compute = compute_pool_.Stats();
    stats.io = io_pool_.Stats();
    stats.llm = turn_pool_ ? turn_pool_->Stats() : stats.io;
    return stats;
}

void SessionManager::SetSessionClosedCallback(SessionClosedCallback callback) {
    session_closed_callback_ = std::move(callback);
}

core::Status SessionManager::Submit(core::ThreadPool& pool,
                                    std::string_view pool_role,
                                    DispatchOptions options,
                                    SessionTask task,
                                    std::optional<TrustedDispatchIdentity> trusted_identity) {
    if (!task) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "session task is empty");
    }
    if (options.session_id.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "session_id is required");
    }

    const std::string task_session_id = options.session_id;
    const std::string task_name = TaskName(pool_role, options);
    auto trace = MakeTrace(options);
    const std::string trace_id = trace.trace_id;
    std::string trusted_tenant_id;
    std::string trusted_user_uuid;
    if (trusted_identity) {
        trusted_tenant_id = std::move(trusted_identity->tenant_id);
        trusted_user_uuid = std::move(trusted_identity->user_uuid);
    } else {
        auto admission_slot = FindSlot(task_session_id);
        if (!admission_slot.ok()) {
            return admission_slot.status();
        }
        {
            std::lock_guard slot_lock(admission_slot.value()->mutex);
            if (admission_slot.value()->state.status != SessionStatus::Active) {
                return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                           "session is not active");
            }
            trusted_tenant_id = admission_slot.value()->state.tenant_id;
            trusted_user_uuid = admission_slot.value()->state.user_uuid;
        }
    }
    const std::string user_uuid = trusted_user_uuid;
    const std::string module = options.module.empty() ? "session" : options.module;
    const std::string operation = options.operation.empty() ? "task" : options.operation;
    core::ThreadPoolTaskMetadata scheduler_metadata;
    scheduler_metadata.concurrency_key = task_session_id;
    scheduler_metadata.tenant_key = trusted_tenant_id;
    if (!trusted_user_uuid.empty()) {
        scheduler_metadata.fairness_key = trusted_tenant_id + ":" + trusted_user_uuid;
    }
    scheduler_metadata.execution_id = options.span_id;

    core::TraceScope trace_scope(trace);
    logger_.info("[trace={}] [session] submit pool={} module={} op={} session={}",
                 core::CurrentTraceId(),
                 pool_role,
                 module,
                 operation,
                 task_session_id);

    auto status = pool.Submit(
        [this,
         task_session_id,
         task = std::move(task),
         pool_role = std::string(pool_role),
         module,
         operation,
         trace_id,
         user_uuid](core::ThreadPoolContext& context) mutable -> core::Status {
            core::TraceContext task_trace;
            task_trace.trace_id = trace_id;
            task_trace.user_uuid = user_uuid;
            core::TraceScope task_trace_scope(task_trace);

            auto slot = FindSlot(task_session_id);
            if (!slot.ok()) {
                logger_.warn("[trace={}] [session] dispatch dropped pool={} module={} op={} session={} reason={}",
                             core::CurrentTraceId(),
                             pool_role,
                             module,
                             operation,
                             task_session_id,
                             slot.status().message());
                return slot.status();
            }

            std::lock_guard lock(slot.value()->mutex);
            if (slot.value()->state.status != SessionStatus::Active) {
                return core::Status::Error(core::ErrorCode::FailedPrecondition, "session is not active");
            }
            slot.value()->state.last_active = std::chrono::steady_clock::now();
            slot.value()->state.last_trace_id = trace_id;

            logger_.debug("[trace={}] [session] dispatch start pool={} module={} op={} session={} worker={}",
                          core::CurrentTraceId(),
                          pool_role,
                          module,
                          operation,
                          task_session_id,
                          context.worker_index());
            auto task_status = task(slot.value()->state, context);
            if (!task_status.ok()) {
                logger_.warn("[trace={}] [session] dispatch failed pool={} module={} op={} session={} code={} reason={}",
                             core::CurrentTraceId(),
                             pool_role,
                             module,
                             operation,
                             task_session_id,
                             static_cast<int>(task_status.code()),
                             task_status.message());
            } else {
                logger_.debug("[trace={}] [session] dispatch done pool={} module={} op={} session={}",
                              core::CurrentTraceId(),
                              pool_role,
                              module,
                              operation,
                              task_session_id);
            }
            return task_status;
        },
        {},
        task_name,
        std::move(scheduler_metadata));

    if (!status.ok()) {
        logger_.warn("[trace={}] [session] submit rejected pool={} module={} op={} session={} reason={}",
                     core::CurrentTraceId(),
                     pool_role,
                     module,
                     operation,
                     task_session_id,
                     status.message());
    }
    return status;
}

core::Result<std::shared_ptr<SessionManager::SessionSlot>> SessionManager::FindSlot(std::string_view session_id) const {
    std::shared_lock lock(sessions_mutex_);
    auto it = sessions_.find(std::string(session_id));
    if (it == sessions_.end()) {
        return core::Status::Error(core::ErrorCode::NotFound, "session not found");
    }
    return it->second;
}

void SessionManager::NotifySessionClosed(const SessionSnapshot& snapshot) const {
    if (session_closed_callback_) {
        session_closed_callback_(snapshot);
    }
    logger_.info("[trace={}] [session] closed session={} user={} persona={}",
                 snapshot.last_trace_id.empty() ? "-" : snapshot.last_trace_id,
                 snapshot.session_id,
                 snapshot.user_uuid,
                 snapshot.persona_id);
}

SessionSnapshot SessionManager::SnapshotLocked(const SessionState& state) {
    SessionSnapshot snapshot;
    snapshot.session_id = state.session_id;
    snapshot.tenant_id = state.tenant_id;
    snapshot.user_uuid = state.user_uuid;
    snapshot.persona_id = state.persona_id;
    snapshot.last_trace_id = state.last_trace_id;
    snapshot.status = state.status;
    snapshot.close_reason = state.close_reason;
    snapshot.created_at = state.created_at;
    snapshot.last_active = state.last_active;
    snapshot.recent_turn_count = state.recent_history.size();
    snapshot.emotion_state = state.emotion_state.state();
    snapshot.metrics = state.metrics;
    return snapshot;
}

core::TraceContext SessionManager::MakeTrace(DispatchOptions options) const {
    core::TraceContext trace;
    trace.trace_id = NonEmptyOrGeneratedTrace(std::move(options.trace_id));
    trace.span_id = std::move(options.span_id);
    trace.user_uuid = std::move(options.user_uuid);
    return trace;
}

std::string SessionManager::MakeSessionId() {
    static std::atomic<std::uint64_t> counter{1};
    std::ostringstream oss;
    oss << "sess-" << core::GenerateTraceId().substr(0, 16)
        << "-" << counter.fetch_add(1, std::memory_order_relaxed);
    return oss.str();
}

} // namespace agent::service::persona
