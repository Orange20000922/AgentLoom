#include "chat_sse_registry.h"

#include <algorithm>
#include <iterator>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace agent::service::gateway {
namespace {

class ReplayChannel final : public IChatSseChannel {
public:
    ReplayChannel(ChatSseReplayOptions limits, std::function<void()> cancel)
        : limits_(limits), cancel_(std::move(cancel)) {}

    core::Status Publish(::net::ServerSentEvent event) override {
        std::lock_guard lock(mutex_);
        if (complete_ || cancelled_)
            return core::Status::Error(core::ErrorCode::Cancelled, "SSE turn is no longer publishing");
        auto stream = stream_.lock();
        auto status = stream ? stream->SendEvent(event)
                             : core::Status::Error(core::ErrorCode::Cancelled, "SSE turn has no connected consumer");
        if (status.ok()) Retain(event);
        if (!status.ok() && delivery_status_.ok()) delivery_status_ = status;
        return status;
    }

    core::Status Complete(::net::ServerSentEvent event) override {
        std::lock_guard lock(mutex_);
        if (complete_) return core::Status::Error(core::ErrorCode::AlreadyExists, "SSE turn already has terminal event");
        complete_ = true;
        expires_ = std::chrono::steady_clock::now() + limits_.terminal_retention;
        Retain(event);
        if (auto stream = stream_.lock()) {
            auto status = stream->SendEvent(std::move(event));
            stream->FinishEvents();
            return status;
        }
        return core::Status::Ok();
    }

    void Disconnected(std::uint64_t connection_id) override {
        std::function<void()> cancel;
        {
            std::lock_guard lock(mutex_);
            if (connection_id != connection_id_) return;
            stream_.reset();
            if (!complete_ && !cancelled_) {
                cancelled_ = true;
                cancel = cancel_;
            }
        }
        if (cancel) cancel();
    }

    core::Status Connect(std::shared_ptr<::net::IServerEventStream> stream, std::string_view last_id) override {
        if (!stream) return core::Status::Error(core::ErrorCode::InvalidArgument, "SSE stream is required");
        std::lock_guard lock(mutex_);
        if (!stream_.expired()) return core::Status::Error(core::ErrorCode::AlreadyExists, "SSE turn already connected");
        auto begin = events_.begin();
        if (!last_id.empty()) {
            auto found = std::find_if(begin, events_.end(), [last_id](const auto& event) { return event.id == last_id; });
            if (found == events_.end())
                return core::Status::Error(core::ErrorCode::FailedPrecondition, "Last-Event-ID is unknown or no longer retained");
            begin = std::next(found);
        } else if (evicted_) {
            return core::Status::Error(core::ErrorCode::FailedPrecondition, "SSE replay prefix is no longer retained");
        }
        for (auto it = begin; it != events_.end(); ++it)
            if (auto status = stream->SendEvent(*it); !status.ok()) return status;
        connection_id_ = stream->stream_connection_id();
        stream_ = stream;
        if (complete_) stream->FinishEvents();
        return core::Status::Ok();
    }

    core::Status Pong(std::uint64_t connection_id) {
        std::lock_guard lock(mutex_);
        if (connection_id != connection_id_ || complete_)
            return core::Status::Error(core::ErrorCode::NotFound, "SSE stream is not active");
        auto stream = stream_.lock();
        return stream ? stream->AcknowledgeHeartbeat()
                      : core::Status::Error(core::ErrorCode::NotFound, "SSE stream is disconnected");
    }

    bool Expired() const {
        std::lock_guard lock(mutex_);
        return complete_ && std::chrono::steady_clock::now() >= expires_;
    }
    ::net::BackpressureStats QueueStats() const override {
        std::lock_guard lock(mutex_);
        auto stream = stream_.lock();
        return stream ? stream->EventQueueStats() : ::net::BackpressureStats{};
    }
    core::Status DeliveryStatus() const override {
        std::lock_guard lock(mutex_);
        return delivery_status_;
    }
    core::Status CheckResume(std::string_view last_id) const override {
        std::lock_guard lock(mutex_);
        if (!stream_.expired()) return core::Status::Error(core::ErrorCode::AlreadyExists, "SSE turn already connected");
        if (last_id.empty()) return evicted_
            ? core::Status::Error(core::ErrorCode::FailedPrecondition, "SSE replay prefix is no longer retained")
            : core::Status::Ok();
        if (std::none_of(events_.begin(), events_.end(), [last_id](const auto& event) { return event.id == last_id; }))
            return core::Status::Error(core::ErrorCode::FailedPrecondition, "Last-Event-ID is unknown or no longer retained");
        return core::Status::Ok();
    }
    std::optional<std::chrono::steady_clock::time_point> RetentionDeadline() const {
        std::lock_guard lock(mutex_);
        return complete_ ? std::optional(expires_) : std::nullopt;
    }

private:
    void Retain(const ::net::ServerSentEvent& event) {
        const auto bytes = event.data.size() + event.event.size() + event.id.size();
        while (!events_.empty() && (events_.size() >= limits_.max_events_per_turn ||
                                    retained_bytes_ + bytes > limits_.max_bytes_per_turn)) {
            const auto& oldest = events_.front();
            retained_bytes_ -= oldest.data.size() + oldest.event.size() + oldest.id.size();
            events_.pop_front();
            evicted_ = true;
        }
        if (bytes <= limits_.max_bytes_per_turn && limits_.max_events_per_turn) {
            events_.push_back(event);
            retained_bytes_ += bytes;
        } else {
            evicted_ = true;
        }
    }

