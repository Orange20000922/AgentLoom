#pragma once

#include "document_analysis_service.h"
#include "gateway_auth.h"
#include "gateway_routing.h"
#include "isemantic_cache.h"
#include "persona_gateway_service.h"
#include "request_interfaces.h"
#include "skill_session_manager.h"
#include "stateful_skill_registry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agent::service::gateway {

struct DocumentUploadSession {
    std::string upload_id;
    std::string file_name;
    std::string owner_user_uuid;
    std::string session_id;
    std::filesystem::path temp_path;
    std::uint64_t expected_size = 0;
    std::uint64_t received_size = 0;
    std::uint64_t connection_id = 0;
    bool binary_mode = false;
};

struct HttpRouteContext {
    PersonaGatewayService& service;
    std::shared_ptr<document::DocumentAnalysisService> document_service;
    std::shared_ptr<llm::ILlmClient> llm_client;
    std::shared_ptr<document::IDocumentEmbeddingProvider> embedding_provider;
    std::shared_ptr<document::IDocumentLlmChunkCache> llm_chunk_cache;
    std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache;
    std::shared_ptr<persona::ISkillSessionManager> skill_session_manager;
    std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router;
    std::shared_ptr<IAuthRegistrationService> auth_registration;
    bool enable_dev_registration = false;
    bool enable_path_register_test_endpoint = false;
    bool enable_path_analyze_test_endpoint = false;
    std::shared_ptr<::net::IHttpRequest> request;
    const ::net::BeastHttpRequest& message;
    const nlohmann::json& body;
    const AuthIdentity& identity;
    std::string trace_id;
    std::vector<std::string> path_parts;
    std::unordered_map<std::string, std::string> path_params;
    std::function<void(std::shared_ptr<::net::IHttpRequest>)> forward_jev;
};

struct WsRouteContext {
    PersonaGatewayService& service;
    std::shared_ptr<document::DocumentAnalysisService> document_service;
    std::shared_ptr<llm::ILlmClient> llm_client;
    std::shared_ptr<document::IDocumentEmbeddingProvider> embedding_provider;
    std::shared_ptr<document::IDocumentLlmChunkCache> llm_chunk_cache;
    std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache;
    std::shared_ptr<persona::ISkillSessionManager> skill_session_manager;
    std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router;
    std::shared_ptr<::net::IWebSocketStreamRequest> request;
    const nlohmann::json& body;
    const AuthIdentity& identity;
    std::string trace_id;
    std::mutex& upload_mutex;
    std::unordered_map<std::string, DocumentUploadSession>& document_uploads;
};

using IHttpRoute = ITypedHttpRoute<HttpRouteContext>;

using HttpRouteFactory = std::unique_ptr<IHttpRoute> (*)();

using IWsRoute = ITypedWsRoute<WsRouteContext>;

using WsRouteFactory = std::unique_ptr<IWsRoute> (*)();

class HttpRouteRegistry : public TypedRouteRegistry<IHttpRoute> {
public:
    static HttpRouteRegistry& Instance() {
        static HttpRouteRegistry registry;
        return registry;
    }

};

class WsRouteRegistry : public TypedRouteRegistry<IWsRoute> {
public:
    static WsRouteRegistry& Instance() {
        static WsRouteRegistry registry;
        return registry;
    }

};

template <typename T>
class HttpRouteRegistrar {
public:
    HttpRouteRegistrar() {
        static_cast<void>(HttpRouteRegistry::Instance().Register(
            T::kRouteName,
            []() -> std::unique_ptr<IHttpRoute> {
                return std::make_unique<T>();
            }));
    }
};

template <typename T>
class WsRouteRegistrar {
public:
    WsRouteRegistrar() {
        static_cast<void>(WsRouteRegistry::Instance().Register(
            T::kRouteName,
            []() -> std::unique_ptr<IWsRoute> {
                return std::make_unique<T>();
            }));
    }
};

#define DECLARE_HTTP_ROUTE(ClassName, MethodValue, ...) \
class ClassName final : public IHttpRoute { \
public: \
    static constexpr std::string_view kRouteName = #ClassName; \
    ::net::http::verb Method() const noexcept override { return MethodValue; } \
    std::vector<std::string_view> Pattern() const override { return {__VA_ARGS__}; } \
    void Handle(HttpRouteContext& context) const override; \
}; \
static const HttpRouteRegistrar<ClassName> g_##ClassName##_registrar; \
void ClassName::Handle(HttpRouteContext& context) const

#define DECLARE_AUTHENTICATED_HTTP_ROUTE(ClassName, MethodValue, ...) \
class ClassName final : public IHttpRoute { \
public: \
    static constexpr std::string_view kRouteName = #ClassName; \
    ::net::http::verb Method() const noexcept override { return MethodValue; } \
    std::vector<std::string_view> Pattern() const override { return {__VA_ARGS__}; } \
    bool RequiresAuthenticatedIdentity() const noexcept override { return true; } \
    void Handle(HttpRouteContext& context) const override; \
}; \
static const HttpRouteRegistrar<ClassName> g_##ClassName##_registrar; \
void ClassName::Handle(HttpRouteContext& context) const

#define DECLARE_PUBLIC_HTTP_ROUTE(ClassName, MethodValue, ...) \
class ClassName final : public IHttpRoute { \
public: \
    static constexpr std::string_view kRouteName = #ClassName; \
    ::net::http::verb Method() const noexcept override { return MethodValue; } \
    std::vector<std::string_view> Pattern() const override { return {__VA_ARGS__}; } \
    bool RequiresAuth() const noexcept override { return false; } \
    void Handle(HttpRouteContext& context) const override; \
}; \
static const HttpRouteRegistrar<ClassName> g_##ClassName##_registrar; \
void ClassName::Handle(HttpRouteContext& context) const

#define DECLARE_WS_ROUTE(ClassName, TypeValue) \
class ClassName final : public IWsRoute { \
public: \
    static constexpr std::string_view kRouteName = #ClassName; \
    std::string_view Type() const noexcept override { return TypeValue; } \
    void Handle(WsRouteContext& context) const override; \
}; \
static const WsRouteRegistrar<ClassName> g_##ClassName##_registrar; \
void ClassName::Handle(WsRouteContext& context) const

#define DECLARE_AUTHENTICATED_WS_ROUTE(ClassName, TypeValue) \
class ClassName final : public IWsRoute { \
public: \
    static constexpr std::string_view kRouteName = #ClassName; \
    std::string_view Type() const noexcept override { return TypeValue; } \
    bool RequiresAuthenticatedIdentity() const noexcept override { return true; } \
    void Handle(WsRouteContext& context) const override; \
}; \
static const WsRouteRegistrar<ClassName> g_##ClassName##_registrar; \
void ClassName::Handle(WsRouteContext& context) const

} // namespace agent::service::gateway
