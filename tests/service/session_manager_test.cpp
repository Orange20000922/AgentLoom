#include "session_manager.h"
#include "gateway_session_affinity_scheduler.h"
#include "runtime_maintenance_service.h"
#include "auth_session_maintenance_task.h"
#include "inference_frame_ipc_control.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <filesystem>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using agent::service::persona::ConversationTurn;
using agent::service::persona::CreateSessionRequest;
using agent::service::persona::DispatchOptions;
using agent::service::persona::GatewaySessionAffinityScheduler;
using agent::service::persona::GatewaySessionAffinitySchedulerOptions;
using agent::service::persona::PersonalityConfig;
using agent::service::persona::SessionManager;
using agent::service::persona::SessionOptions;
using agent::service::persona::SessionTurnCommit;
using agent::service::persona::SessionTurnReceipt;
using agent::service::gateway::IRuntimeMaintenanceTask;
using agent::service::gateway::InferenceFrameIpcPeerMaintenanceTask;
using agent::service::gateway::RuntimeMaintenanceService;
using agent::service::gateway::SessionMaintenanceTask;
using agent::service::gateway::AuthSessionMaintenanceTask;
using agent::service::gateway::AuthSessionRecord;
using agent::service::gateway::SqliteAuthSessionStore;

class CountingMaintenanceTask final : public IRuntimeMaintenanceTask {
public:
    explicit CountingMaintenanceTask(std::atomic<int>& ticks) : ticks_(ticks) {}

    std::string_view Name() const noexcept override {
        return "counting_task";
    }

    std::chrono::milliseconds Interval() const noexcept override {
        return 10ms;
    }

    core::Status Tick(std::stop_token stop_token) override {
        if (!stop_token.stop_requested()) {
            ticks_.fetch_add(1, std::memory_order_relaxed);
        }
        return core::Status::Ok();
    }

private:
    std::atomic<int>& ticks_;
};

class FakeIpcLeaseCoordinator final : public ipc::media::IInferenceFrameIpcLeaseCoordinator {
public:
    core::Status Start() override {
        ++starts;
        snapshot.state = ipc::media::InferenceFrameIpcControlState::Granted;
        return core::Status::Ok();
    }

    core::Status Revoke(std::string) override {
        snapshot.state = ipc::media::InferenceFrameIpcControlState::Fenced;
        return core::Status::Ok();
    }

    core::Status CheckPeer() override {
        ++probes;
        if (!fail_probe) {
            return core::Status::Ok();
        }
        snapshot.state = ipc::media::InferenceFrameIpcControlState::Fenced;
        return core::Status::Error(core::ErrorCode::Unavailable, "fake inference peer lost");
    }

    core::Status Recover() override {
        ++recoveries;
        snapshot.state = ipc::media::InferenceFrameIpcControlState::Granted;
        fail_probe = false;
        return core::Status::Ok();
    }

    void Shutdown() override {
        snapshot.state = ipc::media::InferenceFrameIpcControlState::Shutdown;
    }

    ipc::media::InferenceFrameIpcControlSnapshot Snapshot() const override {
        return snapshot;
    }

    ipc::media::InferenceFrameIpcControlSnapshot snapshot;
    bool fail_probe = false;
    int starts = 0;
    int probes = 0;
    int recoveries = 0;
};

CreateSessionRequest MakeCreateRequest(std::string session_id = "session-a") {
    PersonalityConfig personality;
    personality.name = "小橘";
    personality.description = "教育陪伴人格";

    CreateSessionRequest req;
    req.user_uuid = "user-1";
    req.persona_id = "persona-main";
    req.session_id = std::move(session_id);
    req.trace_id = "trace-create";
    req.personality = std::move(personality);
    req.time_awareness = false;
    req.emotion_state_config.noise_sigma = 0.0;
    return req;
}

