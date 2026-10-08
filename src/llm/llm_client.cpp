#include "llm_client.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace agent::llm {

ChatContentPart ChatContentPart::Text(std::string text) {
    ChatContentPart part;
    part.type = ChatContentPartType::Text;
    part.text = std::move(text);
    return part;
}

ChatContentPart ChatContentPart::ImageUrl(std::string image_url) {
    ChatContentPart part;
    part.type = ChatContentPartType::ImageUrl;
    part.image_url = std::move(image_url);
    return part;
}

ChatContentPart ChatContentPart::ImageData(std::string_view media_type, std::string_view base64_data) {
    ChatContentPart part;
    part.type = ChatContentPartType::ImageUrl;
    part.image_url = "data:";
    part.image_url.append(media_type);
    part.image_url.append(";base64,");
    part.image_url.append(base64_data);
    return part;
}

core::Status LlmPromptStore::Load(
    const std::unordered_map<std::string, std::filesystem::path>& prompt_paths,
    const std::filesystem::path& config_dir) {

    prompts_.clear();

    for (const auto& [name, relative_path] : prompt_paths) {
        std::filesystem::path full_path = config_dir / relative_path;

        if (!std::filesystem::exists(full_path)) {
            return core::Status(core::ErrorCode::NotFound,
                "Prompt file not found: " + full_path.string());
        }

        std::ifstream file(full_path, std::ios::in | std::ios::binary);
        if (!file) {
            return core::Status(core::ErrorCode::InternalError,
                "Failed to open prompt file: " + full_path.string());
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        if (file.bad()) {
            return core::Status(core::ErrorCode::InternalError,
                "Failed to read prompt file: " + full_path.string());
        }

        prompts_[name] = buffer.str();
    }

    return core::Status::Ok();
}

core::Result<std::string> LlmPromptStore::Get(const std::string& name) const {
    auto it = prompts_.find(name);
    if (it == prompts_.end()) {
        return core::Status(core::ErrorCode::NotFound,
            "Prompt not found: " + name);
    }
    return it->second;
}

bool LlmPromptStore::Has(const std::string& name) const {
    return prompts_.find(name) != prompts_.end();
}

void LlmPromptStore::Clear() {
    prompts_.clear();
}

FallbackLlmClient::FallbackLlmClient(std::shared_ptr<ILlmClient> primary,
                                     std::shared_ptr<ILlmClient> fallback,
                                     FallbackLlmClientOptions options)
    : primary_(std::move(primary)),
      fallback_(std::move(fallback)),
      options_(options) {}

core::Result<ChatCompletionResponse> FallbackLlmClient::Complete(const ChatCompletionRequest& req) {
    if (!fallback_) {
        return core::Status(core::ErrorCode::FailedPrecondition, "fallback llm client is required");
    }

    const auto now = std::chrono::steady_clock::now();
    if (!primary_) {
        if (options_.fallback_on_primary_missing) {
            return fallback_->Complete(req);
        }
        return core::Status(core::ErrorCode::FailedPrecondition, "primary llm client is missing");
    }

    if (!ShouldTryPrimary(now)) {
        return fallback_->Complete(req);
    }

    auto primary_result = primary_->Complete(req);
    if (primary_result.ok()) {
        RecordPrimarySuccess();
        return primary_result;
    }

    RecordPrimaryFailure();
    if (!ShouldFallback(primary_result.status())) {
        return primary_result.status();
    }

    auto fallback_result = fallback_->Complete(req);
    if (fallback_result.ok()) {
        return fallback_result;
    }

    return core::Status(
        fallback_result.status().code(),
        "primary llm failed: " + primary_result.status().message() +
            "; fallback llm failed: " + fallback_result.status().message());
}

bool FallbackLlmClient::ShouldTryPrimary(std::chrono::steady_clock::time_point now) const {
    std::lock_guard lock(mutex_);
    return consecutive_failures_ < options_.failure_threshold || now >= next_primary_probe_;
}

bool FallbackLlmClient::ShouldFallback(const core::Status& status) const {
    switch (status.code()) {
    case core::ErrorCode::InvalidArgument:
    case core::ErrorCode::FailedPrecondition:
        return options_.fallback_on_primary_missing;
    case core::ErrorCode::PermissionDenied:
        return options_.fallback_on_auth_failure;
    case core::ErrorCode::Unavailable:
    case core::ErrorCode::Timeout:
    case core::ErrorCode::ResourceExhausted:
        return options_.fallback_on_unavailable;
    default:
        return false;
    }
}

void FallbackLlmClient::RecordPrimarySuccess() {
    std::lock_guard lock(mutex_);
    consecutive_failures_ = 0;
    next_primary_probe_ = {};
}

void FallbackLlmClient::RecordPrimaryFailure() {
    std::lock_guard lock(mutex_);
    ++consecutive_failures_;
    if (consecutive_failures_ >= options_.failure_threshold) {
        next_primary_probe_ = std::chrono::steady_clock::now() + options_.primary_reconnect_interval;
    }
}


}
