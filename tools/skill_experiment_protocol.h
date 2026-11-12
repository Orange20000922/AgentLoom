#pragma once

#include "../src/llm/llm_protocol.h"
#include <nlohmann/json.hpp>

namespace agent::tools {

// 实验专用协议装饰器：只增加 Provider 思考开关，其余编解码及校验复用生产协议。
class SkillExperimentProtocol final : public llm::ILlmProtocol {
public:
    explicit SkillExperimentProtocol(bool thinking) : thinking_(thinking) {}
    std::string_view Endpoint() const noexcept override { return base_.Endpoint(); }
    llm::LlmProtocolCapabilities Capabilities() const noexcept override { return base_.Capabilities(); }
    core::Status ValidateRequest(const llm::ChatCompletionRequest& request) const override {
        return base_.ValidateRequest(request);
    }
    core::Result<std::string> EncodeRequest(const llm::ChatCompletionRequest& request,
                                           const llm::LlmProtocolContext& context) const override {
        auto result = base_.EncodeRequest(request, context);
        if (!result.ok()) return result.status();
        try {
            auto body = nlohmann::json::parse(result.value());
            body["thinking"] = {{"type", thinking_ ? "enabled" : "disabled"}};
            // 两组使用相同 effort 参数，仅开关不同；disabled 时该参数不影响生成。
            body["reasoning_effort"] = "high";
            return body.dump();
        } catch (const std::exception& error) {
            auto status = core::Status::Error(core::ErrorCode::InternalError, error.what());
            core::LoggerAdapter::ForModule("skill-experiment").error("encode failed: {}", status.message());
            return status;
        }
    }
    core::Result<llm::ChatCompletionResponse> DecodeResponse(std::string_view body,
        const llm::ChatCompletionRequest& request, const llm::LlmProtocolContext& context,
        core::LoggerAdapter& logger) const override {
        return base_.DecodeResponse(body, request, context, logger);
    }
    core::Result<std::unique_ptr<llm::ILlmStreamDecoder>> CreateStreamDecoder(
        const llm::ChatCompletionRequest& request, const llm::LlmProtocolContext& context) const override {
        return base_.CreateStreamDecoder(request, context);
    }
private:
    bool thinking_;
    llm::ChatCompletionsProtocol base_;
};
}
