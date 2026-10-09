#pragma once

namespace agent::service::gateway {
struct HttpRouteContext;
struct ChatGatewayRequest;
void HandleChatSse(HttpRouteContext& context, ChatGatewayRequest request);
}
