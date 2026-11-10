#include "config_section.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace server_config {
namespace {

DECLARE_CONFIG_SECTION(LlmConfigSection, "llm")
    CONFIG_CLI_STRING(kBaseUrl, "--llm-base-url");
    CONFIG_CLI_STRING(kApiKeyEnv, "--llm-api-key-env");
    CONFIG_CLI_STRING(kApiKeyFile, "--llm-api-key-file");
    CONFIG_CLI_STRING(kModel, "--llm-model");
    CONFIG_CLI_STRING(kTimeout, "--llm-timeout");
    CONFIG_CLI_STRING(kMaxRetries, "--llm-max-retries");
    void Validate(MultimodalServerOptions& options) const override;
};

void LlmConfigSection::LoadJson(const Json& root, MultimodalServerOptions& options) const {
    const Json* section = FindSection(root, Name());
    if (!section) {
        return;
    }

    SetString(*section, Name(), "base_url", options.llm.base_url);
    SetBool(*section, Name(), "enabled", options.llm.enabled);
    SetBool(*section, Name(), "debug_payloads", options.llm.debug_payloads);
    SetSize(*section, Name(), "debug_payload_max_bytes", options.llm.debug_payload_max_bytes, 16, 16 * 1024 * 1024);
    SetString(*section, Name(), "api_key_env", options.llm.api_key_env);
    SetString(*section, Name(), "api_key_file", options.llm.api_key_file);
    SetString(*section, Name(), "model", options.llm.model);
    SetInt(*section, Name(), "timeout_ms", options.llm.timeout_ms, 1000, 600000);
    SetInt(*section, Name(), "max_retries", options.llm.max_retries, 0, 10);
    SetSize(*section, Name(), "async_http_io_threads", options.llm.async_http_io_threads, 1);
    SetBool(*section, Name(), "http_keep_alive", options.llm.http_keep_alive);
    SetSize(*section, Name(), "http_max_idle_connections",
            options.llm.http_max_idle_connections, 1);
    SetSize(*section, Name(), "http_max_idle_connections_per_origin",
            options.llm.http_max_idle_connections_per_origin, 1);
    SetInt(*section, Name(), "http_idle_timeout_ms",
           options.llm.http_idle_timeout_ms, 1, 600000);
    SetBool(*section, Name(), "require_api_key", options.llm.require_api_key);
    SetBool(*section, Name(), "allow_placeholder", options.llm.allow_placeholder);
    SetBool(*section, Name(), "disable_tls_verify_on_windows", options.llm.disable_tls_verify_on_windows);
    SetString(*section, Name(), "ca_bundle_path", options.llm.ca_bundle_path);
    if (const Json* validation = FindField(*section, Name(), "response_validation")) {
        if (!validation->is_object()) {
            throw std::runtime_error("llm.response_validation must be an object");
        }
        SetString(*validation, "llm.response_validation", "token_count_mode",
                  options.llm.response_token_count_mode);
        SetPath(*validation, "llm.response_validation", "tokenizer_path",
                options.llm.response_tokenizer_path);
        SetString(*validation, "llm.response_validation", "tokenizer_model",
                  options.llm.response_tokenizer_model);
        SetSize(*validation, "llm.response_validation", "max_token_difference",
                options.llm.response_max_token_difference, 0, 1024);
    }

    // Prompt 路径保持配置中的相对形式，由 LlmPromptStore::Load 统一解析。
    if (const Json* prompts_field = FindField(*section, Name(), "prompts")) {
        if (!prompts_field->is_object()) {
            throw std::runtime_error("llm.prompts must be an object");
        }
        for (auto it = prompts_field->begin(); it != prompts_field->end(); ++it) {
            if (!it.value().is_string()) {
                throw std::runtime_error("llm.prompts values must be strings");
            }
            options.llm.prompts[it.key()] = it.value().get<std::string>();
        }
    }
}

