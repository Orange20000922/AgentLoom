#include "llm_protocol.h"
#include "text_validation.h"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <exception>
#include <limits>
#include <unordered_set>

namespace agent::llm {
using Json = nlohmann::json;
namespace {

// 以下编解码与原客户端线格式一致，不持有 HTTP 请求或会话状态。
std::string RoleToString(ChatRole role) {
    switch (role) {
        case ChatRole::System: return "system";
        case ChatRole::User: return "user";
        case ChatRole::Assistant: return "assistant";
        case ChatRole::Tool: return "tool";
    }
    return "user";
}

Json ContentPartToJson(const ChatContentPart& part) {
    switch (part.type) {
        case ChatContentPartType::Text:
            return Json{{"type", "text"}, {"text", part.text}};
        case ChatContentPartType::ImageUrl:
            return Json{{"type", "image_url"}, {"image_url", {{"url", part.image_url}}}};
    }
    return Json{{"type", "text"}, {"text", part.text}};
}

Json BuildRequestJson(const ChatCompletionRequest& req, const std::string& default_model) {
    Json j;
    j["model"] = req.model.empty() ? default_model : req.model;
    j["messages"] = Json::array();
    for (const auto& msg : req.messages) {
        Json msg_obj;
        msg_obj["role"] = RoleToString(msg.role);
        if (msg.parts.empty()) {
            msg_obj["content"] = msg.content;
        } else {
            msg_obj["content"] = Json::array();
            for (const auto& part : msg.parts) {
                msg_obj["content"].push_back(ContentPartToJson(part));
            }
        }
        if (!msg.tool_calls.empty()) {
            if (msg.content.empty()) msg_obj["content"] = nullptr;
            msg_obj["tool_calls"] = Json::array();
            for (const auto& call : msg.tool_calls) {
                msg_obj["tool_calls"].push_back({{"id", call.id}, {"type", "function"},
                    {"function", {{"name", call.name}, {"arguments", call.arguments_json}}}});
            }
        }
        if (msg.role == ChatRole::Tool) msg_obj["tool_call_id"] = msg.tool_call_id;
        if (msg.reasoning_content) msg_obj["reasoning_content"] = *msg.reasoning_content;
        j["messages"].push_back(msg_obj);
    }
    j["temperature"] = req.temperature;
    if (req.max_tokens > 0) {
        j["max_tokens"] = req.max_tokens;
    }
    j["top_p"] = req.top_p;
    j["n"] = req.n;
    j["stream"] = req.stream;
    if (!req.tools.empty()) {
        j["tools"] = Json::array();
        for (const auto& tool : req.tools) {
            const auto parameters = Json::parse(tool.parameters_json);
            j["tools"].push_back({{"type", "function"}, {"function", {
                {"name", tool.name}, {"description", tool.description}, {"parameters", parameters}
            }}});
        }
    }
    if (!req.tool_choice.empty()) j["tool_choice"] = req.tool_choice;
    if (req.parallel_tool_calls) j["parallel_tool_calls"] = *req.parallel_tool_calls;
    return j;
}

core::Result<ChatCompletionResponse> ParseResponse(
    std::string_view body,
    const ChatCompletionRequest& request,
    const LlmProtocolContext& options,
    core::LoggerAdapter& logger) {
    if (body.empty()) {
        return core::Status::Error(core::ErrorCode::DataLoss, "LLM response body is empty");
    }
    Json j;
    try {
        j = Json::parse(body);
    } catch (const std::exception& e) {
        return core::Status(core::ErrorCode::DataLoss,
            std::string("Failed to parse LLM response: ") + e.what());
    }

    ChatCompletionResponse resp;
    if (j.contains("id") && j["id"].is_string()) {
        resp.id = j["id"].get<std::string>();
    }
    if (j.contains("model") && j["model"].is_string()) {
        resp.model = j["model"].get<std::string>();
    }

    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
        return core::Status(core::ErrorCode::DataLoss,
            "LLM response missing choices array");
    }

    const auto& choice = j["choices"][0];
    if (!choice.contains("message") || !choice["message"].is_object()) {
        return core::Status(core::ErrorCode::DataLoss,
            "LLM response missing message object");
    }

