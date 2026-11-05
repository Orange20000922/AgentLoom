#include "connection_pool.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace net {
namespace detail {

struct ConnectionPoolEntry {
    ConnectionPoolEntry(ProtocolConnectionKind connection_kind,
                        ConnectionContext connection_context,
                        std::size_t memory_reserve_bytes,
                        std::size_t memory_blocks_per_slab,
                        core::ThreadPool* borrowed_task_pool)
        : context(std::move(connection_context)),
          kind(connection_kind),
          last_activity(std::chrono::steady_clock::now()),
          memory_pool(memory_reserve_bytes, memory_blocks_per_slab),
          task_pool(borrowed_task_pool) {}

    // context/memory_pool/task_pool 创建后不变；lease 关闭后仍由持有者保活，供迟到回调安全读取。
    ConnectionContext context;
    ProtocolConnectionKind kind = ProtocolConnectionKind::Http;
    std::chrono::steady_clock::time_point last_activity;
    core::BucketMemoryPool memory_pool;
    core::ThreadPool* task_pool = nullptr;
    std::atomic_bool active{true};
    ConnectionCloseInfo close_info;
};

struct ConnectionPoolState {
    explicit ConnectionPoolState(ConnectionPoolOptions pool_options)
        : options(std::move(pool_options)) {}

    core::Result<ConnectionLease> Acquire(ProtocolConnectionKind kind, ConnectionContext context) {
        if (context.connection_id == 0) {
            return core::Status::Error(core::ErrorCode::InvalidArgument, "connection id is required");
        }

        std::lock_guard lock(mutex);
        if (entries.find(context.connection_id) != entries.end()) {
            ++stats.rejected_connections;
            return core::Status::Error(core::ErrorCode::AlreadyExists, "connection already exists");
        }
        if (WouldExceedLimit(kind)) {
            ++stats.rejected_connections;
            return core::Status::Error(core::ErrorCode::ResourceExhausted, "connection pool limit reached");
        }

        auto owner = self.lock();
        if (!owner) {
            ++stats.rejected_connections;
            return core::Status::Error(core::ErrorCode::InternalError, "connection pool is not initialized");
        }

        std::shared_ptr<ConnectionPoolEntry> entry;
        try {
            entry = std::make_shared<ConnectionPoolEntry>(
                kind,
                std::move(context),
                options.memory_reserve_bytes,
                options.memory_blocks_per_slab,
                options.task_pool);
        } catch (const std::bad_alloc&) {
            ++stats.rejected_connections;
            return core::Status::Error(core::ErrorCode::OutOfMemory, "connection allocation failed");
        }

        ++stats.accepted_connections;
        ++stats.active_connections;
        IncrementKind(kind);
        entries.emplace(entry->context.connection_id, entry);
        return ConnectionLease(std::move(owner), std::move(entry));
    }

    void Release(std::uint64_t connection_id, ConnectionCloseInfo close_info) {
        std::lock_guard lock(mutex);
        auto it = entries.find(connection_id);
        if (it == entries.end()) {
            return;
        }

        auto& entry = *it->second;
        if (!entry.active.exchange(false, std::memory_order_acq_rel)) {
            entries.erase(it);
            return;
        }

        entry.close_info = std::move(close_info);
        if (stats.active_connections > 0) {
            --stats.active_connections;
        }
        DecrementKind(entry.kind);
        ++stats.closed_connections;
        entries.erase(it);
    }

    core::Status SetKind(std::uint64_t connection_id, ProtocolConnectionKind next_kind) {
        std::lock_guard lock(mutex);
        auto it = entries.find(connection_id);
        if (it == entries.end() || !it->second->active.load(std::memory_order_acquire)) {
            return core::Status::Error(core::ErrorCode::NotFound, "connection is not active");
        }

        auto& entry = *it->second;
        if (entry.kind == next_kind) {
            entry.last_activity = std::chrono::steady_clock::now();
            return core::Status::Ok();
        }
        if (WouldExceedKindLimit(next_kind)) {
            return core::Status::Error(core::ErrorCode::ResourceExhausted, "connection pool limit reached");
        }

        DecrementKind(entry.kind);
        entry.kind = next_kind;
        IncrementKind(next_kind);
        entry.last_activity = std::chrono::steady_clock::now();
        return core::Status::Ok();
    }

    void Touch(std::uint64_t connection_id) {
        std::lock_guard lock(mutex);
        auto it = entries.find(connection_id);
        if (it != entries.end() && it->second->active.load(std::memory_order_acquire)) {
            it->second->last_activity = std::chrono::steady_clock::now();
        }
    }

    void CloseAll(ConnectionCloseInfo close_info) {
        std::lock_guard lock(mutex);
        for (auto& [_, entry] : entries) {
            if (entry->active.exchange(false, std::memory_order_acq_rel)) {
                entry->close_info = close_info;
                ++stats.closed_connections;
            }
        }
        entries.clear();
        stats.active_connections = 0;
        stats.active_http_connections = 0;
        stats.active_websocket_connections = 0;
    }

    ConnectionPoolStats Stats() const {
        std::lock_guard lock(mutex);
        return stats;
    }

    std::vector<ConnectionSnapshot> Snapshots() const {
        std::lock_guard lock(mutex);
        std::vector<ConnectionSnapshot> snapshots;
        snapshots.reserve(entries.size());
        for (const auto& [_, entry] : entries) {
            if (!entry->active.load(std::memory_order_acquire)) {
                continue;
            }
            snapshots.push_back(ConnectionSnapshot{
                entry->context,
                entry->kind,
                entry->last_activity,
                entry->memory_pool.stats()});
        }
        std::sort(snapshots.begin(), snapshots.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.context.connection_id < rhs.context.connection_id;
        });
        return snapshots;
    }

