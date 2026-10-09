#pragma once

#include "memory_pool.h"
#include "protocol_types.h"
#include "result.h"
#include "thread_pool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace net {

enum class ProtocolConnectionKind {
    Http,
    WebSocket
};

struct ConnectionPoolOptions {
    std::size_t max_connections = 0;
    std::size_t max_http_connections = 0;
    std::size_t max_websocket_connections = 0;
    std::size_t memory_reserve_bytes = 0;
    std::size_t memory_blocks_per_slab = 64;
    // 可空、非拥有的外部线程池；使用指针以兼容既有 API，须活到所有连接任务收口。
    core::ThreadPool* task_pool = nullptr;
};

struct ConnectionPoolStats {
    std::size_t active_connections = 0;
    std::size_t active_http_connections = 0;
    std::size_t active_websocket_connections = 0;
    std::size_t accepted_connections = 0;
    std::size_t rejected_connections = 0;
    std::size_t closed_connections = 0;
};

struct ConnectionSnapshot {
    ConnectionContext context;
    ProtocolConnectionKind kind = ProtocolConnectionKind::Http;
    std::chrono::steady_clock::time_point last_activity;
    core::MemoryPoolStats memory;
};

namespace detail {
struct ConnectionPoolEntry;
struct ConnectionPoolState;
} // namespace detail

class ConnectionLease {
public:
    ConnectionLease() noexcept = default;
    ConnectionLease(ConnectionLease&& other) noexcept;
    ConnectionLease& operator=(ConnectionLease&& other) noexcept;
    ~ConnectionLease();

    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;

    bool valid() const noexcept;
    explicit operator bool() const noexcept;

    std::uint64_t connection_id() const noexcept;
    const ConnectionContext& context() const noexcept;
    ProtocolConnectionKind kind() const noexcept;
    core::RawMemoryPool& memory_pool() noexcept;
    const core::RawMemoryPool& memory_pool() const noexcept;
    core::ThreadPool* task_pool() const noexcept;

    core::Status SetKind(ProtocolConnectionKind kind);
    void Touch();
    void Close(ConnectionCloseInfo close_info = ConnectionCloseInfo::Remote());

private:
    friend class ConnectionPool;
    friend struct detail::ConnectionPoolState;

    ConnectionLease(std::shared_ptr<detail::ConnectionPoolState> owner,
                    std::shared_ptr<detail::ConnectionPoolEntry> entry) noexcept;

    std::shared_ptr<detail::ConnectionPoolState> owner_;
    std::shared_ptr<detail::ConnectionPoolEntry> entry_;
};

class ConnectionPool {
public:
    explicit ConnectionPool(ConnectionPoolOptions options = {});
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    core::Result<ConnectionLease> Acquire(ProtocolConnectionKind kind, ConnectionContext context);
    void CloseAll(ConnectionCloseInfo close_info = ConnectionCloseInfo::Shutdown("connection pool closed"));

    ConnectionPoolStats Stats() const;
    std::vector<ConnectionSnapshot> Snapshots() const;

private:
    std::shared_ptr<detail::ConnectionPoolState> state_;
};

} // namespace net
