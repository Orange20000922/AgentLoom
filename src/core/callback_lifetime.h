#pragma once

#include "result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <exception>
#include <new>
#include <mutex>
#include <utility>

namespace core {

// 独立于 scheduler lane/quota 的退出屏障。正常 callback 路径不加锁、不等待、不通知。
class CallbackLifetime final {
    struct State {
        static constexpr unsigned kClosed = 1u << (std::numeric_limits<unsigned>::digits - 1);
        static constexpr unsigned kCount = kClosed - 1;
        std::atomic<unsigned> admission{0};
        std::mutex wait_mutex;
        std::condition_variable wait_cv;
    };

public:
    class Lease final {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) noexcept = default;
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                Reset();
                state_ = std::move(other.state_);
            }
            return *this;
        }
        ~Lease() { Reset(); }

        void Reset() noexcept {
            if (state_) {
                state_->admission.fetch_sub(1, std::memory_order_acq_rel);
                state_.reset();
            }
        }

    private:
        friend class CallbackLifetime;
        explicit Lease(std::shared_ptr<State> state) : state_(std::move(state)) {}
        // shared ownership 仅管理屏障对象，不替代被借用的 Server 生命周期。
        std::shared_ptr<State> state_;
    };

    CallbackLifetime() : state_(std::make_shared<State>()) {}
    CallbackLifetime(const CallbackLifetime&) = delete;
    CallbackLifetime& operator=(const CallbackLifetime&) = delete;

    Result<Lease> TryAcquire() {
        auto observed = state_->admission.load(std::memory_order_acquire);
        for (;;) {
            if (observed & State::kClosed) {
                return Status::Error(ErrorCode::Cancelled, "callback admission is closed");
            }
            if ((observed & State::kCount) == State::kCount) {
                return Status::Error(ErrorCode::ResourceExhausted, "callback lifetime count exhausted");
            }
            // 关闭位与计数共用 CAS，不能在 Stop 已观察到零后才登记迟到 callback。
            if (state_->admission.compare_exchange_weak(
                    observed, observed + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return Lease(state_);
            }
        }
    }

    void CloseAdmission() noexcept {
        state_->admission.fetch_or(State::kClosed, std::memory_order_acq_rel);
    }

    unsigned outstanding() const noexcept {
        return state_->admission.load(std::memory_order_acquire) & State::kCount;
    }

    // 只能在 callback 外部的停服线程调用；超时不授权销毁仍被 callback 借用的资源。
    Status WaitUntil(std::chrono::steady_clock::time_point deadline) const {
        if (!(state_->admission.load(std::memory_order_acquire) & State::kClosed)) {
            return Status::Error(ErrorCode::FailedPrecondition, "close callback admission before waiting");
        }
        std::unique_lock lock(state_->wait_mutex);
        while (outstanding() != 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return Status::Error(ErrorCode::Timeout, "callback lifetime drain timed out");
            }
            // 完成端仅改原子值：2ms 检查间隔避免依赖 notify；实际唤醒延迟取决于系统调度。
            const auto next = now + std::chrono::milliseconds(2);
            state_->wait_cv.wait_until(lock, next < deadline ? next : deadline);
        }
        return Status::Ok();
    }

    void Wait() const {
        static_cast<void>(WaitUntil(std::chrono::steady_clock::time_point::max()));
    }

    template <typename... Args>
    Result<std::function<void(Args...)>> Track(std::function<void(Args...)> callback) {
        if (!callback) {
            return Status::Error(ErrorCode::InvalidArgument, "tracked callback is required");
        }
        auto acquired = TryAcquire();
        if (!acquired.ok()) return acquired.status();
        struct Completion {
            Completion(Lease value, std::function<void(Args...)> function)
                : lease(std::move(value)), callback(std::move(function)) {}
            // 逆序释放：callback 的捕获先销毁，再归还租约。
            Lease lease;
            std::function<void(Args...)> callback;
            std::atomic<bool> invoked{false};
        };
        try {
            // 延续 core coroutine/task 的 shared ownership；现有 ObjectPool 借用裸 owner，
            // 无法覆盖 provider 保留 callback 空壳的生命周期，暂不引入新的共享 allocator。
            auto completion = std::make_shared<Completion>(
                std::move(acquired).value(), std::move(callback));
            return std::function<void(Args...)>([completion](Args... args) {
                if (completion->invoked.exchange(true, std::memory_order_acq_rel)) return;
                auto lease = std::move(completion->lease);
                auto callback = std::move(completion->callback);
                // Provider 可以保留这个空壳；callback 返回/抛异常后仍能释放捕获和租约。
                callback(std::forward<Args>(args)...);
            });
        } catch (const std::bad_alloc&) {
            return Status::Error(ErrorCode::OutOfMemory, "callback lifetime allocation failed");
        } catch (const std::exception& error) {
            return Status::Error(ErrorCode::InternalError, error.what());
        }
    }

private:
    std::shared_ptr<State> state_;
};

}
