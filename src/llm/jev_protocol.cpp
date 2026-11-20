#include "llm_protocol.h"
#include <cmath>
#include <nlohmann/json.hpp>

namespace agent::llm {
namespace {
using Json = nlohmann::json;

bool IsProbability(const Json& value) {
    return value.is_number() && value >= 0 && value <= 1;
}
}

core::Status JevProtocol::ValidateRequest(const ChatCompletionRequest& request) const {
    if (!request.jev || request.jev->questions.empty() || !request.messages.empty() ||
        !request.tools.empty() || request.stream)
        return core::Status(core::ErrorCode::InvalidArgument, "Jev 需要结构化问题，仅支持非流式 choice");
    for (const auto& [name, question] : request.jev->questions) {
        if (name.empty() || question.criteria.empty() || question.criteria.size() > 255 ||
            question.criteria.contains(""))
            return core::Status(core::ErrorCode::InvalidArgument, "Jev 题名或选项无效");
    }
    return core::Status::Ok();
}

core::Result<std::string> JevProtocol::EncodeRequest(
    const ChatCompletionRequest& request, const LlmProtocolContext& context) const {
    if (!request.jev) return core::Status(core::ErrorCode::InvalidArgument, "缺少 Jev 请求");
    auto state = Json::parse(request.jev->state_json, nullptr, false);
    if (!state.is_string() && !state.is_array() && !state.is_object())
        return core::Status(core::ErrorCode::InvalidArgument, "Jev state 需要 JSON 字符串、数组或对象");
    auto questions = Json::object();
    for (const auto& [name, question] : request.jev->questions) {
        questions[name] = {{"type", "choice"}, {"criteria", question.criteria}};
        if (!question.instructions.empty()) questions[name]["instructions"] = question.instructions;
    }
    try {
        return Json{{"model", request.model.empty() ? context.default_model : request.model},
                    {"state", std::move(state)}, {"questions", std::move(questions)}}.dump();
    } catch (const Json::exception&) {
        return core::Status(core::ErrorCode::InvalidArgument, "Jev 请求编码失败");
    }
}

core::Result<ChatCompletionResponse> JevProtocol::DecodeResponse(
    std::string_view body, const ChatCompletionRequest& request, const LlmProtocolContext&, core::LoggerAdapter&) const {
    const auto data = Json::parse(body, nullptr, false);
    if (!request.jev || !data.is_object() || !data.contains("model") || !data["model"].is_string() ||
        !data.contains("answers") || !data["answers"].is_object() ||
        data["answers"].size() != request.jev->questions.size())
        return core::Status(core::ErrorCode::DataLoss, "Jev 响应缺少模型或题目集合不完整");
    ChatCompletionResponse response;
    response.model = data["model"].get<std::string>();
    for (const auto& [name, question] : request.jev->questions) {
        const auto invalid = core::Status(core::ErrorCode::DataLoss, "Jev 题目结果无效: " + name);
        const auto item = data["answers"].find(name);
        if (item == data["answers"].end() || !item->is_object() || !item->contains("type") ||
            (*item)["type"] != "choice" || !item->contains("choice") || !(*item)["choice"].is_string() ||
            !item->contains("confidence") || !IsProbability((*item)["confidence"]) ||
            !item->contains("probabilities")) return invalid;
        JevChoiceAnswer answer{(*item)["choice"].get<std::string>(), (*item)["confidence"].get<double>(), {}};
        const auto& probabilities = (*item)["probabilities"];
        if (!question.criteria.contains(answer.choice) || !probabilities.is_object() ||
            probabilities.size() != question.criteria.size()) return invalid;
        double total = 0;
        for (const auto& [label, probability] : probabilities.items()) {
            if (!question.criteria.contains(label) || !IsProbability(probability)) return invalid;
            total += probability.get<double>();
            answer.probabilities.emplace(label, probability.get<double>());
        }
        // 协议允许返回值经过舍入；偏差明显的分布不适合提供给决策方。
        if (std::abs(total - 1.0) > 0.01) return invalid;
        response.jev_answers.emplace(name, std::move(answer));
    }
    return response;
}
}