TEST(SessionManagerTest, CreatesTouchesAddsTurnsAndClosesSession) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionOptions options;
    options.max_recent_turns = 2;
    SessionManager manager(compute, io, options);

    auto created = manager.CreateSession(MakeCreateRequest());
    ASSERT_TRUE(created.ok()) << created.status().message();
    EXPECT_EQ(created.value().session_id, "session-a");
    EXPECT_EQ(manager.SessionCount(), 1u);

    EXPECT_TRUE(manager.AddTurn("session-a", ConversationTurn{.user_input = "a", .response = "1"}, "trace-turn-1").ok());
    EXPECT_TRUE(manager.AddTurn("session-a", ConversationTurn{.user_input = "b", .response = "2"}, "trace-turn-2").ok());
    EXPECT_TRUE(manager.AddTurn("session-a", ConversationTurn{.user_input = "c", .response = "3"}, "trace-turn-3").ok());

    auto snapshot = manager.GetSessionSnapshot("session-a");
    ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
    EXPECT_EQ(snapshot.value().recent_turn_count, 2u);
    EXPECT_EQ(snapshot.value().last_trace_id, "trace-turn-3");

    EXPECT_TRUE(manager.TouchSession("session-a", "trace-touch").ok());
    snapshot = manager.GetSessionSnapshot("session-a");
    ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
    EXPECT_EQ(snapshot.value().last_trace_id, "trace-touch");

    EXPECT_TRUE(manager.CloseSession("session-a", "trace-close").ok());
    EXPECT_EQ(manager.SessionCount(), 0u);

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SessionManagerTest, TransfersFromLockedComputeTaskToIoWithoutRelockingAdmission) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager manager(compute, io);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest()).ok());

    std::promise<core::Status> completed;
    auto future = completed.get_future();
    DispatchOptions compute_dispatch;
    compute_dispatch.session_id = "session-a";
    compute_dispatch.trace_id = "trace-compute-to-io";
    ASSERT_TRUE(manager.SubmitCompute(
        compute_dispatch,
        [&manager, &completed](agent::service::persona::SessionState& session,
                              core::ThreadPoolContext&) {
            DispatchOptions io_dispatch;
            io_dispatch.session_id = session.session_id;
            io_dispatch.trace_id = "trace-io-stage";
            return manager.SubmitIoFromSessionTask(
                session,
                std::move(io_dispatch),
                [&completed](agent::service::persona::SessionState&,
                             core::ThreadPoolContext&) {
                    completed.set_value(core::Status::Ok());
                    return core::Status::Ok();
                });
        }).ok());

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(future.get().ok());
    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SessionManagerTest, RejectsNewSessionsWhenActiveLimitIsReached) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionOptions options;
    options.max_active_sessions = 2;
    SessionManager manager(compute, io, options);

    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-limit-a")).ok());
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-limit-b")).ok());
    EXPECT_EQ(manager.SessionCount(), 2u);

    auto rejected = manager.CreateSession(MakeCreateRequest("session-limit-c"));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), core::ErrorCode::ResourceExhausted);
    EXPECT_EQ(rejected.status().message(), "active session limit reached");

    ASSERT_TRUE(manager.CloseSession("session-limit-a").ok());
    auto admitted = manager.CreateSession(MakeCreateRequest("session-limit-c"));
    ASSERT_TRUE(admitted.ok()) << admitted.status().message();
    EXPECT_EQ(manager.SessionCount(), 2u);

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SessionManagerTest, DispatchPropagatesTraceIdIntoComputeAndIoPools) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager manager(compute, io);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-dispatch")).ok());

    std::promise<std::string> compute_trace;
    auto compute_future = compute_trace.get_future();
    auto compute_status = manager.SubmitCompute(
        DispatchOptions{
            .session_id = "session-dispatch",
            .trace_id = "trace-compute",
            .user_uuid = "user-1",
            .module = "persona",
            .operation = "emotion_update",
        },
        [&compute_trace](auto& session, core::ThreadPoolContext&) {
            auto updated = session.emotion_state.Update("joy", 0.9, "neutral", 0.2);
            if (!updated.ok()) {
                return updated.status();
            }
            compute_trace.set_value(std::string(core::CurrentTraceId()));
            return core::Status::Ok();
        });
    ASSERT_TRUE(compute_status.ok()) << compute_status.message();
    ASSERT_EQ(compute_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(compute_future.get(), "trace-compute");

    std::promise<std::string> io_trace;
    auto io_future = io_trace.get_future();
    auto io_status = manager.SubmitIo(
        DispatchOptions{
            .session_id = "session-dispatch",
            .trace_id = "trace-io",
            .user_uuid = "user-1",
            .module = "storage",
            .operation = "persist",
        },
        [&io_trace](auto&, core::ThreadPoolContext&) {
            io_trace.set_value(std::string(core::CurrentTraceId()));
            return core::Status::Ok();
        });
    ASSERT_TRUE(io_status.ok()) << io_status.message();
    ASSERT_EQ(io_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(io_future.get(), "trace-io");

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SessionManagerTest, DispatchRejectsSessionClosedBeforeSubmission) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager manager(compute, io);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-close-race")).ok());
    ASSERT_TRUE(manager.CloseSession("session-close-race", "trace-close").ok());

    std::atomic<bool> ran{false};
    auto status = manager.SubmitCompute(
        DispatchOptions{.session_id = "session-close-race", .trace_id = "trace-late"},
        [&ran](auto&, core::ThreadPoolContext&) {
            ran.store(true, std::memory_order_relaxed);
            return core::Status::Ok();
        });
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), core::ErrorCode::NotFound);

    compute.Shutdown(true);
    io.Shutdown(true);
    EXPECT_FALSE(ran.load(std::memory_order_relaxed));
    EXPECT_EQ(compute.Stats().failed_tasks, 0u);
}

