#include "persona_runtime.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <utility>

namespace agent::service::persona {
namespace {
std::string FormatRecentTurn(const ConversationTurn& turn) {
    return "用户: " + turn.user_input + "\n助手: " + turn.response;
}
std::string FormatL3Facts(const std::vector<vector_storage::EntryRecord>& facts) {
    if (facts.empty()) {
        return {};
    }
    std::string out = "<memory_l3>\n";
    for (const auto& fact : facts) {
        if (!fact.payload.empty()) {
            out += "- " + fact.payload + "\n";
        }
    }
    out += "</memory_l3>";
    return out;
}
std::vector<ConversationTurn> TakeRecent(std::span<const ConversationTurn> turns, std::size_t limit) {
    std::vector<ConversationTurn> out;
    const auto count = std::min<std::size_t>(turns.size(), limit);
    out.reserve(count);
    const auto begin = turns.size() - count;
    for (std::size_t i = begin; i < turns.size(); ++i) {
        out.push_back(turns[i]);
    }
    return out;
}
/// 将 provider 的同步完成、异步完成和异常实现统一收敛为一次外部结果。
class MemoryBuildCompletionState final {
public:
    explicit MemoryBuildCompletionState(
        IAsyncMemoryContextProvider::BuildCompletion completion)
        : completion_(std::move(completion)) {}
    bool TryComplete(core::Result<RecalledContext> result) {
        bool expected = false;
        if (!completed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        // 状态可能被下游缓存持有；先移出回调，确保完成后及时释放 Runtime/session 捕获。
        auto completion = std::move(completion_);
        completion(std::move(result));
        return true;
    }
    bool CancelWithoutCompletion() noexcept {
        bool expected = false;
        return completed_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel);
    }
private:
    std::atomic<bool> completed_{false};
    IAsyncMemoryContextProvider::BuildCompletion completion_;
};
}

// 复用 L0 异步缓存与宿主 I/O pool；存储一致性由原 provider 保证。
SemanticMemoryContextProvider::SemanticMemoryContextProvider(
    std::shared_ptr<semantic_cache::ISemanticCache> l0_cache,
    std::shared_ptr<memory::LongTermMemoryCompressor> l3_memory,
    SemanticMemoryContextProviderOptions options)
    : l0_cache_(std::move(l0_cache)),
      l3_memory_(std::move(l3_memory)),
      options_(options) {}
core::Result<RecalledContext> SemanticMemoryContextProvider::BuildContext(
    const MemoryContextRequest& request) {
    RecalledContext context;
    const auto recent_limit = request.max_recent_turns == 0
        ? options_.max_recent_turns
        : request.max_recent_turns;
    context.recent_turns = TakeRecent(request.current_session_recent, recent_limit);
    std::vector<std::string> sections;
    if (l0_cache_) {
        semantic_cache::CacheLookupRequest lookup;
        lookup.text = request.query;
        lookup.scope = semantic_cache::CacheScope::User;
        lookup.answer_type = semantic_cache::AnswerType::Personalized;
        lookup.tenant_id = request.tenant_id;
        lookup.user_id = request.user_uuid;
        lookup.session_id = request.session_id;
        lookup.persona_id = request.persona_id;
        lookup.extra["trace_id"] = request.trace_id;
        lookup.recent_turns.reserve(context.recent_turns.size());
        lookup.recent_turn_ids.reserve(context.recent_turns.size());
        for (const auto& turn : context.recent_turns) {
            lookup.recent_turns.push_back(FormatRecentTurn(turn));
            lookup.recent_turn_ids.push_back(turn.turn_id);
        }
        auto l0 = l0_cache_->Lookup(lookup);
        if (!l0.ok()) {
            return l0.status();
        }
        if (l0.value().hit && !l0.value().payload.empty()) {
            context.l0_hit = true;
            sections.push_back("<memory_l0>\n" + l0.value().payload + "\n</memory_l0>");
            if (l0.value().memory_records.empty()) context.items.push_back({ContextSource::L0, {}, sections.back()});
            else for (const auto& record : l0.value().memory_records)
                context.items.push_back({ContextSource::L0, record.record_id, record.content});
        }
    }
    if (l3_memory_) {
        auto facts = l3_memory_->SearchFacts(
            {request.tenant_id, request.user_uuid}, request.query, options_.l3_top_k);
        if (!facts.ok()) {
            return facts.status();
        }
        auto formatted = FormatL3Facts(facts.value());
        for (const auto& fact : facts.value()) {
            context.items.push_back({ContextSource::L3, std::to_string(fact.entry_id), fact.payload,
                fact.extra_metadata_json.empty() ? "{\"source_role\":\"unknown\"}" : fact.extra_metadata_json});
        }
        if (!formatted.empty()) {
            context.l3_hit = true;
            sections.push_back(std::move(formatted));
        }
    }
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i != 0) {
            context.system_context += "\n";
        }
        context.system_context += sections[i];
    }
    return context;
}
core::Status SemanticMemoryContextProvider::BuildContextAsync(
    AsyncMemoryContextRequest request,
    BuildCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "memory context completion is required");
    }
    RecalledContext context;
    const auto recent_limit = request.max_recent_turns == 0
        ? options_.max_recent_turns
        : request.max_recent_turns;
    context.recent_turns = TakeRecent(request.current_session_recent, recent_limit);
    auto completion_state = std::make_shared<MemoryBuildCompletionState>(
        std::move(completion));
    const auto l3_memory = l3_memory_;
    const auto l3_top_k = options_.l3_top_k;
    const auto user_uuid = request.user_uuid;
    const auto query = request.query;
    std::function<void(RecalledContext)> complete_remaining =
        [l3_memory, l3_top_k, tenant_id = request.tenant_id, user_uuid, query, completion_state](
            RecalledContext value) mutable {
            std::vector<std::string> sections;
            if (!value.system_context.empty()) {
                sections.push_back(std::move(value.system_context));
            }
            if (l3_memory) {
                auto facts = l3_memory->SearchFacts(
                    {tenant_id, user_uuid}, query, l3_top_k);
                if (!facts.ok()) {
                    completion_state->TryComplete(facts.status());
                    return;
                }
                auto formatted = FormatL3Facts(facts.value());
                for (const auto& fact : facts.value()) {
                    value.items.push_back({ContextSource::L3, std::to_string(fact.entry_id), fact.payload,
                        fact.extra_metadata_json.empty() ? "{\"source_role\":\"unknown\"}" : fact.extra_metadata_json});
                }
                if (!formatted.empty()) {
                    value.l3_hit = true;
                    sections.push_back(std::move(formatted));
                }
            }
            value.system_context.clear();
            for (std::size_t index = 0; index < sections.size(); ++index) {
                if (index != 0) {
                    value.system_context += "\n";
                }
                value.system_context += sections[index];
            }
            completion_state->TryComplete(std::move(value));
        };
    if (options_.blocking_pool) {
        auto work = std::move(complete_remaining);
        complete_remaining = [work = std::move(work), pool = options_.blocking_pool,
                              completion_state](RecalledContext value) mutable {
            auto holder = std::make_shared<RecalledContext>(std::move(value));
            auto status = pool->Submit([work, holder, completion_state]() mutable {
                try { work(std::move(*holder)); }
                catch (const std::exception& error) {
                    completion_state->TryComplete(core::Status::Error(core::ErrorCode::InternalError, error.what()));
                } catch (...) {
                    completion_state->TryComplete(core::Status::Error(core::ErrorCode::InternalError, "L3 lookup threw"));
                }
            }, {}, "memory-l3-lookup");
            if (!status.ok()) completion_state->TryComplete(status);
        };
    }
    if (!l0_cache_) {
        complete_remaining(std::move(context));
        return core::Status::Ok();
    }
    auto async_l0 = std::dynamic_pointer_cast<semantic_cache::IAsyncSemanticCache>(l0_cache_);
    if (!async_l0) {
        MemoryContextRequest sync_request;
        sync_request.session_id = request.session_id;
        sync_request.tenant_id = request.tenant_id;
        sync_request.user_uuid = request.user_uuid;
        sync_request.persona_id = request.persona_id;
        sync_request.query = request.query;
        sync_request.trace_id = request.trace_id;
        sync_request.current_session_recent = request.current_session_recent;
        sync_request.max_recent_turns = request.max_recent_turns;
        completion_state->TryComplete(BuildContext(sync_request));
        return core::Status::Ok();
    }
    semantic_cache::CacheLookupRequest lookup;
    lookup.text = request.query;
    lookup.scope = semantic_cache::CacheScope::User;
    lookup.answer_type = semantic_cache::AnswerType::Personalized;
    lookup.tenant_id = request.tenant_id;
    lookup.user_id = request.user_uuid;
    lookup.session_id = request.session_id;
    lookup.persona_id = request.persona_id;
    lookup.extra["trace_id"] = request.trace_id;
    lookup.recent_turns.reserve(context.recent_turns.size());
    lookup.recent_turn_ids.reserve(context.recent_turns.size());
    for (const auto& turn : context.recent_turns) {
        lookup.recent_turns.push_back(FormatRecentTurn(turn));
        lookup.recent_turn_ids.push_back(turn.turn_id);
    }
    core::Status start_status;
    try {
        start_status = async_l0->LookupAsync(
            std::move(lookup),
            [context = std::move(context),
             complete_remaining = std::move(complete_remaining),
             completion_state](
                core::Result<semantic_cache::CacheLookupResult> l0) mutable {
                if (!l0.ok()) {
                    completion_state->TryComplete(l0.status());
                    return;
                }
                if (l0.value().hit && !l0.value().payload.empty()) {
                    context.l0_hit = true;
                    context.system_context =
                        "<memory_l0>\n" + l0.value().payload + "\n</memory_l0>";
                    if (l0.value().memory_records.empty()) context.items.push_back({ContextSource::L0, {}, context.system_context});
                    else for (const auto& record : l0.value().memory_records)
                        context.items.push_back({ContextSource::L0, record.record_id, record.content});
                }
                complete_remaining(std::move(context));
            });
    } catch (const std::exception& exception) {
        start_status = core::Status::Error(core::ErrorCode::InternalError, exception.what());
    } catch (...) {
        start_status = core::Status::Error(
            core::ErrorCode::Unknown,
            "async semantic cache threw an unknown exception");
    }
    if (!start_status.ok()) {
        // 若下游违反契约并在返回失败前同步回调，以已完成结果为准，避免上层二次 finish。
        if (!completion_state->CancelWithoutCompletion()) {
            return core::Status::Ok();
        }
    }
    return start_status;
}
core::Status SemanticMemoryContextProvider::AdmitTurnAsync(
    std::string session_id,
    std::string tenant_id,
    std::string user_uuid,
    ConversationTurn turn,
    std::string trace_id,
    AdmissionCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "memory admission completion is required");
    }
    if (!l0_cache_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "L0 memory cache is not initialized");
    }
    const memory::MemoryOwner owner{tenant_id, user_uuid};
    auto async_l0 = std::dynamic_pointer_cast<semantic_cache::IAsyncSemanticCache>(l0_cache_);
    if (!async_l0 && !options_.blocking_pool) {
        return core::Status::Error(
            core::ErrorCode::Unimplemented,
            "asynchronous L0 memory admission is not supported");
    }
    semantic_cache::CacheStoreRequest store;
    store.origin.text = turn.user_input;
    store.origin.scope = semantic_cache::CacheScope::User;
    store.origin.answer_type = semantic_cache::AnswerType::Personalized;
    store.origin.tenant_id = std::move(tenant_id);
    store.origin.user_id = std::move(user_uuid);
    store.origin.session_id = std::move(session_id);
    store.origin.persona_id = turn.persona_id;
    store.origin.extra["trace_id"] = std::move(trace_id);
    if (!turn.turn_id.empty()) store.origin.extra["turn_id"] = turn.turn_id;
    store.origin.extra["emotion"] = turn.emotion;
    store.origin.extra["intensity"] = std::to_string(turn.intensity);
    store.origin.extra["behavior"] = turn.behavior;
    store.origin.extra["tone"] = turn.tone;
    store.origin.extra["context_id"] = turn.context_id;
    store.origin.extra["payload_type"] = "conversation_turn";
    if (turn.valence) store.origin.extra["valence"] = std::to_string(*turn.valence);
    if (turn.arousal) store.origin.extra["arousal"] = std::to_string(*turn.arousal);
    store.response_payload = turn.response;
    store.answer_type = semantic_cache::AnswerType::Personalized;
    auto completion_state = std::make_shared<std::atomic<bool>>(false);
    auto guarded_completion = [completion = std::move(completion), completion_state,
                               l3_memory = l3_memory_, owner](
                                  core::Status status) mutable {
        bool expected = false;
        if (!completion_state->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (status.ok() && l3_memory) {
            status = l3_memory->RegisterOwner(owner);
        }
        try {
            completion(std::move(status));
        } catch (...) {
            // Provider 回调不应将业务异常传播回存储 executor。
        }
    };
    core::Status status;
    try {
        if (async_l0) status = async_l0->StoreAsync(std::move(store), std::move(guarded_completion));
        else {
            auto request = std::make_shared<semantic_cache::CacheStoreRequest>(std::move(store));
            status = options_.blocking_pool->Submit(
                [cache = l0_cache_, request, completion = guarded_completion]() mutable {
                    core::Status result;
                    try { result = cache->Store(*request); }
                    catch (const std::exception& error) { result = core::Status::Error(core::ErrorCode::InternalError, error.what()); }
                    catch (...) { result = core::Status::Error(core::ErrorCode::InternalError, "legacy L0 store threw"); }
                    completion(std::move(result));
                }, {}, "legacy-l0-admission");
        }
    } catch (const std::exception& exception) {
        status = core::Status::Error(core::ErrorCode::InternalError, exception.what());
    } catch (...) {
        status = core::Status::Error(
            core::ErrorCode::Unknown,
            "asynchronous semantic cache store threw an unknown exception");
    }
    if (!status.ok()) {
        bool expected = false;
        // 违反“失败不回调”契约时，以已交付完成为准，避免二次 admission 收口。
        if (!completion_state->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return core::Status::Ok();
        }
    }
    return status;
}

