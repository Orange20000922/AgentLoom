#include "persona_runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <future>
#include <thread>

namespace {

using agent::semantic_cache::CacheLookupRequest;
using agent::semantic_cache::CacheLookupResult;
using agent::semantic_cache::CacheStoreRequest;
using agent::service::persona::AsyncMemoryContextRequest;
using agent::service::persona::ConversationTurn;
using agent::service::persona::RecalledContext;
using agent::service::persona::SemanticMemoryContextProvider;

class ManualAsyncSemanticCache final
    : public agent::semantic_cache::ISemanticCache,
      public agent::semantic_cache::IAsyncSemanticCache {
public:
    core::Result<CacheLookupResult> Lookup(const CacheLookupRequest&) override {
        return core::Status::Error(core::ErrorCode::InternalError,
                                   "unexpected synchronous lookup");
    }

    core::Status Store(const CacheStoreRequest&) override {
        return core::Status::Ok();
    }

    core::Status LookupAsync(CacheLookupRequest request,
                             LookupCompletion completion) override {
        if (complete_synchronously) {
            CacheLookupResult result;
            result.hit = true;
            result.payload = "sync-memory";
            completion(std::move(result));
            if (complete_twice) {
                CacheLookupResult duplicate;
                duplicate.hit = true;
                duplicate.payload = "duplicate-memory";
                completion(std::move(duplicate));
            }
            return start_status;
        }
        if (!start_status.ok()) {
            return start_status;
        }
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(std::move(request));
            completions_.push_back(std::move(completion));
        }
        condition_.notify_all();
        return core::Status::Ok();
    }

    bool WaitForCount(std::size_t count) const {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return requests_.size() >= count;
        });
    }

    CacheLookupRequest Request(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return requests_.at(index);
    }

    void Complete(std::size_t index, core::Result<CacheLookupResult> result) {
        LookupCompletion completion;
        {
            std::lock_guard lock(mutex_);
            completion = completions_.at(index);
        }
        completion(std::move(result));
    }

    core::Status start_status = core::Status::Ok();
    bool complete_synchronously = false;
    bool complete_twice = false;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::vector<CacheLookupRequest> requests_;
    std::vector<LookupCompletion> completions_;
};

AsyncMemoryContextRequest MakeRequest() {
    AsyncMemoryContextRequest request;
    request.session_id = "session-a";
    request.tenant_id = "tenant-a";
    request.user_uuid = "user-a";
    request.persona_id = "persona-a";
    request.query = "当前问题";
    request.trace_id = "trace-a";
    request.max_recent_turns = 2;
    request.current_session_recent = {
        ConversationTurn{.user_input = "u1", .response = "a1"},
        ConversationTurn{.user_input = "u2", .response = "a2", .turn_id = "session-a:2"},
        ConversationTurn{.user_input = "u3", .response = "a3", .turn_id = "session-a:3"},
    };
    return request;
}

TEST(SemanticMemoryContextProviderTest, PreservesScopeAndCompletesAcceptedLookupOnce) {
    auto cache = std::make_shared<ManualAsyncSemanticCache>();
    SemanticMemoryContextProvider provider(cache);

    std::mutex mutex;
    std::optional<RecalledContext> observed;
    std::atomic<int> callback_count{0};
    auto status = provider.BuildContextAsync(
        MakeRequest(),
        [&](core::Result<RecalledContext> result) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
            ASSERT_TRUE(result.ok()) << result.status().message();
            std::lock_guard lock(mutex);
            observed = std::move(result).value();
        });

    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_TRUE(cache->WaitForCount(1));
    const auto lookup = cache->Request(0);
    EXPECT_EQ(lookup.session_id, "session-a");
    EXPECT_EQ(lookup.tenant_id, "tenant-a");
    EXPECT_EQ(lookup.user_id, "user-a");
    EXPECT_EQ(lookup.persona_id, "persona-a");
    EXPECT_EQ(lookup.extra.at("trace_id"), "trace-a");
    ASSERT_EQ(lookup.recent_turns.size(), 2u);
    EXPECT_EQ(lookup.recent_turn_ids, (std::vector<std::string>{"session-a:2", "session-a:3"}));
    EXPECT_NE(lookup.recent_turns[0].find("u2"), std::string::npos);
    EXPECT_NE(lookup.recent_turns[1].find("u3"), std::string::npos);

    CacheLookupResult hit;
    hit.hit = true;
    hit.payload = "tenant-a-user-a-memory";
    cache->Complete(0, std::move(hit));
    CacheLookupResult duplicate;
    duplicate.hit = true;
    duplicate.payload = "must-not-replace";
    cache->Complete(0, std::move(duplicate));

    EXPECT_EQ(callback_count.load(std::memory_order_relaxed), 1);
    std::lock_guard lock(mutex);
    ASSERT_TRUE(observed.has_value());
    EXPECT_TRUE(observed->l0_hit);
    EXPECT_EQ(observed->recent_turns.size(), 2u);
    EXPECT_NE(observed->system_context.find("tenant-a-user-a-memory"), std::string::npos);
    EXPECT_EQ(observed->system_context.find("must-not-replace"), std::string::npos);
}