TEST(SessionManagerTest, DispatchUsesRegisteredIdentityForCrossSessionFairness) {
    auto scheduler = std::make_shared<GatewaySessionAffinityScheduler>(
        GatewaySessionAffinitySchedulerOptions{
            .max_active_keys = 8,
            .max_outstanding_per_key = 4,
            .max_outstanding_per_fairness_key = 1,
            .max_outstanding_per_tenant = 8,
        });
    core::ThreadPool compute({
        .worker_count = 1,
        .queue_capacity = 8,
        .name = "trusted-fairness-test",
        .scheduler = scheduler,
    });
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionManager manager(compute, io);
    auto first_request = MakeCreateRequest("session-fairness-a");
    first_request.tenant_id = "tenant-a";
    auto second_request = MakeCreateRequest("session-fairness-b");
    second_request.tenant_id = "tenant-a";
    ASSERT_TRUE(manager.CreateSession(std::move(first_request)).ok());
    ASSERT_TRUE(manager.CreateSession(std::move(second_request)).ok());

    std::promise<void> first_started;
    auto first_started_future = first_started.get_future();
    std::promise<void> finish_first;
    auto finish_first_future = finish_first.get_future().share();
    ASSERT_TRUE(manager.SubmitCompute(
                           DispatchOptions{
                               .session_id = "session-fairness-a",
                               .trace_id = "trace-fairness-a",
                               .user_uuid = "spoofed-user-a",
                           },
                           [&](auto&, core::ThreadPoolContext&) {
                               first_started.set_value();
                               finish_first_future.wait();
                               return core::Status::Ok();
                           })
                    .ok());
    ASSERT_EQ(first_started_future.wait_for(2s), std::future_status::ready);

    const auto rejected = manager.SubmitCompute(
        DispatchOptions{
            .session_id = "session-fairness-b",
            .trace_id = "trace-fairness-b",
            .user_uuid = "spoofed-user-b",
        },
        [](auto&, core::ThreadPoolContext&) { return core::Status::Ok(); });
    EXPECT_EQ(rejected.code(), core::ErrorCode::ResourceExhausted);
    EXPECT_EQ(scheduler->Snapshot().rejected_per_fairness_key, 1u);

    finish_first.set_value();
    compute.Shutdown(true);
    io.Shutdown(true);
    EXPECT_EQ(scheduler->Snapshot().active_keys, 0u);
}

