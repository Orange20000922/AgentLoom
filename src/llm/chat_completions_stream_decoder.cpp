#include "llm_protocol.h"
#include "../net/sse.h"
#include "text_validation.h"

#include <nlohmann/json.hpp>
#include <climits>
#include <limits>
#include <map>

namespace agent::llm {
namespace {
using Json = nlohmann::json;

core::Status Corrupt(std::string message) {
    return core::Status::Error(core::ErrorCode::DataLoss, std::move(message));
}

class ChatCompletionsStreamDecoder final : public ILlmStreamDecoder {
public:
    ChatCompletionsStreamDecoder(ChatCompletionRequest request, LlmProtocolContext context)
        : request_(std::move(request)), context_(std::move(context)),
          framing_([this](const net::SseEvent& event) { return OnEvent(event); },
                   context_.stream_limits.max_event_bytes) {}

    core::Status Feed(std::string_view bytes) override {
        if (!status_.ok()) return status_;
        // 总输入量亦有界：无限注释、usage 快照和零增量不能绕过生成预算。
        if (bytes.size() > context_.stream_limits.max_generation_bytes - received_bytes_)
            return status_ = core::Status::Error(core::ErrorCode::ResourceExhausted,
                                                  "LLM stream exceeds generation byte limit");
        received_bytes_ += bytes.size();
        status_ = framing_.Feed(bytes);
        return status_;
    }

    core::Result<ChatCompletionResponse> Finish() override {
        if (!status_.ok()) return status_;
        if (auto status = framing_.Finish(); !status.ok()) return status;
        if (!done_ || !response_) return Corrupt("LLM stream ended without [DONE] and finish_reason");
        return *response_;
    }

private:
    core::Status Publish(LlmStreamEvent event) {
        event.sequence = ++sequence_;
        event.generation_id = id_;
        return context_.event_sink ? context_.event_sink(event) : core::Status::Ok();
    }

    core::Status Append(const Json& object, const char* field, std::string& target) {
        const auto found = object.find(field);
        if (found == object.end() || found->is_null()) return core::Status::Ok();
        if (!found->is_string()) return Corrupt(std::string("LLM delta.") + field + " must be text");
        const auto& value = found->get_ref<const std::string&>();
        if (!core::IsValidUtf8(value) || core::HasInvalidTextControl(value))
            return Corrupt("LLM delta contains invalid text");
        target += value;
        return core::Status::Ok();
    }

    core::Status Metadata(const Json& chunk, const char* name, std::string& value) {
        const auto found = chunk.find(name);
        if (found == chunk.end()) return core::Status::Ok();
        if (!found->is_string()) return Corrupt("LLM chunk metadata must be text");
        const auto next = found->get<std::string>();
        if (!value.empty() && next != value) return Corrupt("LLM chunk changed generation metadata");
        value = next;
        return core::Status::Ok();
    }

    core::Status Usage(const Json& value) {
        if (!value.is_object()) return Corrupt("LLM chunk usage must be an object");
        // usage 是覆盖快照；从不把重复上报逐次相加。
        for (const auto* name : {"prompt_tokens", "completion_tokens", "total_tokens"}) {
            const auto found = value.find(name);
            if (found == value.end()) continue;
            if ((!found->is_number_integer() && !found->is_number_unsigned()) ||
                (found->is_number_unsigned() && found->get<std::uint64_t>() > INT_MAX))
                return Corrupt("LLM chunk usage is out of range");
            const auto count = found->get<std::int64_t>();
            if (count < 0 || count > INT_MAX) return Corrupt("LLM chunk usage is out of range");
            usage_[name] = count;
        }
        const auto prompt = usage_.value("prompt_tokens", 0);
        const auto completion = usage_.value("completion_tokens", 0);
        const auto total = usage_.value("total_tokens", 0);
        if (total > 0 && static_cast<std::int64_t>(total) < static_cast<std::int64_t>(prompt) + completion)
            return Corrupt("LLM chunk usage is inconsistent");
        return Publish({.kind = LlmStreamEventKind::UsageUpdated,
                        .prompt_tokens = prompt, .completion_tokens = completion, .total_tokens = total});
    }

    core::Status Tools(const Json& deltas) {
        if (!deltas.is_array()) return Corrupt("LLM tool delta must be an array");
        for (const auto& delta : deltas) {
            if (!delta.is_object() || !delta.contains("index") || !delta["index"].is_number_integer())
                return Corrupt("LLM tool delta is missing index");
            const auto index = delta["index"].get<std::int64_t>();
            if (index < 0 || static_cast<std::uint64_t>(index) >= context_.stream_limits.max_tool_calls)
                return core::Status::Error(core::ErrorCode::ResourceExhausted, "LLM tool index exceeds limit");
            auto& call = calls_[static_cast<std::size_t>(index)];
            if (delta.contains("type") && delta["type"] != "function") return Corrupt("invalid tool delta type");
            if (auto status = Append(delta, "id", call.id); !status.ok()) return status;
            ChatToolCall fragment;
            fragment.id = call.id;
            if (delta.contains("function")) {
                const auto& fn = delta["function"];
                if (!fn.is_object()) return Corrupt("invalid tool delta function");
                if (auto status = Append(fn, "name", fragment.name); !status.ok()) return status;
                if (auto status = Append(fn, "arguments", fragment.arguments_json); !status.ok()) return status;
                call.name += fragment.name;
                if (fragment.arguments_json.size() >
                    context_.stream_limits.max_tool_arguments_bytes - call.arguments_json.size())
                    return core::Status::Error(core::ErrorCode::ResourceExhausted, "LLM tool arguments exceed limit");
                call.arguments_json += fragment.arguments_json;
            }
            if (auto status = Publish({.kind = LlmStreamEventKind::ToolCallProgress,
                                       .item_id = "tool:" + std::to_string(index),
                                       .tool_call = std::move(fragment)}); !status.ok()) return status;
        }
        return core::Status::Ok();
    }

