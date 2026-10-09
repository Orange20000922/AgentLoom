#include "chat_sse_registry.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {
using namespace agent::service::gateway;

class MemoryStream final : public ::net::IServerEventStream {
public:
    explicit MemoryStream(std::uint64_t id) : id_(id) {}
    core::Status SendEvent(::net::ServerSentEvent event) override {
        events.push_back(std::move(event));
        return core::Status::Ok();
    }
    void FinishEvents() override { finished = true; }
    void AbortEvents(core::Status) override { finished = true; }
    core::Status AcknowledgeHeartbeat() override { ++pongs; return core::Status::Ok(); }
    std::uint64_t stream_connection_id() const noexcept override { return id_; }
    ::net::BackpressureStats EventQueueStats() const override { return {}; }
    std::vector<::net::ServerSentEvent> events;
    bool finished = false;
    int pongs = 0;
private:
    std::uint64_t id_;
};

TEST(ChatSseRegistryTest, ReconnectionReplaysOnlySuffixAndTerminalWithoutRestartingGeneration) {
    ChatSseRegistry registry;
    int cancels = 0;
    auto created = registry.Begin("request", "owner", [&] { ++cancels; });
    ASSERT_TRUE(created.ok());
    auto channel = created.value();
    auto first = std::make_shared<MemoryStream>(1);
    ASSERT_TRUE(channel->Connect(first, {}).ok());
    ASSERT_TRUE(channel->Publish({"TextDelta", "first", "request:1"}).ok());
    ASSERT_TRUE(channel->Publish({"TextDelta", "second", "request:2"}).ok());
    channel->Disconnected(1);
    channel->Disconnected(1);
    EXPECT_EQ(cancels, 1);
    EXPECT_EQ(channel->Publish({"TextDelta", "late", "request:3"}).code(), core::ErrorCode::Cancelled);
    ASSERT_TRUE(channel->Complete({"TurnFailed", "cancelled", "request:3"}).ok());
    auto second = std::make_shared<MemoryStream>(2);
    ASSERT_TRUE(registry.Find("request", "owner").value()->Connect(second, "request:1").ok());
    ASSERT_EQ(second->events.size(), 2);
    EXPECT_EQ(second->events[0].id, "request:2");
    EXPECT_EQ(second->events[1].event, "TurnFailed");
    EXPECT_TRUE(second->finished);
    EXPECT_EQ(cancels, 1);
}

TEST(ChatSseRegistryTest, EnforcesOwnerCursorCapacityAndExpiresTerminals) {
    ChatSseReplayOptions options;
    options.max_turns = 1;
    options.max_events_per_turn = 2;
    options.terminal_retention = std::chrono::milliseconds(10);
    ChatSseRegistry registry(options);
    auto created = registry.Begin("r", "owner", [] {});
    ASSERT_TRUE(created.ok());
    auto channel = created.value();
    auto stream = std::make_shared<MemoryStream>(1);
    ASSERT_TRUE(channel->Connect(stream, {}).ok());
    EXPECT_EQ(registry.Begin("next", "owner", [] {}).status().code(), core::ErrorCode::ResourceExhausted);
    EXPECT_EQ(registry.Find("r", "intruder").status().code(), core::ErrorCode::PermissionDenied);
    EXPECT_EQ(registry.Pong(1, "intruder").code(), core::ErrorCode::NotFound);
    ASSERT_TRUE(registry.Pong(1, "owner").ok());
    EXPECT_EQ(stream->pongs, 1);
    for (int i = 1; i <= 3; ++i) ASSERT_TRUE(channel->Publish({"TextDelta", "x", "r:" + std::to_string(i)}).ok());
    channel->Disconnected(1);
    auto resume = std::make_shared<MemoryStream>(2);
    EXPECT_EQ(channel->Connect(resume, "r:1").code(), core::ErrorCode::FailedPrecondition);
    EXPECT_EQ(channel->Connect(resume, {}).code(), core::ErrorCode::FailedPrecondition);
    EXPECT_EQ(channel->Connect(resume, "foreign:3").code(), core::ErrorCode::FailedPrecondition);
    EXPECT_EQ(channel->CheckResume("r:1").code(), core::ErrorCode::FailedPrecondition);
    EXPECT_EQ(channel->CheckResume({}).code(), core::ErrorCode::FailedPrecondition);
    EXPECT_TRUE(channel->CheckResume("r:3").ok());
    ASSERT_TRUE(channel->Complete({"TurnFailed", "cancelled", "r:4"}).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(registry.Find("r", "owner").status().code(), core::ErrorCode::NotFound);
    EXPECT_TRUE(registry.Begin("next", "owner", [] {}).ok());
}

TEST(ChatSseRegistryTest, CompletedTurnEvictionDoesNotRejectNewHealthyTurns) {
    ChatSseReplayOptions options;
    options.max_turns = 1;
    ChatSseRegistry registry(options);
    auto created = registry.Begin("r", "owner", [] {});
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(created.value()->Complete({"TurnCompleted", "ok", "r:1"}).ok());
    EXPECT_TRUE(registry.Begin("next", "owner", [] {}).ok());
    EXPECT_EQ(registry.Find("r", "owner").status().code(), core::ErrorCode::NotFound);
}
}
