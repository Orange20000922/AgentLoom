#pragma once

#include "result.h"
#include <algorithm>

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace net {

struct BackpressureOptions {
    std::size_t max_items = 0;
    std::size_t max_bytes = 0;
};

struct BackpressureStats {
    std::size_t queued_items = 0;
    std::size_t queued_bytes = 0;
    std::size_t pushed_items = 0;
    std::size_t popped_items = 0;
    std::size_t rejected_items = 0;
    bool closed = false;
    std::size_t peak_queued_items = 0;
    std::size_t peak_queued_bytes = 0;
};

template <typename T>
class BackpressureQueue {
public:
    using SizeFunction = std::function<std::size_t(const T&)>;

    BackpressureQueue(BackpressureOptions options, SizeFunction size_function)
        : options_(options), size_function_(std::move(size_function)) {}

    BackpressureQueue(const BackpressureQueue&) = delete;
    BackpressureQueue& operator=(const BackpressureQueue&) = delete;

    core::Status TryPush(T value) {
        const auto item_size = size_function_ ? size_function_(value) : 0;
        std::lock_guard lock(mutex_);
        if (closed_) {
            ++rejected_items_;
            return core::Status::Error(core::ErrorCode::Cancelled, "queue is closed");
        }
        if (options_.max_items > 0 && queue_.size() >= options_.max_items) {
            ++rejected_items_;
            return core::Status::Error(core::ErrorCode::ResourceExhausted, "backpressure item limit reached");
        }
        if (options_.max_bytes > 0 && (item_size > options_.max_bytes ||
                                      queued_bytes_ > options_.max_bytes - item_size)) {
            ++rejected_items_;
            return core::Status::Error(core::ErrorCode::ResourceExhausted, "backpressure byte limit reached");
        }

        queued_bytes_ += item_size;
        queue_.push_back(Entry{std::move(value), item_size});
        peak_queued_items_ = std::max(peak_queued_items_, queue_.size());
        peak_queued_bytes_ = std::max(peak_queued_bytes_, queued_bytes_);
        ++pushed_items_;
        return core::Status::Ok();
    }

    core::Result<T> TryPop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return core::Status::Error(core::ErrorCode::NotFound, "queue is empty");
        }

        auto entry = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= entry.size;
        ++popped_items_;
        return std::move(entry.value);
    }

    void Close(bool discard = false) {
        std::lock_guard lock(mutex_);
        closed_ = true;
        if (discard) {
            queue_.clear();
            queued_bytes_ = 0;
        }
    }

    BackpressureStats Stats() const {
        std::lock_guard lock(mutex_);
        return {queue_.size(), queued_bytes_, pushed_items_, popped_items_, rejected_items_, closed_,
                peak_queued_items_, peak_queued_bytes_};
    }

private:
    struct Entry {
        T value;
        std::size_t size = 0;
    };

    BackpressureOptions options_;
    SizeFunction size_function_;
    mutable std::mutex mutex_;
    std::deque<Entry> queue_;
    std::size_t queued_bytes_ = 0;
    std::size_t pushed_items_ = 0;
    std::size_t popped_items_ = 0;
    std::size_t rejected_items_ = 0;
    bool closed_ = false;
    std::size_t peak_queued_items_ = 0;
    std::size_t peak_queued_bytes_ = 0;
};

} // namespace net
