#include "callback_lifetime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

// 测试线程专门制造停服竞争；生产完成路径没有新增线程、等待或通知。
TEST(CallbackLifetimeTest, ClosedAdmissionWaitsForCallbackExitAndCaptureDestruction) {
    core::CallbackLifetime lifetime;
    auto captured = std::make_shared<int>(42);
    std::weak_ptr<int> weak = captured;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto tracked = lifetime.Track<>(std::function<void()>([captured, &entered, released] {
        entered.set_value();
        released.wait();
    }));
    ASSERT_TRUE(tracked.ok());
    auto callback = std::move(tracked).value();
    captured.reset();
    auto completed = std::async(std::launch::async, [&] { callback(); });
    const auto entered_status = entered.get_future().wait_for(2s);
    EXPECT_EQ(entered_status, std::future_status::ready);
    lifetime.CloseAdmission();
    EXPECT_EQ(lifetime.TryAcquire().status().code(), core::ErrorCode::Cancelled);
    EXPECT_EQ(lifetime.WaitUntil(std::chrono::steady_clock::now() + 10ms).code(),
              core::ErrorCode::Timeout);
    EXPECT_FALSE(weak.expired());
    release.set_value();
    completed.get();
    EXPECT_TRUE(lifetime.WaitUntil(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_TRUE(weak.expired());
    // callback 空壳被 provider 保存时，不继续计数或持有业务捕获；重复调用无效。
    callback();
    EXPECT_EQ(lifetime.outstanding(), 0u);
}

TEST(CallbackLifetimeTest, ExceptionAndRejectedSubmissionReleaseCapturesBeforeLease) {
    core::CallbackLifetime lifetime;
    auto captured = std::make_shared<int>(7);
    std::weak_ptr<int> weak = captured;
    auto tracked = lifetime.Track<>(std::function<void()>([captured] {
        throw std::runtime_error("callback");
    }));
    ASSERT_TRUE(tracked.ok());
    captured.reset();
    auto callback = std::move(tracked).value();
    EXPECT_THROW(callback(), std::runtime_error);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(lifetime.outstanding(), 0u);
    auto dropped = lifetime.Track<>(std::function<void()>([] {}));
    ASSERT_TRUE(dropped.ok());
    EXPECT_EQ(lifetime.outstanding(), 1u);
    std::move(dropped).value() = {};
    lifetime.CloseAdmission();
    EXPECT_TRUE(lifetime.WaitUntil(std::chrono::steady_clock::now() + 1s).ok());
}

TEST(CallbackLifetimeTest, CloseAndConcurrentAcquireCannotAdmitAfterDrain) {
    core::CallbackLifetime lifetime;
    std::promise<void> go;
    auto started = go.get_future().share();
    std::vector<std::thread> workers;
    std::atomic<unsigned> completed{0};
    for (unsigned i = 0; i < 8; ++i) {
        workers.emplace_back([&lifetime, started, &completed] {
            started.wait();
            for (unsigned j = 0; j < 1000; ++j) {
                auto acquired = lifetime.TryAcquire();
                if (!acquired.ok()) break;
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.set_value();
    lifetime.CloseAdmission();
    for (auto& worker : workers) worker.join();
    EXPECT_TRUE(lifetime.WaitUntil(std::chrono::steady_clock::now() + 1s).ok());
    EXPECT_EQ(lifetime.outstanding(), 0u);
    EXPECT_EQ(lifetime.TryAcquire().status().code(), core::ErrorCode::Cancelled);
}

TEST(CallbackLifetimeTest, InstancesAndMovedLeasesAreIndependent) {
    core::CallbackLifetime first, second;
    EXPECT_EQ(first.WaitUntil(std::chrono::steady_clock::now()).code(),
              core::ErrorCode::FailedPrecondition);
    auto acquired = first.TryAcquire();
    ASSERT_TRUE(acquired.ok());
    auto lease = std::move(acquired).value();
    core::CallbackLifetime::Lease moved;
    moved = std::move(lease);
    first.CloseAdmission();
    EXPECT_EQ(first.outstanding(), 1u);
    EXPECT_TRUE(second.TryAcquire().ok());
    moved.Reset();
    first.Wait();
    EXPECT_EQ(first.outstanding(), 0u);
}

TEST(CallbackLifetimeTest, CompletionBurstRecordsContentionWithoutCallbackMutex) {
    core::CallbackLifetime lifetime;
    std::atomic<unsigned> completed{0};
    std::vector<std::function<void()>> callbacks;
    for (unsigned i = 0; i < 4096; ++i) {
        auto tracked = lifetime.Track<>(std::function<void()>([&] {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
        ASSERT_TRUE(tracked.ok());
        callbacks.push_back(std::move(tracked).value());
    }
    lifetime.CloseAdmission();
    const auto begin = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned shard = 0; shard < 8; ++shard) {
        workers.emplace_back([&, shard] {
            for (std::size_t i = shard; i < callbacks.size(); i += 8) callbacks[i]();
        });
    }
    for (auto& worker : workers) worker.join();
    lifetime.Wait();
    RecordProperty("completion_burst_us", std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count()));
    EXPECT_EQ(completed.load(), 4096u);
    EXPECT_EQ(lifetime.outstanding(), 0u);
}
}
