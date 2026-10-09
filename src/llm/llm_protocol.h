#pragma once

#include "llm_client.h"
#include "logger_adapter.h"

#include <memory>
#include <string>
#include <string_view>

namespace agent::llm {

// 只描述线协议可表达的能力；Provider/模型的实际支持仍需部署配置与集成测试确认。
struct LlmProtocolCapabilities {
    bool function_tools = false;
    bool image_inputs = false;
    bool reasoning_content = false;
    bool streaming = false;
};

struct LlmProtocolContext {
    std::string default_model;
    CompletionResponseValidationOptions response_validation;
    LlmStreamLimits stream_limits;
    LlmEventSink event_sink;
};

// 输入视图仅在调用期间有效；decoder 自有 SSE framing、JSON 聚合和终态状态。
class ILlmStreamDecoder {
public:
    virtual ~ILlmStreamDecoder() = default;
    virtual core::Status Feed(std::string_view bytes) = 0;
    virtual core::Result<ChatCompletionResponse> Finish() = 0;
};

// 不依赖 HTTP DTO、认证、连接或重试；共享协议对象不可保存某次请求的可变状态。
class ILlmProtocol {
public:
    virtual ~ILlmProtocol() = default;
    /// 相对 base URL 的路径，不包含前导斜线；所有权由协议实现持有。
    virtual std::string_view Endpoint() const noexcept = 0;
    virtual LlmProtocolCapabilities Capabilities() const noexcept = 0;
    virtual core::Status ValidateRequest(const ChatCompletionRequest& request) const = 0;
    /// 由客户端先调用 ValidateRequest；编码仍返回拥有独立存储的完整 body。
    virtual core::Result<std::string> EncodeRequest(
        const ChatCompletionRequest& request, const LlmProtocolContext& context) const = 0;
    /// 只解析成功响应的 body；HTTP 状态映射由传输客户端完成。
    virtual core::Result<ChatCompletionResponse> DecodeResponse(
        std::string_view body, const ChatCompletionRequest& request,
        const LlmProtocolContext& context, core::LoggerAdapter& logger) const = 0;
    /// 每次请求独立创建 decoder；默认明确拒绝尚未实现的 streaming 能力。
    virtual core::Result<std::unique_ptr<ILlmStreamDecoder>> CreateStreamDecoder(
        const ChatCompletionRequest&, const LlmProtocolContext&) const {
        return core::Status::Error(core::ErrorCode::Unimplemented,
                                   "streaming decoder is not implemented by this protocol");
    }
};

// 当前 OpenAI-compatible Chat Completions 线格式；方法均无请求级共享状态。
class ChatCompletionsProtocol final : public ILlmProtocol {
public:
    std::string_view Endpoint() const noexcept override { return "chat/completions"; }
    LlmProtocolCapabilities Capabilities() const noexcept override {
        return {.function_tools = true, .image_inputs = true, .reasoning_content = true, .streaming = true};
    }
    core::Status ValidateRequest(const ChatCompletionRequest& request) const override;
    core::Result<std::string> EncodeRequest(
        const ChatCompletionRequest& request, const LlmProtocolContext& context) const override;
    core::Result<ChatCompletionResponse> DecodeResponse(
        std::string_view body, const ChatCompletionRequest& request,
        const LlmProtocolContext& context, core::LoggerAdapter& logger) const override;
    core::Result<std::unique_ptr<ILlmStreamDecoder>> CreateStreamDecoder(
        const ChatCompletionRequest& request, const LlmProtocolContext& context) const override;
};

}
