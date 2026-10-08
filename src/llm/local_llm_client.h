#pragma once

#include "llm_client.h"

#include "multimodal_inference.grpc.pb.h"

#include <chrono>
#include <memory>
#include <string>

namespace agent::llm {

struct LocalLlmRequest {
    std::string prompt;
    std::string session_id;
    std::string request_id;
    std::string task_type = "chat_completion";
    int max_tokens = 0;
    float temperature = 0.7f;
    float top_p = 1.0f;
    int top_k = 40;
    int context_size = 0;
};

struct LocalLlmResponse {
    std::string text;
    int prompt_tokens = 0;
    int generated_tokens = 0;
    float prompt_eval_ms = 0.0f;
    float eval_ms = 0.0f;
    std::string result_source;
};

class ILocalLlm {
public:
    virtual ~ILocalLlm() = default;
    virtual core::Result<LocalLlmResponse> Generate(const LocalLlmRequest& request) = 0;
};

struct GrpcLocalLlmClientOptions {
    std::string target = "127.0.0.1:50051";
    std::chrono::milliseconds deadline{30000};
    std::string auth_token;
    std::string auth_metadata_key = "authorization";
};

class GrpcLocalLlmClient final : public ILocalLlm {
public:
    explicit GrpcLocalLlmClient(GrpcLocalLlmClientOptions options);
    GrpcLocalLlmClient(GrpcLocalLlmClientOptions options,
                       std::shared_ptr<grpc::Channel> channel);

    core::Result<LocalLlmResponse> Generate(const LocalLlmRequest& request) override;

private:
    static core::Status FromGrpcStatus(const grpc::Status& status);
    multimodal_inference::VLMRequest BuildGrpcRequest(const LocalLlmRequest& request) const;

    GrpcLocalLlmClientOptions options_;
    std::unique_ptr<multimodal_inference::MultimodalInference::Stub> stub_;
};

struct LocalLlmChatClientOptions {
    std::string default_model = "local-llm";
    std::string task_type = "chat_completion";
};

class LocalLlmChatClient final : public ILlmClient {
public:
    LocalLlmChatClient(std::shared_ptr<ILocalLlm> local_llm,
                       LocalLlmChatClientOptions options = {});

    core::Result<ChatCompletionResponse> Complete(const ChatCompletionRequest& req) override;

private:
    static std::string BuildPrompt(const ChatCompletionRequest& req);

    std::shared_ptr<ILocalLlm> local_llm_;
    LocalLlmChatClientOptions options_;
};

} // namespace agent::llm
