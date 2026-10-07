#pragma once

#include "persona_runtime.h"

namespace agent::test {

// 只提供确定的答案缓存命中，用于验证 Persona 的 LLM 跳过与用量语义。
class FixedAnswerCacheProvider final : public service::persona::IAnswerCacheProvider {
public:
    core::Result<service::persona::AnswerCacheLookupResult> Lookup(
        const service::persona::AnswerCacheLookupRequest&) override {
        service::persona::AnswerCacheLookupResult result;
        result.hit = true;
        result.response = "cached answer";
        return result;
    }

    core::Status Store(const service::persona::AnswerCacheStoreRequest&) override {
        return core::Status::Error(core::ErrorCode::InternalError,
                                   "cache hit must not store a new answer");
    }
};

}