    core::Status Complete() {
        if (finish_reason_.empty() || !saw_choice_) return Corrupt("[DONE] arrived without finish_reason");
        Json message{{"content", content_}};
        if (!reasoning_.empty()) message["reasoning_content"] = reasoning_;
        if (!calls_.empty()) {
            message["tool_calls"] = Json::array();
            std::size_t expected = 0;
            for (const auto& [index, call] : calls_) {
                if (index != expected++) return Corrupt("LLM tool indices are not contiguous");
                if (!Json::parse(call.arguments_json, nullptr, false).is_object())
                    return Corrupt("LLM completed tool arguments are not a JSON object");
                message["tool_calls"].push_back({{"id", call.id}, {"type", "function"},
                    {"function", {{"name", call.name}, {"arguments", call.arguments_json}}}});
            }
            if (finish_reason_ != "tool_calls") return Corrupt("tool calls missing tool_calls finish_reason");
        } else if (finish_reason_ == "tool_calls") {
            return Corrupt("tool_calls finish_reason without tool calls");
        }
        Json aggregate{{"id", id_}, {"model", model_}, {"usage", usage_},
            {"choices", Json::array({{{"message", message}, {"finish_reason", finish_reason_}}})}};
        auto logger = core::LoggerAdapter::ForModule("llm-stream");
        // 普通与流式复用同一个最终校验入口（内容、截断、tool id、usage、token audit）。
        auto result = ChatCompletionsProtocol{}.DecodeResponse(aggregate.dump(), request_, context_, logger);
        if (!result.ok()) return result.status();
        response_ = std::move(result).value();
        done_ = true;
        if (auto status = Publish({.kind = LlmStreamEventKind::OutputItemCompleted}); !status.ok()) return status;
        return Publish({.kind = LlmStreamEventKind::GenerationCompleted});
    }

    core::Status OnEvent(const net::SseEvent& event) {
        if (done_) return Corrupt("LLM data arrived after [DONE]");
        if (event.data == "[DONE]") return Complete();
        const auto chunk = Json::parse(event.data, nullptr, false);
        if (!chunk.is_object() || chunk.contains("error")) return Corrupt("malformed LLM stream chunk");
        if (auto status = Metadata(chunk, "id", id_); !status.ok()) return status;
        if (auto status = Metadata(chunk, "model", model_); !status.ok()) return status;
        if (!chunk.contains("choices") || !chunk["choices"].is_array()) return Corrupt("LLM chunk missing choices");
        const auto& choices = chunk["choices"];
        if (choices.size() > 1) return Corrupt("LLM stream has multiple choices");
        if (!choices.empty()) {
            const auto& choice = choices[0];
            if (finished_choice_ || !choice.is_object() || !choice.contains("index") ||
                !choice["index"].is_number_integer() || choice["index"] != 0 ||
                !choice.contains("delta") || !choice["delta"].is_object())
                return Corrupt("invalid or post-terminal LLM choice delta");
            saw_choice_ = true;
            const auto& delta = choice["delta"];
            if (delta.contains("role") && delta["role"] != "assistant") return Corrupt("invalid LLM delta role");
            std::string text;
            if (auto status = Append(delta, "content", text); !status.ok()) return status;
            content_ += text;
            if (!text.empty()) {
                if (auto status = Publish({.kind = LlmStreamEventKind::TextDelta, .text = std::move(text)});
                    !status.ok()) return status;
            }
            if (auto status = Append(delta, "reasoning_content", reasoning_); !status.ok()) return status;
            if (delta.contains("tool_calls") && !delta["tool_calls"].is_null()) {
                if (auto status = Tools(delta["tool_calls"]); !status.ok()) return status;
            }
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                if (!choice["finish_reason"].is_string()) return Corrupt("invalid LLM finish_reason");
                finish_reason_ = choice["finish_reason"].get<std::string>();
                if (finish_reason_.empty()) return Corrupt("empty LLM finish_reason");
                finished_choice_ = true;
            }
        }
        if (chunk.contains("usage") && !chunk["usage"].is_null()) return Usage(chunk["usage"]);
        if (choices.empty()) return Corrupt("empty LLM chunk without usage");
        return core::Status::Ok();
    }

    ChatCompletionRequest request_;
    LlmProtocolContext context_;
    net::SseDecoder framing_;
    core::Status status_;
    std::size_t received_bytes_ = 0;
    std::uint64_t sequence_ = 0;
    bool saw_choice_ = false;
    bool finished_choice_ = false;
    bool done_ = false;
    std::string id_, model_, content_, reasoning_, finish_reason_;
    std::map<std::size_t, ChatToolCall> calls_;
    Json usage_ = Json::object();
    std::optional<ChatCompletionResponse> response_;
};
}

core::Result<std::unique_ptr<ILlmStreamDecoder>> ChatCompletionsProtocol::CreateStreamDecoder(
    const ChatCompletionRequest& request, const LlmProtocolContext& context) const {
    if (auto status = ValidateRequest(request); !status.ok()) return status;
    const auto& limits = context.stream_limits;
    if (!limits.max_event_bytes || !limits.max_generation_bytes ||
        !limits.max_tool_arguments_bytes || !limits.max_tool_calls)
        return core::Status::Error(core::ErrorCode::InvalidArgument, "LLM streaming limits must be positive");
    return std::unique_ptr<ILlmStreamDecoder>(new ChatCompletionsStreamDecoder(request, context));
}

}
