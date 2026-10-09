#include <AgentLoom/core/memory_pool.h>
#include <AgentLoom/config/config_section.h>
#include <AgentLoom/config/server_config.h>
#include <AgentLoom/generated/bert_inference.grpc.pb.h>
#include <AgentLoom/net/http_server.h>
#include <AgentLoom/service/gateway/gateway_lifecycle.h>
#include <AgentLoom/service/gateway/gateway_routing.h>
#include <AgentLoom/service/persona/persona_interaction.h>
#include <AgentLoom/llm/llm_client.h>
#include <AgentLoom/llm/llm_protocol.h>
#include <AgentLoom/llm/openai_llm_client.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ConsumerRouteContext {};

class ConsumerRoute final
    : public agent::service::gateway::ITypedHttpRoute<ConsumerRouteContext> {
public:
    ::net::http::verb Method() const noexcept override {
        return ::net::http::verb::get;
    }

    std::vector<std::string_view> Pattern() const override {
        return {"runtime", "{runtime_id}"};
    }

    void Handle(ConsumerRouteContext&) const override {}
};

std::unique_ptr<agent::service::gateway::ITypedHttpRoute<ConsumerRouteContext>>
MakeConsumerRoute() {
    return std::make_unique<ConsumerRoute>();
}

bool VerifyInstalledWebSocketUpgrade() {
    ::net::HttpServer server({.address = "127.0.0.1", .port = 0, .io_threads = 1});
    server.SetWebSocketHandler(
        "/sdk-ws",
        [](::net::WebSocketSessionHandle&, ::net::WebSocketMessage) {});
    if (auto status = server.Start(); !status.ok()) {
        return false;
    }

    namespace asio = boost::asio;
    namespace websocket = boost::beast::websocket;
    using tcp = asio::ip::tcp;
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    websocket::stream<tcp::socket> client(io_context);
    boost::system::error_code error;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server.port()), error);
    if (!error) {
        asio::connect(client.next_layer(), endpoints, error);
    }
    if (!error) {
        client.handshake("127.0.0.1", "/sdk-ws", error);
    }
    const bool upgraded = !error;
    if (upgraded) {
        client.close(websocket::close_code::normal, error);
    }
    server.Stop();
    return upgraded;
}

}

int main() {
    // 新公共入口与旧 Provider 入口必须可并存；协议在安装包中可直接链接与调用。
    const agent::llm::ChatCompletionsProtocol protocol;
    agent::llm::ChatCompletionRequest llm_request;
    llm_request.messages.push_back({agent::llm::ChatRole::User, "SDK protocol check"});
    auto encoded = protocol.EncodeRequest(llm_request, {"sdk-model", {}});
    if (!encoded.ok() || protocol.Endpoint() != "chat/completions" ||
        encoded.value().find("sdk-model") == std::string::npos ||
        !agent::llm::ValidateChatCompletionRequest(llm_request).ok()) {
        return 9;
    }
    auto protocol_logger = core::LoggerAdapter::ForModule("sdk-consumer");
    auto decoded = protocol.DecodeResponse(
        R"({"choices":[{"message":{"content":"SDK reply"}}]})",
        llm_request, {}, protocol_logger);
    if (!decoded.ok() || decoded.value().content != "SDK reply") return 10;
    // 安装包必须包含 SSE framing 的链接依赖与 decoder 工厂，不只验证头文件存在。
    llm_request.stream = true;
    auto stream_decoder = protocol.CreateStreamDecoder(llm_request, {});
    if (!protocol.Capabilities().streaming || !stream_decoder.ok()) return 11;
    auto stream_status = stream_decoder.value()->Feed(
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"SDK stream\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n");
    if (!stream_status.ok() || !stream_decoder.value()->Finish().ok()) return 12;

    core::BucketMemoryPool pool;
    auto block = pool.allocate(256);
    if (!block.ok() || block.value().size() != 256) {
        return 1;
    }

    server_config::Json helper_json{{"value", "from-helper"}};
    std::string helper_value;
    server_config::SetString(helper_json, "helper", "value", helper_value);
    if (helper_value != "from-helper") {
        return 2;
    }

    const auto selection = server_config::ConfigSectionSelection::Only({"llm", "sdk_consumer"});
    const auto sections = server_config::BuildConfigSections(selection);
    if (sections.size() != 2) {
        return 3;
    }

    const auto temp = std::filesystem::temp_directory_path() / "agentloom_package_consumer";
    std::filesystem::create_directories(temp);
    {
        std::ofstream key(temp / "api-key.txt", std::ios::binary);
        key << "package-key\n";
        std::ofstream config(temp / "config.json", std::ios::binary);
        config << R"({
            "llm": {
                "enabled": true,
                "base_url": "https://example.invalid/v1",
                "model": "package-model",
                "api_key_file": "api-key.txt"
            },
            "sdk_consumer": { "value": "registered" }
        })";
    }

    MultimodalServerOptions options;
    server_config::LoadConfigFile(temp / "config.json", options, selection);
    server_config::ValidateOptions(options, selection);
    std::error_code ignored;
    std::filesystem::remove_all(temp, ignored);

    bert_inference::PredictRequest generated_request;
    generated_request.add_input_ids(42);
    agent::service::gateway::TypedRouteRegistry<
        agent::service::gateway::ITypedHttpRoute<ConsumerRouteContext>> route_registry;
    if (!route_registry.Register("consumer-runtime", &MakeConsumerRoute).ok()) {
        return 5;
    }
    agent::service::gateway::TypedHttpRouteDispatcher<ConsumerRouteContext> dispatcher(route_registry);
    auto route = dispatcher.Match(::net::http::verb::get, {"runtime", "runtime-001"});
    if (!route.ok() || route.value().path_params.at("runtime_id") != "runtime-001") {
        return 6;
    }
    agent::service::gateway::GatewayLifecycleCoordinator lifecycle;
    if (!lifecycle.Start().ok() || !lifecycle.Stop().ok()) {
        return 7;
    }
    if (!VerifyInstalledWebSocketUpgrade()) {
        return 8;
    }
    return options.llm.api_key == "package-key" &&
                   options.auth.token == "registered" &&
                   generated_request.input_ids_size() == 1 &&
                   generated_request.input_ids(0) == 42
               ? 0
               : 4;
}