core::Status SemanticMemoryContextProvider::AdmitTurn(std::string_view session_id,
                                                      std::string_view tenant_id,
                                                      std::string_view user_uuid,
                                                      const ConversationTurn& turn,
                                                      std::string_view) {
    if (!l0_cache_) {
        return core::Status::Ok();
    }

    const memory::MemoryOwner owner{std::string(tenant_id), std::string(user_uuid)};
    semantic_cache::CacheStoreRequest store;
    store.origin.text = turn.user_input;
    store.origin.scope = semantic_cache::CacheScope::User;
    store.origin.answer_type = semantic_cache::AnswerType::Personalized;
    store.origin.tenant_id = std::string(tenant_id);
    store.origin.user_id = std::string(user_uuid);
    store.origin.session_id = std::string(session_id);
    store.origin.persona_id = turn.persona_id;
    if (!turn.turn_id.empty()) store.origin.extra["turn_id"] = turn.turn_id;
    store.origin.extra["emotion"] = turn.emotion;
    store.origin.extra["intensity"] = std::to_string(turn.intensity);
    store.origin.extra["behavior"] = turn.behavior;
    store.origin.extra["tone"] = turn.tone;
    store.origin.extra["context_id"] = turn.context_id;
    if (turn.valence) {
        store.origin.extra["valence"] = std::to_string(*turn.valence);
    }
    if (turn.arousal) {
        store.origin.extra["arousal"] = std::to_string(*turn.arousal);
    }
    store.origin.extra["payload_type"] = "conversation_turn";
    store.response_payload = turn.response;
    store.answer_type = semantic_cache::AnswerType::Personalized;
    auto status = l0_cache_->Store(store);
    if (status.ok() && l3_memory_) {
        status = l3_memory_->RegisterOwner(owner);
    }
    return status;
}

}