    const auto& message = choice["message"];
    if (message.contains("content") && !message["content"].is_null() && !message["content"].is_string()) {
        return core::Status::Error(core::ErrorCode::DataLoss, "LLM content must be text or null");
    }
    const bool has_content = message.contains("content") && message["content"].is_string();
    if (has_content) {
        resp.content = message["content"].get<std::string>();
    }
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
        if (!choice["finish_reason"].is_string()) {
            return core::Status::Error(core::ErrorCode::DataLoss,
                                       "LLM finish_reason must be text or null");
        }
        resp.finish_reason = choice["finish_reason"].get<std::string>();
    }
    if (message.contains("reasoning_content") && !message["reasoning_content"].is_null()) {
        if (!message["reasoning_content"].is_string()) {
            return core::Status::Error(core::ErrorCode::DataLoss,
                                       "LLM reasoning_content must be text or null");
        }
        resp.reasoning_content = message["reasoning_content"].get<std::string>();
    }
    if (message.contains("tool_calls") && !message["tool_calls"].is_null()) {
        if (!message["tool_calls"].is_array()) {
            return core::Status::Error(core::ErrorCode::DataLoss, "LLM tool_calls must be an array");
        }
        std::unordered_set<std::string> ids;
        for (const auto& call : message["tool_calls"]) {
            // 整批拒绝损坏调用，避免部分执行产生不可解释的副作用。
            if (!call.is_object() || !call.contains("id") || !call["id"].is_string() ||
                !call.contains("type") || call["type"] != "function" ||
                !call.contains("function") || !call["function"].is_object()) {
                return core::Status::Error(core::ErrorCode::DataLoss, "malformed LLM tool call");
            }
            const auto& fn = call["function"];
            if (!fn.contains("name") || !fn["name"].is_string() ||
                !fn.contains("arguments") ||
                (!fn["arguments"].is_string() && !fn["arguments"].is_object())) {
                return core::Status::Error(core::ErrorCode::DataLoss, "malformed LLM function fields");
            }
            const auto arguments = fn["arguments"].is_string()
                ? fn["arguments"].get<std::string>()
                : fn["arguments"].dump();
            ChatToolCall parsed{call["id"].get<std::string>(), fn["name"].get<std::string>(), arguments};
            if (parsed.id.empty() || parsed.name.empty() || !ids.insert(parsed.id).second) {
                return core::Status::Error(core::ErrorCode::DataLoss, "invalid or duplicate LLM tool call id");
            }
            resp.tool_calls.push_back(std::move(parsed));
        }
    }
    if (j.contains("usage") && !j["usage"].is_null() && !j["usage"].is_object()) {
        return core::Status::Error(core::ErrorCode::DataLoss,
                                   "LLM usage must be an object or null");
    }
    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& usage = j["usage"];
        const auto read_tokens = [&usage](std::string_view name, int& target) -> core::Status {
            const auto found = usage.find(std::string(name));
            if (found == usage.end()) {
                return core::Status::Ok();
            }
            if (!found->is_number_integer() && !found->is_number_unsigned()) {
                return core::Status::Error(
                    core::ErrorCode::DataLoss,
                    "LLM usage." + std::string(name) + " must be a non-negative integer");
            }
            try {
                const auto value = found->get<std::int64_t>();
                if (value < 0 || value > std::numeric_limits<int>::max()) {
                    return core::Status::Error(
                        core::ErrorCode::DataLoss,
                        "LLM usage." + std::string(name) + " is out of range");
                }
                target = static_cast<int>(value);
                return core::Status::Ok();
            } catch (const std::exception&) {
                return core::Status::Error(
                    core::ErrorCode::DataLoss,
                    "LLM usage." + std::string(name) + " is out of range");
            }
        };
        if (auto status = read_tokens("prompt_tokens", resp.prompt_tokens); !status.ok()) {
            return status;
        }
        if (auto status = read_tokens("completion_tokens", resp.completion_tokens); !status.ok()) {
            return status;
        }
        if (auto status = read_tokens("total_tokens", resp.total_tokens); !status.ok()) {
            return status;
        }
    }

    if (resp.prompt_tokens < 0 || resp.completion_tokens < 0 || resp.total_tokens < 0 ||
        (resp.total_tokens > 0 &&
         resp.total_tokens < resp.prompt_tokens + resp.completion_tokens)) {
        return core::Status::Error(core::ErrorCode::DataLoss,
                                   "LLM response contains inconsistent token usage");
    }
    if (options.response_validation.reject_length_finish && resp.finish_reason == "length") {
        return core::Status::Error(
            core::ErrorCode::ResourceExhausted,
            "LLM generation was truncated because the token budget was exhausted");
    }
    if (has_content) {
        if (!core::IsValidUtf8(resp.content) || core::HasInvalidTextControl(resp.content)) {
            return core::Status::Error(core::ErrorCode::DataLoss,
                                       "LLM content is not valid UTF-8 text");
        }
    }
    if (resp.reasoning_content &&
        (!core::IsValidUtf8(*resp.reasoning_content) ||
         core::HasInvalidTextControl(*resp.reasoning_content))) {
        return core::Status::Error(core::ErrorCode::DataLoss,
                                   "LLM reasoning_content is not valid UTF-8 text");
    }
    if (resp.tool_calls.empty() &&
        (!has_content ||
         (options.response_validation.reject_empty_content && core::IsBlankAscii(resp.content)))) {
        return core::Status::Error(core::ErrorCode::Unavailable,
                                   "LLM response contains no usable content or tool calls");
    }

    const auto& validation = options.response_validation;
    if (validation.token_count_mode != CompletionTokenValidationMode::Off &&
        validation.token_counter && resp.completion_tokens > 0) {
        const auto model = resp.model.empty()
            ? (request.model.empty() ? options.default_model : request.model)
            : resp.model;
        auto counted = validation.token_counter->CountTokens(model, resp);
        if (!counted.ok()) {
            if (validation.token_count_mode == CompletionTokenValidationMode::Strict) {
                return counted.status();
            }
            logger.warn("LLM token count audit skipped model={} reason={}",
                        model, counted.status().message());
        } else {
            const auto reported = static_cast<std::size_t>(resp.completion_tokens);
            const auto actual = counted.value();
            const auto difference = actual > reported ? actual - reported : reported - actual;
            if (difference > validation.max_token_difference) {
                if (validation.token_count_mode == CompletionTokenValidationMode::Strict) {
                    return core::Status::Error(
                        core::ErrorCode::DataLoss,
                        "LLM completion token count differs from provider usage");
                }
                logger.warn(
                    "LLM token count audit mismatch model={} reported={} actual={} difference={}",
                    model, reported, actual, difference);
            }
        }
    }

    return resp;
}


}