TEST(SessionManagerTest, CloseAndSnapshotDoNotWaitForSlowTurn) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({2, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionOptions options;
    options.max_active_sessions = 1;
    SessionManager manager(compute, io, options);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-slow-turn")).ok());
    std::promise<void> entered;
    std::promise<void> release;
    std::promise<core::Result<SessionTurnReceipt>> completed;
    auto entered_future = entered.get_future();
    auto completed_future = completed.get_future();
    auto release_future = release.get_future();
    ASSERT_TRUE(manager.SubmitTurn(
        DispatchOptions{.session_id = "session-slow-turn", .trace_id = "trace-slow"},
        [&entered, &release_future](const auto& snapshot, auto& commit, auto&) {
            entered.set_value();
            release_future.wait();
            commit.trace_id = "trace-slow";
            commit.turn.user_input = "slow";
            commit.turn.response = "done";
            commit.emotion_state = snapshot.state.emotion_state;
            return core::Status::Ok();
        },
        [&completed](auto result) { completed.set_value(std::move(result)); })
        .ok());
    ASSERT_EQ(entered_future.wait_for(2s), std::future_status::ready);

    const auto close_started = std::chrono::steady_clock::now();
    ASSERT_TRUE(manager.CloseSession("session-slow-turn", "trace-close").ok());
    EXPECT_LT(std::chrono::steady_clock::now() - close_started, 100ms);
    EXPECT_EQ(manager.SessionCount(), 0u);
    auto rejected = manager.CreateSession(MakeCreateRequest("session-rejected"));
    EXPECT_EQ(rejected.status().code(), core::ErrorCode::ResourceExhausted);

    release.set_value();
    ASSERT_EQ(completed_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(completed_future.get().ok());
    auto admitted = manager.CreateSession(MakeCreateRequest("session-admitted"));
    ASSERT_TRUE(admitted.ok()) << admitted.status().message();
    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SessionManagerTest, SerializesTurnsAcrossIoWorkersAndPreservesResidentQuota) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({2, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionOptions options;
    options.max_active_sessions = 1;
    SessionManager manager(compute, io, options);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-ordered")).ok());

    std::promise<void> first_entered;
    std::promise<void> release_first;
    std::promise<void> second_entered;
    std::promise<core::Result<SessionTurnReceipt>> first_done;
    std::promise<core::Result<SessionTurnReceipt>> second_done;
    auto first_entered_future = first_entered.get_future();
    auto second_entered_future = second_entered.get_future();
    auto first_done_future = first_done.get_future();
    auto second_done_future = second_done.get_future();
    auto release_future = release_first.get_future();
    std::atomic<int> entered{0};

    auto submit = [&manager](std::string trace,
                             auto task,
                             auto completion) {
        return manager.SubmitTurn(
            DispatchOptions{.session_id = "session-ordered", .trace_id = std::move(trace)},
            std::move(task), std::move(completion));
    };
    ASSERT_TRUE(submit(
        "trace-first",
        [&first_entered, &release_future, &entered](const auto& snapshot, auto& commit, auto&) {
            entered.fetch_add(1);
            first_entered.set_value();
            release_future.wait();
            commit.trace_id = "trace-first";
            commit.turn.user_input = "first";
            commit.turn.response = "1";
            commit.emotion_state = snapshot.state.emotion_state;
            return core::Status::Ok();
        },
        [&first_done](auto result) { first_done.set_value(std::move(result)); })
        .ok());
    ASSERT_EQ(first_entered_future.wait_for(2s), std::future_status::ready);

    ASSERT_TRUE(submit(
        "trace-second",
        [&second_entered, &entered](const auto& snapshot, auto& commit, auto&) {
            entered.fetch_add(1);
            second_entered.set_value();
            commit.trace_id = "trace-second";
            commit.turn.user_input = "second";
            commit.turn.response = "2";
            commit.emotion_state = snapshot.state.emotion_state;
            return core::Status::Ok();
        },
        [&second_done](auto result) { second_done.set_value(std::move(result)); })
        .ok());
    EXPECT_EQ(second_entered_future.wait_for(100ms), std::future_status::timeout);
    EXPECT_EQ(entered.load(), 1);

    release_first.set_value();
    ASSERT_EQ(first_done_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second_done_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(first_done_future.get().ok());
    EXPECT_TRUE(second_done_future.get().ok());
    EXPECT_EQ(entered.load(), 2);

    ASSERT_TRUE(manager.CloseSession("session-ordered").ok());

    auto admitted = manager.CreateSession(MakeCreateRequest("session-admitted"));
    ASSERT_TRUE(admitted.ok()) << admitted.status().message();
    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SessionManagerTest, SubmitTurnOnDedicatedAffinityPoolKeepsOtherIoWorkAvailable) {
    core::ThreadPool compute({1, 32, "session-dedicated-compute"});
    core::ThreadPool io({1, 32, "session-dedicated-io"});
    core::ThreadPool llm({2, 32, "session-dedicated-llm",
                          std::make_shared<GatewaySessionAffinityScheduler>(
                              GatewaySessionAffinitySchedulerOptions{
                                  .max_active_keys = 16,
                                  .max_outstanding_per_key = 8,
                                  .max_outstanding_per_fairness_key = 16,
                                  .max_outstanding_per_tenant = 32})});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(llm.Start().ok());

    SessionManager manager(compute, io, {}, core::LoggerAdapter::ForModule("service"), &llm);
    EXPECT_EQ(manager.PoolStats().io.worker_count, 1u);
    EXPECT_EQ(manager.PoolStats().llm.worker_count, 2u);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-dedicated-a")).ok());
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-dedicated-b")).ok());

    std::promise<void> slow_started;
    auto slow_started_future = slow_started.get_future();
    std::promise<core::Result<SessionTurnReceipt>> slow_done;
    auto slow_done_future = slow_done.get_future();
    ASSERT_TRUE(manager.SubmitTurn(
        DispatchOptions{.session_id = "session-dedicated-a", .trace_id = "trace-slow"},
        [&slow_started](const auto&, auto&, auto&) {
            slow_started.set_value();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return core::Status::Ok();
        },
        [&slow_done](core::Result<SessionTurnReceipt> result) mutable {
            slow_done.set_value(std::move(result));
        }).ok());
    ASSERT_EQ(slow_started_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    std::promise<void> queued_same_session_started;
    auto queued_same_session_future = queued_same_session_started.get_future();
    std::promise<core::Result<SessionTurnReceipt>> queued_same_session_done;
    auto queued_same_session_done_future = queued_same_session_done.get_future();
    ASSERT_TRUE(manager.SubmitTurn(
        DispatchOptions{.session_id = "session-dedicated-a", .trace_id = "trace-queued-same"},
        [&queued_same_session_started](const auto&, auto&, auto&) {
            queued_same_session_started.set_value();
            return core::Status::Ok();
        },
        [&queued_same_session_done](core::Result<SessionTurnReceipt> result) mutable {
            queued_same_session_done.set_value(std::move(result));
        }).ok());
    EXPECT_EQ(queued_same_session_future.wait_for(std::chrono::milliseconds(50)),
              std::future_status::timeout);

    std::promise<void> other_session_llm_ran;
    auto other_session_llm_future = other_session_llm_ran.get_future();
    std::promise<core::Result<SessionTurnReceipt>> other_session_llm_done;
    auto other_session_llm_done_future = other_session_llm_done.get_future();
    ASSERT_TRUE(manager.SubmitTurn(
        DispatchOptions{.session_id = "session-dedicated-b", .trace_id = "trace-other-session"},
        [&other_session_llm_ran](const auto&, auto&, auto&) {
            other_session_llm_ran.set_value();
            return core::Status::Ok();
        },
        [&other_session_llm_done](core::Result<SessionTurnReceipt> result) mutable {
            other_session_llm_done.set_value(std::move(result));
        }).ok());
    EXPECT_EQ(other_session_llm_future.wait_for(std::chrono::milliseconds(500)),
              std::future_status::ready);
    ASSERT_EQ(other_session_llm_done_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_TRUE(other_session_llm_done_future.get().ok());

    std::promise<void> io_ran;
    auto io_ran_future = io_ran.get_future();
    ASSERT_TRUE(manager.SubmitIo(
        DispatchOptions{.session_id = "session-dedicated-b", .trace_id = "trace-io"},
        [&io_ran](auto&, auto&) {
            io_ran.set_value();
            return core::Status::Ok();
        }).ok());
    EXPECT_EQ(io_ran_future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
    ASSERT_EQ(slow_done_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(slow_done_future.get().ok());
    ASSERT_EQ(queued_same_session_done_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_TRUE(queued_same_session_done_future.get().ok());

    manager.CloseSession("session-dedicated-a");
    manager.CloseSession("session-dedicated-b");
    llm.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(SessionManagerTest, AsyncTurnRejectsFifoSchedulerBecauseItCannotPreserveSessionOrder) {
    core::ThreadPool compute({1, 8, "async-turn-fifo-compute"});
    core::ThreadPool io({1, 8, "async-turn-fifo-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    SessionManager manager(compute, io);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-async-fifo")).ok());

    auto status = manager.SubmitTurnAsync(
        DispatchOptions{.session_id = "session-async-fifo"},
        [](const auto&, auto&, auto) { return core::Status::Ok(); },
        [](auto) {});

    EXPECT_EQ(status.code(), core::ErrorCode::FailedPrecondition);
    manager.CloseSession("session-async-fifo");
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(SessionManagerTest, AsyncTurnReleasesWorkerButKeepsSameSessionLaneUntilFinish) {
    core::ThreadPool compute({1, 32, "async-turn-compute"});
    core::ThreadPool io({1, 32, "async-turn-io"});
    core::ThreadPool llm({1, 32, "async-turn-llm",
                          std::make_shared<GatewaySessionAffinityScheduler>(
                              GatewaySessionAffinitySchedulerOptions{
                                  .max_active_keys = 8,
                                  .max_outstanding_per_key = 4,
                                  .max_outstanding_per_fairness_key = 8,
                                  .max_outstanding_per_tenant = 16})});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(llm.Start().ok());
    SessionManager manager(compute, io, {}, core::LoggerAdapter::ForModule("service"), &llm);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-async-a")).ok());
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-async-b")).ok());

    std::promise<SessionManager::SessionTurnAsyncFinish> first_finish_ready;
    auto first_finish_future = first_finish_ready.get_future();
    std::promise<core::Result<SessionTurnReceipt>> first_done;
    auto first_done_future = first_done.get_future();
    ASSERT_TRUE(manager.SubmitTurnAsync(
        DispatchOptions{.session_id = "session-async-a", .trace_id = "trace-async-a1"},
        [&first_finish_ready](const auto&, auto&, auto finish) {
            first_finish_ready.set_value(std::move(finish));
            return core::Status::Ok();
        },
        [&first_done](auto result) { first_done.set_value(std::move(result)); }).ok());
    ASSERT_EQ(first_finish_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto finish_first = first_finish_future.get();

    std::promise<void> same_session_started;
    auto same_session_started_future = same_session_started.get_future();
    std::promise<core::Result<SessionTurnReceipt>> same_session_done;
    auto same_session_done_future = same_session_done.get_future();
    ASSERT_TRUE(manager.SubmitTurnAsync(
        DispatchOptions{.session_id = "session-async-a", .trace_id = "trace-async-a2"},
        [&same_session_started](const auto& snapshot, auto&, auto finish) {
            same_session_started.set_value();
            SessionTurnCommit commit;
            commit.trace_id = "trace-async-a2";
            commit.turn.user_input = "second";
            commit.turn.response = "second-response";
            commit.emotion_state = snapshot.state.emotion_state;
            finish(std::move(commit));
            return core::Status::Ok();
        },
        [&same_session_done](auto result) { same_session_done.set_value(std::move(result)); }).ok());
    EXPECT_EQ(same_session_started_future.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);

    // 只有一个 LLM worker，但 A 的异步等待已经释放 worker，因此 B 能立即开始并完成。
    std::promise<void> other_session_started;
    auto other_session_started_future = other_session_started.get_future();
    std::promise<core::Result<SessionTurnReceipt>> other_session_done;
    auto other_session_done_future = other_session_done.get_future();
    ASSERT_TRUE(manager.SubmitTurnAsync(
        DispatchOptions{.session_id = "session-async-b", .trace_id = "trace-async-b1"},
        [&other_session_started](const auto& snapshot, auto&, auto finish) {
            other_session_started.set_value();
            SessionTurnCommit commit;
            commit.trace_id = "trace-async-b1";
            commit.turn.user_input = "other";
            commit.turn.response = "other-response";
            commit.emotion_state = snapshot.state.emotion_state;
            finish(std::move(commit));
            return core::Status::Ok();
        },
        [&other_session_done](auto result) { other_session_done.set_value(std::move(result)); }).ok());
    ASSERT_EQ(other_session_started_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    ASSERT_EQ(other_session_done_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_TRUE(other_session_done_future.get().ok());

    SessionTurnCommit first_commit;
    first_commit.trace_id = "trace-async-a1";
    first_commit.turn.user_input = "first";
    first_commit.turn.response = "first-response";
    finish_first(std::move(first_commit));

    ASSERT_EQ(first_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_TRUE(first_done_future.get().ok());
    ASSERT_EQ(same_session_started_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    ASSERT_EQ(same_session_done_future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    ASSERT_TRUE(same_session_done_future.get().ok());

    auto snapshot = manager.GetSessionSnapshot("session-async-a");
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.value().metrics.turn_count, 2u);
    manager.CloseSession("session-async-a");
    manager.CloseSession("session-async-b");
    llm.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

// #67：排队中的 Turn 按 trace 出队丢弃，不进入执行流；已启动 Turn 与其他排队 Turn 不受影响。
TEST(SessionManagerTest, CancelQueuedTurnDiscardsOnlyMatchingTraceBeforeStart) {
    core::ThreadPool compute({1, 32, "queued-cancel-compute"});
    core::ThreadPool io({1, 32, "queued-cancel-io"});
    core::ThreadPool llm({1, 32, "queued-cancel-llm",
                          std::make_shared<GatewaySessionAffinityScheduler>(
                              GatewaySessionAffinitySchedulerOptions{
                                  .max_active_keys = 8,
                                  .max_outstanding_per_key = 4,
                                  .max_outstanding_per_fairness_key = 8,
                                  .max_outstanding_per_tenant = 16})});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());
    ASSERT_TRUE(llm.Start().ok());
    SessionManager manager(compute, io, {}, core::LoggerAdapter::ForModule("service"), &llm);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-queued")).ok());

    std::promise<SessionManager::SessionTurnAsyncFinish> active_ready;
    auto active_future = active_ready.get_future();
    ASSERT_TRUE(manager.SubmitTurnAsync(
        DispatchOptions{.session_id = "session-queued", .trace_id = "trace-active"},
        [&active_ready](const auto&, auto&, auto finish) {
            active_ready.set_value(std::move(finish));
            return core::Status::Ok();
        },
        [](auto) {}).ok());
    ASSERT_EQ(active_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto finish_active = active_future.get();

    std::atomic<int> cancelled_started{0};
    std::atomic<int> cancelled_completions{0};
    core::ErrorCode cancelled_code = core::ErrorCode::Ok;
    ASSERT_TRUE(manager.SubmitTurnAsync(
        DispatchOptions{.session_id = "session-queued", .trace_id = "trace-cancelled"},
        [&cancelled_started](const auto&, auto&, auto finish) {
            ++cancelled_started;
            finish(core::Status::Error(core::ErrorCode::InternalError, "must not start"));
            return core::Status::Ok();
        },
        [&](auto result) {
            ++cancelled_completions;
            cancelled_code = result.ok() ? core::ErrorCode::Ok : result.status().code();
        }).ok());
    std::promise<core::Result<SessionTurnReceipt>> kept_done;
    auto kept_future = kept_done.get_future();
    ASSERT_TRUE(manager.SubmitTurnAsync(
        DispatchOptions{.session_id = "session-queued", .trace_id = "trace-kept"},
        [](const auto& snapshot, auto&, auto finish) {
            SessionTurnCommit commit;
            commit.trace_id = "trace-kept";
            commit.turn.user_input = "kept";
            commit.turn.response = "kept-response";
            commit.emotion_state = snapshot.state.emotion_state;
            finish(std::move(commit));
            return core::Status::Ok();
        },
        [&kept_done](auto result) { kept_done.set_value(std::move(result)); }).ok());

    EXPECT_EQ(manager.CancelQueuedTurn("session-queued", "trace-active").code(), core::ErrorCode::NotFound);
    ASSERT_TRUE(manager.CancelQueuedTurn("session-queued", "trace-cancelled").ok());
    EXPECT_EQ(manager.CancelQueuedTurn("session-queued", "trace-cancelled").code(), core::ErrorCode::NotFound);
    EXPECT_EQ(cancelled_completions, 1);
    EXPECT_EQ(cancelled_code, core::ErrorCode::Cancelled);

    SessionTurnCommit active_commit;
    active_commit.trace_id = "trace-active";
    active_commit.turn.user_input = "active";
    active_commit.turn.response = "active-response";
    finish_active(std::move(active_commit));
    ASSERT_EQ(kept_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(kept_future.get().ok());
    EXPECT_EQ(cancelled_started, 0);
    EXPECT_EQ(cancelled_completions, 1);

    auto snapshot = manager.GetSessionSnapshot("session-queued");
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.value().metrics.turn_count, 2u);
    manager.CloseSession("session-queued");
    llm.Shutdown(true);
    io.Shutdown(true);
    compute.Shutdown(true);
}

TEST(GatewaySessionAffinitySchedulerTest, SerializesSessionKeyAndRejectsExcessOutstandingWork) {
    auto scheduler = std::make_shared<GatewaySessionAffinityScheduler>(
        GatewaySessionAffinitySchedulerOptions{
            .max_active_keys = 8,
            .max_outstanding_per_key = 2,
            .max_outstanding_per_fairness_key = 4,
        });
    core::ThreadPool pool({
        .worker_count = 2,
        .queue_capacity = 8,
        .name = "session-affinity-test",
        .scheduler = scheduler,
    });
    ASSERT_TRUE(pool.Start().ok());

    std::promise<void> first_started;
    auto first_started_future = first_started.get_future();
    std::promise<void> allow_first_finish;
    auto allow_first_finish_future = allow_first_finish.get_future().share();
    std::promise<void> other_session_ran;
    auto other_session_future = other_session_ran.get_future();
    std::promise<void> second_session_a_ran;
    auto second_session_a_future = second_session_a_ran.get_future();

    core::ThreadPoolTaskMetadata first_metadata;
    first_metadata.concurrency_key = "session-a";
    first_metadata.fairness_key = "user-a";
    ASSERT_TRUE(pool.Submit(
                        [&] {
                            first_started.set_value();
                            allow_first_finish_future.wait();
                        },
                        {},
                        "session-a-first",
                        std::move(first_metadata))
                        .ok());
    ASSERT_EQ(first_started_future.wait_for(2s), std::future_status::ready);

    core::ThreadPoolTaskMetadata second_metadata;
    second_metadata.concurrency_key = "session-a";
    second_metadata.fairness_key = "user-a";
    ASSERT_TRUE(pool.Submit(
                        [&] { second_session_a_ran.set_value(); },
                        {},
                        "session-a-second",
                        std::move(second_metadata))
                        .ok());
    EXPECT_EQ(second_session_a_future.wait_for(100ms), std::future_status::timeout);

    core::ThreadPoolTaskMetadata rejected_metadata;
    rejected_metadata.concurrency_key = "session-a";
    rejected_metadata.fairness_key = "user-a";
    const auto rejected = pool.Submit(
        [] {}, {}, "session-a-rejected", std::move(rejected_metadata));
    EXPECT_EQ(rejected.code(), core::ErrorCode::ResourceExhausted);

    core::ThreadPoolTaskMetadata other_metadata;
    other_metadata.concurrency_key = "session-b";
    other_metadata.fairness_key = "user-b";
    ASSERT_TRUE(pool.Submit(
                        [&] { other_session_ran.set_value(); },
                        {},
                        "session-b",
                        std::move(other_metadata))
                        .ok());
    EXPECT_EQ(other_session_future.wait_for(2s), std::future_status::ready);

    allow_first_finish.set_value();
    EXPECT_EQ(second_session_a_future.wait_for(2s), std::future_status::ready);
    pool.Shutdown(true);

    const auto snapshot = scheduler->Snapshot();
    EXPECT_EQ(snapshot.active_keys, 0u);
    EXPECT_EQ(snapshot.queued_tasks, 0u);
    EXPECT_EQ(snapshot.running_tasks, 0u);
    EXPECT_GE(snapshot.rejected_tasks, 1u);
}

TEST(GatewaySessionAffinitySchedulerTest, EnforcesTenantOutstandingLimitAcrossUsers) {
    auto scheduler = std::make_shared<GatewaySessionAffinityScheduler>(
        GatewaySessionAffinitySchedulerOptions{
            .max_active_keys = 8,
            .max_outstanding_per_key = 4,
            .max_outstanding_per_fairness_key = 4,
            .max_outstanding_per_tenant = 1,
        });
    core::ThreadPool pool({
        .worker_count = 1,
        .queue_capacity = 8,
        .name = "tenant-affinity-test",
        .scheduler = scheduler,
    });
    ASSERT_TRUE(pool.Start().ok());

    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> finish;
    auto finish_future = finish.get_future().share();
    core::ThreadPoolTaskMetadata first;
    first.concurrency_key = "session-a";
    first.fairness_key = "tenant-a:user-a";
    first.tenant_key = "tenant-a";
    ASSERT_TRUE(pool.Submit(
                        [&] {
                            started.set_value();
                            finish_future.wait();
                        },
                        {},
                        "tenant-a-first",
                        std::move(first))
                        .ok());
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);

    core::ThreadPoolTaskMetadata second;
    second.concurrency_key = "session-b";
    second.fairness_key = "tenant-a:user-b";
    second.tenant_key = "tenant-a";
    const auto rejected = pool.Submit([] {}, {}, "tenant-a-second", std::move(second));
    EXPECT_EQ(rejected.code(), core::ErrorCode::ResourceExhausted);
    EXPECT_EQ(scheduler->Snapshot().rejected_per_tenant, 1u);

    finish.set_value();
    pool.Shutdown(true);
    EXPECT_EQ(scheduler->Snapshot().active_keys, 0u);
}

TEST(RuntimeMaintenanceServiceTest, RunsRegisteredTaskOnDedicatedWorker) {
    std::atomic<int> ticks{0};
    RuntimeMaintenanceService maintenance;
    ASSERT_TRUE(maintenance.RegisterTask(std::make_shared<CountingMaintenanceTask>(ticks)).ok());
    ASSERT_TRUE(maintenance.Start().ok());

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (ticks.load(std::memory_order_relaxed) < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    maintenance.Stop();

    EXPECT_GE(ticks.load(std::memory_order_relaxed), 2);
    auto health = maintenance.SnapshotHealth();
    ASSERT_EQ(health.size(), 1u);
    EXPECT_EQ(health.front().name, "counting_task");
    EXPECT_GE(health.front().success_count, 1u);
    EXPECT_EQ(health.front().failure_count, 0u);
}

TEST(RuntimeMaintenanceServiceTest, SessionCleanupTaskExpiresIdleSessions) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionOptions options;
    options.idle_timeout = std::chrono::minutes(0);
    SessionManager manager(compute, io, options);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-maintenance-expire")).ok());

    RuntimeMaintenanceService maintenance;
    ASSERT_TRUE(maintenance.RegisterTask(std::make_shared<SessionMaintenanceTask>(
                    manager,
                    10ms))
                    .ok());
    ASSERT_TRUE(maintenance.Start().ok());

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (manager.SessionCount() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    maintenance.Stop();

    EXPECT_EQ(manager.SessionCount(), 0u);
    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(SessionManagerTest, ReclamationHoldDefersIdleCleanupUntilReleased) {
    core::ThreadPool compute({1, 8, "test-compute"});
    core::ThreadPool io({1, 8, "test-io"});
    ASSERT_TRUE(compute.Start().ok());
    ASSERT_TRUE(io.Start().ok());

    SessionOptions options;
    options.idle_timeout = std::chrono::minutes(0);
    SessionManager manager(compute, io, options);
    ASSERT_TRUE(manager.CreateSession(MakeCreateRequest("session-reclamation-hold")).ok());
    ASSERT_TRUE(manager.HoldReclamationUntil(
        "session-reclamation-hold",
        std::chrono::steady_clock::now() + std::chrono::hours(1),
        "trace-hold").ok());

    EXPECT_TRUE(manager.CleanupExpired().empty());
    EXPECT_EQ(manager.SessionCount(), 1u);

    ASSERT_TRUE(manager.ReleaseReclamationHold(
        "session-reclamation-hold",
        "trace-release").ok());
    EXPECT_EQ(manager.CleanupExpired().size(), 1u);
    EXPECT_EQ(manager.SessionCount(), 0u);

    compute.Shutdown(true);
    io.Shutdown(true);
}

TEST(RuntimeMaintenanceServiceTest, AuthSessionCleanupTaskRemovesExpiredSessions) {
    const auto path = std::filesystem::temp_directory_path() / "agent_gateway_auth_maintenance_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    auto store = std::make_shared<SqliteAuthSessionStore>(path.string());
    ASSERT_TRUE(store->EnsureSchema().ok());
    AuthSessionRecord record;
    record.token_id = "maintenance-expired";
    record.user_uuid = "maintenance-user";
    record.issued_at = std::chrono::system_clock::now() - 2h;
    record.expires_at = std::chrono::system_clock::now() - 1h;
    ASSERT_TRUE(store->UpsertSession(record).ok());

    AuthSessionMaintenanceTask task(store, 10ms, 8);
    std::stop_source stop;
    ASSERT_TRUE(task.Tick(stop.get_token()).ok());
    auto resolved = store->ResolveSession(record.token_id);
    EXPECT_FALSE(resolved.ok());
    EXPECT_EQ(resolved.status().code(), core::ErrorCode::NotFound);

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(RuntimeMaintenanceServiceTest, IpcPeerTaskFencesThenRecoversLostInferencePeer) {
    auto coordinator = std::make_shared<FakeIpcLeaseCoordinator>();
    InferenceFrameIpcPeerMaintenanceTask task(coordinator, 10ms, true);
    std::stop_source stop;

    ASSERT_TRUE(task.Tick(stop.get_token()).ok());
    EXPECT_EQ(coordinator->starts, 1);
    EXPECT_EQ(coordinator->snapshot.state, ipc::media::InferenceFrameIpcControlState::Granted);

    coordinator->fail_probe = true;
    const auto lost = task.Tick(stop.get_token());
    EXPECT_EQ(lost.code(), core::ErrorCode::Unavailable);
    EXPECT_EQ(coordinator->snapshot.state, ipc::media::InferenceFrameIpcControlState::Fenced);

    ASSERT_TRUE(task.Tick(stop.get_token()).ok());
    EXPECT_EQ(coordinator->recoveries, 1);
    EXPECT_EQ(coordinator->snapshot.state, ipc::media::InferenceFrameIpcControlState::Granted);
}

} // namespace
