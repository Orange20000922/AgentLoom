#pragma once

#include "gateway_auth.h"
#include "document_analysis_service.h"
#include "isemantic_cache.h"
#include "persona_gateway_route_core.h"
#include "persona_gateway_service.h"
#include "request_interfaces.h"
#include "skill_session_manager.h"
#include "stateful_skill_registry.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agent::service::gateway {

struct PersonaGatewayHttpAdapterOptions {
    bool enable_dev_registration = false;
    bool enable_path_register_test_endpoint = false;
    bool enable_path_analyze_test_endpoint = false;
    bool enable_llm_dev_routes = false;
    std::function<void(std::shared_ptr<::net::IHttpRequest>)> forward_jev;
};

class PersonaGatewayHttpAdapter {
public:
    explicit PersonaGatewayHttpAdapter(PersonaGatewayService& service,
                                       std::shared_ptr<IGatewayAuthenticator> authenticator = nullptr,
                                       std::shared_ptr<IAuthRegistrationService> auth_registration = nullptr,
                                       std::shared_ptr<document::DocumentAnalysisService> document_service = nullptr,
                                       std::shared_ptr<llm::ILlmClient> llm_client = nullptr,
                                       std::shared_ptr<document::IDocumentEmbeddingProvider> embedding_provider = nullptr,
                                       std::shared_ptr<document::IDocumentLlmChunkCache> llm_chunk_cache = nullptr,
                                       std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache = nullptr,
                                       std::shared_ptr<persona::ISkillSessionManager> skill_session_manager = nullptr,
                                       bool enable_dev_registration = false,
                                       std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router = nullptr);
    explicit PersonaGatewayHttpAdapter(PersonaGatewayService& service,
                                       std::shared_ptr<IGatewayAuthenticator> authenticator,
                                       std::shared_ptr<IAuthRegistrationService> auth_registration,
                                       std::shared_ptr<document::DocumentAnalysisService> document_service,
                                       std::shared_ptr<llm::ILlmClient> llm_client,
                                       std::shared_ptr<document::IDocumentEmbeddingProvider> embedding_provider,
                                       std::shared_ptr<document::IDocumentLlmChunkCache> llm_chunk_cache,
                                       std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache,
                                       std::shared_ptr<persona::ISkillSessionManager> skill_session_manager,
                                       PersonaGatewayHttpAdapterOptions options,
                                       std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router = nullptr);

    static bool IsApiRequest(std::string_view target) noexcept;

    void HandleHttp(std::shared_ptr<::net::IHttpRequest> request);
    void HandleWebSocket(std::shared_ptr<::net::IWebSocketStreamRequest> request);
    void CleanupUnfinishedDocumentUploadsForConnection(std::uint64_t connection_id);

private:
    PersonaGatewayService& service_;
    std::shared_ptr<IGatewayAuthenticator> authenticator_;
    std::shared_ptr<IAuthRegistrationService> auth_registration_;
    std::shared_ptr<document::DocumentAnalysisService> document_service_;
    std::shared_ptr<llm::ILlmClient> llm_client_;
    std::shared_ptr<document::IDocumentEmbeddingProvider> embedding_provider_;
    std::shared_ptr<document::IDocumentLlmChunkCache> llm_chunk_cache_;
    std::shared_ptr<semantic_cache::ISemanticCache> document_semantic_cache_;
    std::shared_ptr<persona::ISkillSessionManager> skill_session_manager_;
    std::shared_ptr<persona::IStatefulSkillExecutionRouter> stateful_skill_router_;
    PersonaGatewayHttpAdapterOptions options_;
    std::mutex document_upload_mutex_;
    std::unordered_map<std::string, DocumentUploadSession> document_uploads_;
};

} // namespace agent::service::gateway