core::Status ChatCompletionsProtocol::ValidateRequest(const ChatCompletionRequest& request) const {
    const auto invalid = [](const char* reason) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, reason);
    };
    if (request.stream) return invalid("streaming completions are not implemented by this client");
    if (request.tool_choice != "" && request.tool_choice != "none" &&
        request.tool_choice != "auto" && request.tool_choice != "required") {
        return invalid("unsupported tool_choice");
    }
    if ((request.tool_choice == "auto" || request.tool_choice == "required" ||
         request.parallel_tool_calls.has_value()) && request.tools.empty()) {
        return invalid("tool selection requires tool definitions");
    }
    std::unordered_set<std::string> names;
    for (const auto& tool : request.tools) {
        const auto schema = Json::parse(tool.parameters_json, nullptr, false);
        if (tool.name.empty() || !names.insert(tool.name).second || !schema.is_object() ||
            !schema.contains("type") || schema["type"] != "object") {
            return invalid("tools require unique names and an object parameter schema");
        }
    }
    // 一次 assistant 调用批次必须收齐结果，才能继续用户/助手消息。
    std::unordered_set<std::string> pending;
    std::unordered_set<std::string> used;
    for (const auto& message : request.messages) {
        if (message.role == ChatRole::Tool) {
            if (message.tool_call_id.empty() || pending.erase(message.tool_call_id) != 1 ||
                !message.tool_calls.empty() || !message.parts.empty() || message.reasoning_content) {
                return invalid("tool result does not match a pending assistant call");
            }
            continue;
        }
        if (!pending.empty()) return invalid("assistant tool calls are missing results");
        if (!message.tool_call_id.empty() ||
            (message.role != ChatRole::Assistant && (!message.tool_calls.empty() || message.reasoning_content))) {
            return invalid("tool metadata belongs to assistant or tool messages only");
        }
        for (const auto& call : message.tool_calls) {
            if (call.id.empty() || call.name.empty() || !used.insert(call.id).second) {
                return invalid("assistant tool call id is empty or duplicated");
            }
            pending.insert(call.id);
        }
    }
    if (!pending.empty()) return invalid("assistant tool calls are missing results");
    return core::Status::Ok();
}


// 兼容旧的显式校验入口，避免下游测试和自定义客户端必须同步迁移。
core::Status ValidateChatCompletionRequest(const ChatCompletionRequest& request) {
    return ChatCompletionsProtocol{}.ValidateRequest(request);
}

core::Result<std::string> ChatCompletionsProtocol::EncodeRequest(
    const ChatCompletionRequest& request, const LlmProtocolContext& context) const {
    try {
        // JSON serializer 参数与旧实现一致，包括无效 UTF-8 的替换策略。
        return BuildRequestJson(request, context.default_model)
            .dump(-1, ' ', false, Json::error_handler_t::replace);
    } catch (const std::exception& error) {
        return core::Status::Error(core::ErrorCode::InternalError,
                                  std::string("LLM client exception: ") + error.what());
    } catch (...) {
        return core::Status::Error(core::ErrorCode::InternalError,
                                  "LLM client exception: unknown error");
    }
}

core::Result<ChatCompletionResponse> ChatCompletionsProtocol::DecodeResponse(
    std::string_view body, const ChatCompletionRequest& request,
    const LlmProtocolContext& context, core::LoggerAdapter& logger) const {
    try {
        return ParseResponse(body, request, context, logger);
    } catch (const std::exception& error) {
        return core::Status::Error(core::ErrorCode::InternalError,
                                  std::string("LLM client exception: ") + error.what());
    } catch (...) {
        return core::Status::Error(core::ErrorCode::InternalError,
                                  "LLM client exception: unknown error");
    }
}

}