bool LlmConfigSection::LoadCli(CliCursor& cursor, MultimodalServerOptions& options) const {
    CliArgumentParser parser(cursor);

    CONFIG_VALUE_ARG(kBaseUrl, value, options.llm.base_url = *value;)
    CONFIG_VALUE_ARG(kApiKeyEnv, value, options.llm.api_key_env = *value;)
    CONFIG_VALUE_ARG(kApiKeyFile, value, options.llm.api_key_file = *value;)
    CONFIG_VALUE_ARG(kModel, value, options.llm.model = *value;)
    CONFIG_VALUE_ARG(kTimeout, value, {
        options.llm.timeout_ms = ParsePositiveOption(kTimeout, *value);
    })
    CONFIG_VALUE_ARG(kMaxRetries, value, {
        options.llm.max_retries = ParseNonNegativeOption(kMaxRetries, *value);
    })

    return false;
}

// API key 按环境变量、文件、报错的顺序解析，与认证配置保持一致。
namespace {

std::optional<std::string> ReadKeyFromEnv(const std::string& name) {
    if (name.empty()) return std::nullopt;
    const char* val = std::getenv(name.c_str());
    if (!val || *val == '\0') return std::nullopt;
    return std::string(val);
}

std::string ReadKeyFromFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Failed to open LLM api_key_file: " + path.string());
    }
    std::string key((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    // 凭据文件按 UTF-8 读取，兼容可选 BOM，并去除首尾空白。
    constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
    if (key.starts_with(utf8_bom)) {
        key.erase(0, utf8_bom.size());
    }
    while (!key.empty() && (std::isspace(static_cast<unsigned char>(key.back())))) {
        key.pop_back();
    }
    auto first = key.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) key = key.substr(first);
    return key;
}

std::filesystem::path ResolveRelativeToConfig(const std::filesystem::path& p,
                                               const std::filesystem::path& config_path) {
    if (p.empty() || p.is_absolute() || config_path.empty()) {
        return p;
    }
    return config_path.parent_path() / p;
}

}

void LlmConfigSection::Validate(MultimodalServerOptions& options) const {
    if (options.llm.response_token_count_mode != "off" &&
        options.llm.response_token_count_mode != "audit" &&
        options.llm.response_token_count_mode != "strict") {
        throw std::runtime_error(
            "llm.response_validation.token_count_mode must be off, audit, or strict");
    }
    options.llm.response_tokenizer_path = ResolveRelativeToConfig(
        options.llm.response_tokenizer_path, options.config_file_path);
    if (options.llm.response_token_count_mode == "strict" &&
        options.llm.response_tokenizer_path.empty()) {
        throw std::runtime_error(
            "llm.response_validation.tokenizer_path is required in strict mode");
    }
    // 未启用或未配置 base_url 时允许只使用本地 LLM。
    if (!options.llm.enabled || options.llm.base_url.empty()) {
        return;
    }

    if (options.llm.model.empty()) {
        throw std::runtime_error("llm.model must not be empty when base_url is set");
    }
    if (options.llm.http_keep_alive &&
        options.llm.http_max_idle_connections_per_origin >
            options.llm.http_max_idle_connections) {
        // 单 origin 空闲连接属于全局空闲连接集合，不能超过其全局上限。
        throw std::runtime_error(
            "llm.http_max_idle_connections_per_origin must not exceed "
            "llm.http_max_idle_connections");
    }

    // 环境变量优先于文件，避免配置文件覆盖部署时注入的凭据。
    if (auto key = ReadKeyFromEnv(options.llm.api_key_env)) {
        options.llm.api_key = *key;
        return;
    }
    if (!options.llm.api_key_file.empty()) {
        auto path = ResolveRelativeToConfig(options.llm.api_key_file,
                                              options.config_file_path);
        options.llm.api_key = ReadKeyFromFile(path);
        if (options.llm.api_key.empty()) {
            throw std::runtime_error("LLM api_key_file resolved to an empty key");
        }
        return;
    }

    if (options.llm.require_api_key) {
        throw std::runtime_error(
            "LLM base_url is set but no API key found. "
            "Set env var " + options.llm.api_key_env +
            " or set llm.api_key_file in config.");
    }
}

}

REGISTER_CONFIG_SECTION(LlmConfigSection)

}
