#pragma once

#include "backpressure_queue.h"
#include "result.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace net {

struct SseStreamOptions {
    BackpressureOptions outbound{256, 1024 * 1024};
    std::size_t max_event_bytes = 256 * 1024;
    std::chrono::milliseconds heartbeat_interval{15000};
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds max_duration{300000};
    std::chrono::milliseconds write_timeout{30000};
    int reconnect_delay_ms = 1000;
    // SSE 单向心跳不能原地回复 pong；启用后由独立 HTTP 路由调用 AcknowledgeHeartbeat。
    bool require_pong = false;
};

struct ServerSentEvent {
    std::string event;
    std::string data;
    std::string id;
};

class IServerEventStream {
public:
    virtual ~IServerEventStream() = default;
    virtual core::Status SendEvent(ServerSentEvent event) = 0;
    virtual void FinishEvents() = 0;
    virtual void AbortEvents(core::Status status) = 0;
    virtual core::Status AcknowledgeHeartbeat() = 0;
    virtual std::uint64_t stream_connection_id() const noexcept = 0;
    virtual BackpressureStats EventQueueStats() const = 0;
};

using EventStreamCloseCallback = std::function<void(core::Status)>;

}
