#pragma once

#include "../core/result.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agent::llm {

// 协议无关的公共契约；保留现有 Chat* 名称，旧 Provider 头文件仍转发这些定义。
struct ChatCompletionResponse;

enum class CompletionTokenValidationMode {
    Off,
    Audit,
    Strict,
};

class ICompletionTokenCounter;

struct CompletionResponseValidationOptions {
    bool reject_empty_content = true;
    bool reject_length_finish = true;
    CompletionTokenValidationMode token_count_mode = CompletionTokenValidationMode::Off;
    std::size_t max_token_difference = 2;
    std::shared_ptr<const ICompletionTokenCounter> token_counter;
};

enum class ChatRole {
    System,
    User,
    Assistant,
    Tool,
};

enum class ChatContentPartType {
    Text,
    ImageUrl,
};

struct ChatContentPart {
    ChatContentPartType type = ChatContentPartType::Text;
    std::string text;
    std::string image_url;

    static ChatContentPart Text(std::string text);
    static ChatContentPart ImageUrl(std::string image_url);
    static ChatContentPart ImageData(std::string_view media_type, std::string_view base64_data);
};

// 工具调用标识必须原样回传；arguments 在协议层保留为 JSON 文本。
struct ChatToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

struct ChatMessage {
    ChatRole role = ChatRole::User;
    std::string content;
    std::vector<ChatContentPart> parts;
    std::vector<ChatToolCall> tool_calls;
    std::string tool_call_id;
    std::optional<std::string> reasoning_content;
};

struct ChatCompletionRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    float temperature = 0.7f;
    int max_tokens = 0;
    float top_p = 1.0f;
    int n = 1;
    bool stream = false;
    // schema 文本保持接口轻量；发送前必须校验，不能静默降级为空对象。
    struct Tool {
        std::string name;
        std::string description;
        std::string parameters_json = R"({"type":"object"})";
    };
    std::vector<Tool> tools;
    std::string tool_choice; // 空串遵循 Provider 默认，另支持 none/auto/required。
    std::optional<bool> parallel_tool_calls;
};

struct ChatCompletionResponse {
    std::string id;
    std::string model;
    std::string content;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    std::vector<ChatToolCall> tool_calls;
    std::string finish_reason;
    std::optional<std::string> reasoning_content;
};

class ICompletionTokenCounter {
public:
    virtual ~ICompletionTokenCounter() = default;
    virtual core::Result<std::size_t> CountTokens(
        std::string_view model,
        const ChatCompletionResponse& response) const = 0;
};

// 保留旧的 Chat Completions 校验入口；可替换的 HTTP 客户端通过 ILlmProtocol 校验。
core::Status ValidateChatCompletionRequest(const ChatCompletionRequest& request);

class ILlmClient {
public:
    virtual ~ILlmClient() = default;

    /// 同步执行一次 chat completion。
    /// @param req 模型、消息和生成参数；调用期间只读。
    /// @return 传输失败、API 错误或响应格式错误时返回失败 Status。
    virtual core::Result<ChatCompletionResponse> Complete(
        const ChatCompletionRequest& req) = 0;
};

class IAsyncLlmOperation {
public:
    virtual ~IAsyncLlmOperation() = default;
    /// 幂等取消；完成 callback 仍恰好调用一次并返回 Cancelled。
    virtual void Cancel() noexcept = 0;
};

class IAsyncLlmClient {
public:
    using Callback = std::function<void(core::Result<ChatCompletionResponse>)>;

    virtual ~IAsyncLlmClient() = default;
    /// 异步执行 completion；返回句柄只用于取消，丢弃句柄不取消请求。
    virtual core::Result<std::shared_ptr<IAsyncLlmOperation>> CompleteAsync(
        ChatCompletionRequest request,
        Callback callback) = 0;
};

struct FallbackLlmClientOptions {
    int failure_threshold = 3;
    std::chrono::milliseconds primary_reconnect_interval{30000};
    bool fallback_on_primary_missing = true;
    bool fallback_on_auth_failure = true;
    bool fallback_on_unavailable = true;
};

class FallbackLlmClient final : public ILlmClient {
public:
    /// @param primary 首选客户端，可为空并按 options 决定是否直接降级。
    /// @param fallback 降级客户端；需要降级但为空时返回原始失败。
    /// @param options 连续失败阈值、探测间隔和允许降级的错误类型。
    FallbackLlmClient(std::shared_ptr<ILlmClient> primary,
                      std::shared_ptr<ILlmClient> fallback,
                      FallbackLlmClientOptions options = {});

    core::Result<ChatCompletionResponse> Complete(const ChatCompletionRequest& req) override;

private:
    bool ShouldTryPrimary(std::chrono::steady_clock::time_point now) const;
    bool ShouldFallback(const core::Status& status) const;
    void RecordPrimarySuccess();
    void RecordPrimaryFailure();

    std::shared_ptr<ILlmClient> primary_;
    std::shared_ptr<ILlmClient> fallback_;
    FallbackLlmClientOptions options_;
    mutable std::mutex mutex_;
    int consecutive_failures_ = 0;
    std::chrono::steady_clock::time_point next_primary_probe_{};
};

class LlmPromptStore {
public:
    /// Load all prompts from the given map of name → relative path.
    /// Paths are resolved relative to `config_dir`.
    /// Returns `NotFound` if any file is missing, `InternalError` on read failure.
    core::Status Load(const std::unordered_map<std::string, std::filesystem::path>& prompt_paths,
                      const std::filesystem::path& config_dir);

    /// Retrieve a loaded prompt by name.
    /// Returns `NotFound` if the prompt was not loaded.
    core::Result<std::string> Get(const std::string& name) const;

    /// Check if a prompt exists.
    bool Has(const std::string& name) const;

    /// Clear all loaded prompts.
    void Clear();

private:
    std::unordered_map<std::string, std::string> prompts_;
};


}
