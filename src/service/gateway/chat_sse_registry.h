#pragma once

#include "sse_stream.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace agent::service::gateway {

struct ChatSseReplayOptions {
    std::size_t max_turns = 64;
    std::size_t max_events_per_turn = 128;
    std::size_t max_bytes_per_turn = 512 * 1024;
    std::chrono::milliseconds terminal_retention{30000};
};

struct GatewayStreamingOptions {
    ::net::SseStreamOptions transport;
    ChatSseReplayOptions replay;
};

class IChatSseChannel {
public:
    virtual ~IChatSseChannel() = default;
    virtual core::Status Publish(::net::ServerSentEvent event) = 0;
    virtual core::Status Complete(::net::ServerSentEvent terminal) = 0;
    virtual void Disconnected(std::uint64_t connection_id) = 0;
    virtual core::Status Connect(std::shared_ptr<::net::IServerEventStream> stream, std::string_view last_id) = 0;
    virtual ::net::BackpressureStats QueueStats() const = 0;
    virtual core::Status DeliveryStatus() const = 0;
    virtual core::Status CheckResume(std::string_view last_id) const = 0;
};

class IChatSseRegistry {
public:
    virtual ~IChatSseRegistry() = default;
    virtual core::Result<std::shared_ptr<IChatSseChannel>> Begin(
        std::string request_id, std::string owner, std::function<void()> cancel) = 0;
    virtual core::Result<std::shared_ptr<IChatSseChannel>> Find(
        std::string_view request_id, std::string_view owner) = 0;
    virtual core::Status Pong(std::uint64_t connection_id, std::string_view owner) = 0;
};

// 重连只重放已经发布的事件/终态；断开时取消生成，绝不据 Last-Event-ID 重新执行工具。
class ChatSseRegistry final : public IChatSseRegistry {
public:
    explicit ChatSseRegistry(ChatSseReplayOptions options = {});
    ~ChatSseRegistry() override;
    core::Result<std::shared_ptr<IChatSseChannel>> Begin(
        std::string request_id, std::string owner, std::function<void()> cancel) override;
    core::Result<std::shared_ptr<IChatSseChannel>> Find(std::string_view request_id, std::string_view owner) override;
    core::Status Pong(std::uint64_t connection_id, std::string_view owner) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
