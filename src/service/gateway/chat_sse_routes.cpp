#include "chat_sse_routes.h"
#include "persona_gateway_route_helpers.h"

#include <atomic>

namespace agent::service::gateway {
namespace {
using namespace route_detail;

const char* EventName(llm::LlmStreamEventKind kind) {
    switch (kind) {
    case llm::LlmStreamEventKind::TextDelta: return "TextDelta";
    case llm::LlmStreamEventKind::ToolCallProgress: return "ToolCallProgress";
    case llm::LlmStreamEventKind::ToolExecutionState: return "ToolExecutionState";
    case llm::LlmStreamEventKind::UsageUpdated: return "UsageUpdated";
    case llm::LlmStreamEventKind::OutputItemCompleted: return "OutputItemCompleted";
    case llm::LlmStreamEventKind::GenerationCompleted: return "GenerationCompleted";
    }
    return "Unknown";
}

void SendFailure(const HttpRouteContext& context, const core::Status& status) {
    SendJson(context.request, HttpStatusFor(status.code()), ErrorEnvelope(context.trace_id, status), context.trace_id);
}

core::Status Connect(const HttpRouteContext& context, const std::shared_ptr<IChatSseChannel>& channel,
                     std::string_view last_id = {}) {
    auto connection_id = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::weak_ptr<IChatSseChannel> weak = channel;
    auto options = context.service.StreamOptions();
    options.require_pong = context.body.value("requirePong", options.require_pong);
    auto opened = context.request->BeginEventStream(options, [weak, connection_id](core::Status) {
        if (auto turn = weak.lock()) turn->Disconnected(connection_id->load());
    });
    if (!opened.ok()) { SendFailure(context, opened.status()); return opened.status(); }
    auto stream = opened.value();
    connection_id->store(stream->stream_connection_id());
    auto status = channel->Connect(stream, last_id);
    if (!status.ok()) {
        stream->SendEvent({"TurnFailed", ErrorEnvelope(context.trace_id, status).dump(), {}});
        stream->FinishEvents();
    }
    return status;
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(ChatStreamResumeRoute, ::net::http::verb::get, "api", "chat", "stream", "{requestId}") {
    auto found = context.service.ChatStreams().Find(context.path_params.at("requestId"), context.identity.user_uuid);
    if (!found.ok()) { SendFailure(context, found.status()); return; }
    const auto header = context.message.find("Last-Event-ID");
    const auto last_id = header == context.message.end() ? std::string{} : std::string(header->value());
    if (auto status = found.value()->CheckResume(last_id); !status.ok()) { SendFailure(context, status); return; }
    auto status = Connect(context, found.value(), last_id);
    if (!status.ok()) {
        core::LoggerAdapter::ForModule("gateway-sse").warn("SSE resume failed code={}", static_cast<int>(status.code()));
        // Connect 已发送失败终态；重连不接纳新的 Turn，原始生成的取消语义保持不变。
    }
}

DECLARE_AUTHENTICATED_HTTP_ROUTE(ChatStreamPongRoute, ::net::http::verb::post, "api", "chat", "stream", "pong") {
    auto status = context.service.ChatStreams().Pong(context.body.value("connectionId", std::uint64_t{0}),
                                                    context.identity.user_uuid);
    if (!status.ok()) { SendFailure(context, status); return; }
    SendJson(context.request, ::net::http::status::ok, Json{{"ok", true}, {"traceId", context.trace_id}}, context.trace_id);
}
}

void HandleChatSse(HttpRouteContext& context, ChatGatewayRequest request) {
    if (!context.service.SupportsStreaming()) {
        SendFailure(context, core::Status::Error(core::ErrorCode::Unimplemented, "LLM has no streaming capability"));
        return;
    }
    if (request.session_id.empty() || request.message.empty()) {
        SendFailure(context, core::Status::Error(core::ErrorCode::InvalidArgument, "sessionId and message are required"));
        return;
    }
    auto session = context.service.GetSession(request.session_id, context.trace_id, request.authenticated_user_uuid);
    if (!session.ok()) { SendFailure(context, session.status()); return; }
    if (context.message.find("Last-Event-ID") != context.message.end()) {
        SendFailure(context, core::Status::Error(core::ErrorCode::FailedPrecondition,
                                                "resume using GET /api/chat/stream/{requestId}; POST starts a new turn"));
        return;
    }
    // Service 借用引用只活在其拥有的 Registry 内；Gateway Stop 先关闭网络，再 drain Runtime。
    auto created = context.service.ChatStreams().Begin(request.trace_id, request.authenticated_user_uuid,
        [service = std::ref(context.service), session_id = request.session_id,
         owner = request.authenticated_user_uuid, trace = request.trace_id] {
            service.get().CancelChat(session_id, owner, trace);
        });
    if (!created.ok()) { SendFailure(context, created.status()); return; }
    auto channel = created.value();
    auto status = Connect(context, channel);
    if (!status.ok()) {
        channel->Complete({"TurnFailed", ErrorEnvelope(context.trace_id, status).dump(), request.trace_id + ":0"});
        return;
    }
    auto sequence = std::make_shared<std::atomic<std::uint64_t>>(0);
    request.event_sink = [channel, sequence](const llm::LlmStreamEvent& event) {
        Json data{{"requestId", event.request_id}, {"turnId", event.turn_id},
                  {"generationId", event.generation_id}, {"itemId", event.item_id}, {"sequence", event.sequence}};
        if (event.kind == llm::LlmStreamEventKind::TextDelta) data["text"] = event.text;
        if (event.kind == llm::LlmStreamEventKind::ToolCallProgress || event.kind == llm::LlmStreamEventKind::ToolExecutionState) {
            data["toolCall"] = {{"id", event.tool_call.id}, {"name", event.tool_call.name},
                                {"argumentsDelta", event.tool_call.arguments_json}};
            if (!event.text.empty()) data["state"] = event.text;
        }
        if (event.kind == llm::LlmStreamEventKind::UsageUpdated)
            data["usage"] = {{"promptTokens", event.prompt_tokens}, {"completionTokens", event.completion_tokens},
                             {"totalTokens", event.total_tokens}};
        auto status = channel->Publish({EventName(event.kind), data.dump(), event.request_id + ":" + std::to_string(event.sequence)});
        if (status.ok()) sequence->store(event.sequence);
        return status;
    };
    const auto trace = request.trace_id;
    const auto turn = request.session_id + ":" + trace;
    auto finish = [channel, sequence, trace, turn](core::Result<ChatGatewayResponse> result) {
        // 背压引发的 socket 关闭会触发取消；保留最初的 ResourceExhausted，避免被后续 Cancelled 覆盖。
        const auto delivery = channel->DeliveryStatus();
        if (!result.ok() && delivery.code() == core::ErrorCode::ResourceExhausted) result = delivery;
        Json body = result.ok() ? ChatEnvelope(result.value()) : ErrorEnvelope(trace, result.status());
        body["requestId"] = trace;
        body["turnId"] = turn;
        body["sequence"] = sequence->fetch_add(1) + 1;
        body["committed"] = result.ok();
        const auto queue = channel->QueueStats();
        body["streamQueue"] = {{"peakItems", queue.peak_queued_items}, {"peakBytes", queue.peak_queued_bytes},
                                {"rejectedItems", queue.rejected_items}};
        if (result.ok()) {
            // 文本已通过 delta 发出；终态只传提交元数据，避免完整回复再次占用单事件预算。
            body["data"]["reply"].erase("content");
        }
        auto status = channel->Complete({result.ok() ? "TurnCompleted" : "TurnFailed", body.dump(),
                                        trace + ":" + std::to_string(sequence->load())});
        if (!status.ok()) core::LoggerAdapter::ForModule("gateway-sse").warn("SSE terminal delivery failed code={}",
                                                                               static_cast<int>(status.code()));
    };
    status = context.service.SubmitChat(std::move(request), finish);
    if (!status.ok()) finish(status);
}

}