TEST(SemanticMemoryContextProviderTest, DispatchesContinuationOffInlineL0Callback) {
    auto cache = std::make_shared<ManualAsyncSemanticCache>();
    cache->complete_synchronously = true;
    core::ThreadPool io({1, 8, "memory-context-test"});
    ASSERT_TRUE(io.Start().ok());
    SemanticMemoryContextProvider provider(cache);
    provider.ConfigureBlockingPool(io);
    std::promise<std::thread::id> done;
    auto future = done.get_future();
    ASSERT_TRUE(provider.BuildContextAsync(MakeRequest(), [&](auto result) {
        EXPECT_TRUE(result.ok());
        done.set_value(std::this_thread::get_id());
    }).ok());
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_NE(future.get(), std::this_thread::get_id());
    io.Shutdown(true);
}

TEST(SemanticMemoryContextProviderTest, ReturnsStartFailureWithoutCompletion) {
    auto cache = std::make_shared<ManualAsyncSemanticCache>();
    cache->start_status = core::Status::Error(
        core::ErrorCode::ResourceExhausted,
        "batch queue full");
    SemanticMemoryContextProvider provider(cache);
    std::atomic<int> callback_count{0};

    auto status = provider.BuildContextAsync(
        MakeRequest(),
        [&](core::Result<RecalledContext>) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), core::ErrorCode::ResourceExhausted);
    EXPECT_EQ(callback_count.load(std::memory_order_relaxed), 0);
}

TEST(SemanticMemoryContextProviderTest, DeliversLookupFailureExactlyOnce) {
    auto cache = std::make_shared<ManualAsyncSemanticCache>();
    SemanticMemoryContextProvider provider(cache);
    std::atomic<int> callback_count{0};
    core::ErrorCode observed = core::ErrorCode::Ok;

    ASSERT_TRUE(provider.BuildContextAsync(
        MakeRequest(),
        [&](core::Result<RecalledContext> result) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
            ASSERT_FALSE(result.ok());
            observed = result.status().code();
        }).ok());
    ASSERT_TRUE(cache->WaitForCount(1));
    cache->Complete(0, core::Status::Error(core::ErrorCode::Unavailable, "model unavailable"));
    cache->Complete(0, core::Status::Error(core::ErrorCode::InternalError, "duplicate"));

    EXPECT_EQ(callback_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(observed, core::ErrorCode::Unavailable);
}

TEST(SemanticMemoryContextProviderTest, PrefersSynchronousCompletionOverInvalidErrorReturn) {
    auto cache = std::make_shared<ManualAsyncSemanticCache>();
    cache->complete_synchronously = true;
    cache->complete_twice = true;
    cache->start_status = core::Status::Error(
        core::ErrorCode::InternalError,
        "invalid provider behavior after callback");
    SemanticMemoryContextProvider provider(cache);
    std::atomic<int> callback_count{0};
    std::string context;

    const auto status = provider.BuildContextAsync(
        MakeRequest(),
        [&](core::Result<RecalledContext> result) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
            ASSERT_TRUE(result.ok()) << result.status().message();
            context = std::move(result).value().system_context;
        });

    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(callback_count.load(std::memory_order_relaxed), 1);
    EXPECT_NE(context.find("sync-memory"), std::string::npos);
    EXPECT_EQ(context.find("duplicate-memory"), std::string::npos);
}

}