    ChatSseReplayOptions limits_;
    std::function<void()> cancel_;
    mutable std::mutex mutex_;
    std::weak_ptr<::net::IServerEventStream> stream_;
    std::uint64_t connection_id_ = 0;
    std::deque<::net::ServerSentEvent> events_;
    std::size_t retained_bytes_ = 0;
    bool complete_ = false, cancelled_ = false, evicted_ = false;
    core::Status delivery_status_;
    std::chrono::steady_clock::time_point expires_;
};
}

struct ChatSseRegistry::Impl {
    struct Entry { std::string owner; std::shared_ptr<ReplayChannel> channel; };
    explicit Impl(ChatSseReplayOptions limits) : options(limits) {}
    void Prune() { std::erase_if(entries, [](const auto& pair) { return pair.second.channel->Expired(); }); }
    ChatSseReplayOptions options;
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
};

ChatSseRegistry::ChatSseRegistry(ChatSseReplayOptions options) : impl_(std::make_unique<Impl>(options)) {}
ChatSseRegistry::~ChatSseRegistry() = default;

core::Result<std::shared_ptr<IChatSseChannel>> ChatSseRegistry::Begin(
    std::string request_id, std::string owner, std::function<void()> cancel) {
    if (request_id.empty() || owner.empty() || !cancel || !impl_->options.max_turns ||
        !impl_->options.max_events_per_turn || !impl_->options.max_bytes_per_turn ||
        impl_->options.terminal_retention.count() <= 0)
        return core::Status::Error(core::ErrorCode::InvalidArgument, "invalid SSE replay options or identity");
    std::lock_guard lock(impl_->mutex);
    impl_->Prune();
    if (impl_->entries.contains(request_id))
        return core::Status::Error(core::ErrorCode::AlreadyExists, "SSE requestId is already retained; use resume endpoint");
    if (impl_->entries.size() >= impl_->options.max_turns) {
        auto oldest = impl_->entries.end();
        std::optional<std::chrono::steady_clock::time_point> deadline;
        for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it) {
            const auto candidate = it->second.channel->RetentionDeadline();
            if (candidate && (!deadline || *candidate < *deadline)) { oldest = it; deadline = candidate; }
        }
        // 容量紧张时淘汰最旧终态；活跃 Turn 仍受 admission 上限保护。
        if (oldest != impl_->entries.end()) impl_->entries.erase(oldest);
    }
    if (impl_->entries.size() >= impl_->options.max_turns)
        return core::Status::Error(core::ErrorCode::ResourceExhausted, "SSE replay turn limit reached");
    auto channel = std::make_shared<ReplayChannel>(impl_->options, std::move(cancel));
    impl_->entries.emplace(std::move(request_id), Impl::Entry{std::move(owner), channel});
    return std::shared_ptr<IChatSseChannel>(channel);
}

core::Result<std::shared_ptr<IChatSseChannel>> ChatSseRegistry::Find(
    std::string_view request_id, std::string_view owner) {
    std::lock_guard lock(impl_->mutex);
    impl_->Prune();
    const auto found = impl_->entries.find(std::string(request_id));
    if (found == impl_->entries.end()) return core::Status::Error(core::ErrorCode::NotFound, "SSE replay expired or missing");
    if (found->second.owner != owner) return core::Status::Error(core::ErrorCode::PermissionDenied, "SSE replay owner mismatch");
    return std::shared_ptr<IChatSseChannel>(found->second.channel);
}

core::Status ChatSseRegistry::Pong(std::uint64_t connection_id, std::string_view owner) {
    std::lock_guard lock(impl_->mutex);
    impl_->Prune();
    for (const auto& [id, entry] : impl_->entries) {
        if (entry.owner == owner) {
            auto status = entry.channel->Pong(connection_id);
            if (status.ok()) return status;
        }
    }
    return core::Status::Error(core::ErrorCode::NotFound, "SSE connection is not owned or active");
}

}