    bool WouldExceedLimit(ProtocolConnectionKind kind) const noexcept {
        if (options.max_connections > 0 && stats.active_connections >= options.max_connections) {
            return true;
        }
        if (kind == ProtocolConnectionKind::Http &&
            options.max_http_connections > 0 &&
            stats.active_http_connections >= options.max_http_connections) {
            return true;
        }
        if (kind == ProtocolConnectionKind::WebSocket &&
            options.max_websocket_connections > 0 &&
            stats.active_websocket_connections >= options.max_websocket_connections) {
            return true;
        }
        return false;
    }

    bool WouldExceedKindLimit(ProtocolConnectionKind kind) const noexcept {
        if (kind == ProtocolConnectionKind::Http &&
            options.max_http_connections > 0 &&
            stats.active_http_connections >= options.max_http_connections) {
            return true;
        }
        if (kind == ProtocolConnectionKind::WebSocket &&
            options.max_websocket_connections > 0 &&
            stats.active_websocket_connections >= options.max_websocket_connections) {
            return true;
        }
        return false;
    }

    void IncrementKind(ProtocolConnectionKind kind) noexcept {
        if (kind == ProtocolConnectionKind::Http) {
            ++stats.active_http_connections;
        } else {
            ++stats.active_websocket_connections;
        }
    }

    void DecrementKind(ProtocolConnectionKind kind) noexcept {
        if (kind == ProtocolConnectionKind::Http) {
            if (stats.active_http_connections > 0) {
                --stats.active_http_connections;
            }
        } else {
            if (stats.active_websocket_connections > 0) {
                --stats.active_websocket_connections;
            }
        }
    }

    ConnectionPoolOptions options;
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionPoolEntry>> entries;
    ConnectionPoolStats stats;
    std::weak_ptr<ConnectionPoolState> self;
};

} // namespace detail

ConnectionLease::ConnectionLease(std::shared_ptr<detail::ConnectionPoolState> owner,
                                 std::shared_ptr<detail::ConnectionPoolEntry> entry) noexcept
    : owner_(std::move(owner)),
      entry_(std::move(entry)) {}

ConnectionLease::ConnectionLease(ConnectionLease&& other) noexcept
    : owner_(std::move(other.owner_)),
      entry_(std::move(other.entry_)) {}

ConnectionLease& ConnectionLease::operator=(ConnectionLease&& other) noexcept {
    if (this != &other) {
        Close(ConnectionCloseInfo::Remote("connection lease reassigned"));
        owner_ = std::move(other.owner_);
        entry_ = std::move(other.entry_);
    }
    return *this;
}

ConnectionLease::~ConnectionLease() {
    Close(ConnectionCloseInfo::Remote("connection lease released"));
}

bool ConnectionLease::valid() const noexcept {
    return owner_ && entry_ && entry_->active.load(std::memory_order_acquire);
}

ConnectionLease::operator bool() const noexcept {
    return valid();
}

std::uint64_t ConnectionLease::connection_id() const noexcept {
    return entry_ ? entry_->context.connection_id : 0;
}

const ConnectionContext& ConnectionLease::context() const noexcept {
    static const ConnectionContext empty;
    return entry_ ? entry_->context : empty;
}

ProtocolConnectionKind ConnectionLease::kind() const noexcept {
    return entry_ ? entry_->kind : ProtocolConnectionKind::Http;
}

core::RawMemoryPool& ConnectionLease::memory_pool() noexcept {
    return entry_->memory_pool;
}

const core::RawMemoryPool& ConnectionLease::memory_pool() const noexcept {
    return entry_->memory_pool;
}

core::ThreadPool* ConnectionLease::task_pool() const noexcept {
    // 读取 entry 上的不可变快照，避免与 Close() 重置 owner_ 产生跨线程竞争。
    return entry_ ? entry_->task_pool : nullptr;
}

core::Status ConnectionLease::SetKind(ProtocolConnectionKind kind) {
    if (!owner_ || !entry_) {
        return core::Status::Error(core::ErrorCode::NotFound, "connection is not active");
    }
    auto status = owner_->SetKind(entry_->context.connection_id, kind);
    if (status.ok()) {
        entry_->kind = kind;
    }
    return status;
}

void ConnectionLease::Touch() {
    if (owner_ && entry_) {
        owner_->Touch(entry_->context.connection_id);
    }
}

void ConnectionLease::Close(ConnectionCloseInfo close_info) {
    // 关闭只归还连接配额并从池中摘除；entry 随 lease 析构释放，
    // 关闭后的 context()/memory_pool()/task_pool() 仍指向有效对象（#67）。
    if (!owner_) {
        return;
    }
    if (entry_) {
        owner_->Release(entry_->context.connection_id, std::move(close_info));
    }
    owner_.reset();
}

ConnectionPool::ConnectionPool(ConnectionPoolOptions options)
    : state_(std::make_shared<detail::ConnectionPoolState>(std::move(options))) {
    state_->self = state_;
}

ConnectionPool::~ConnectionPool() {
    CloseAll(ConnectionCloseInfo::Shutdown("connection pool destroyed"));
}

core::Result<ConnectionLease> ConnectionPool::Acquire(ProtocolConnectionKind kind, ConnectionContext context) {
    return state_->Acquire(kind, std::move(context));
}

void ConnectionPool::CloseAll(ConnectionCloseInfo close_info) {
    state_->CloseAll(std::move(close_info));
}

ConnectionPoolStats ConnectionPool::Stats() const {
    return state_->Stats();
}

std::vector<ConnectionSnapshot> ConnectionPool::Snapshots() const {
    return state_->Snapshots();
}

} // namespace net
