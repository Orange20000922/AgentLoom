#include "config_section.h"
#include "option_parser.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ArgvBuilder {
public:
    ArgvBuilder(std::initializer_list<std::string_view> args) {
        storage_.reserve(args.size());
        argv_.reserve(args.size());
        for (std::string_view arg : args) {
            storage_.emplace_back(arg);
        }
        for (std::string& arg : storage_) {
            argv_.push_back(arg.data());
        }
    }

    explicit ArgvBuilder(std::vector<std::string> args)
        : storage_(std::move(args)) {
        argv_.reserve(storage_.size());
        for (std::string& arg : storage_) {
            argv_.push_back(arg.data());
        }
    }

    int argc() const noexcept {
        return static_cast<int>(argv_.size());
    }

    char** argv() noexcept {
        return argv_.data();
    }

private:
    std::vector<std::string> storage_;
    std::vector<char*> argv_;
};

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(std::string_view prefix)
        : path_(std::filesystem::temp_directory_path() /
                (std::string(prefix) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(std::rand()))) {
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::filesystem::path WriteFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to create test file");
    }
    file << content;
    return path;
}

MultimodalServerOptions Parse(std::initializer_list<std::string_view> args) {
    ArgvBuilder argv(args);
    return ParseMultimodalOptions(argv.argc(), argv.argv());
}

}

TEST(ConfigOptionParserTest, DetectsHelpFlag) {
    ArgvBuilder long_help({"server", "--help"});
    EXPECT_TRUE(IsHelpRequested(long_help.argc(), long_help.argv()));

    ArgvBuilder short_help({"server", "-h"});
    EXPECT_TRUE(IsHelpRequested(short_help.argc(), short_help.argv()));

    ArgvBuilder no_help({"server", "--llm", "model.gguf"});
    EXPECT_FALSE(IsHelpRequested(no_help.argc(), no_help.argv()));
}

TEST(ConfigSectionSelectionTest, BuildsAndValidatesOnlySelectedSections) {
    const auto selection = server_config::ConfigSectionSelection::Only({"llm", "grpc"});
    const auto sections = server_config::BuildConfigSections(selection);

    ASSERT_EQ(sections.size(), 2u);
    std::vector<std::string_view> names;
    for (const auto& section : sections) {
        names.push_back(section->Name());
    }
    EXPECT_NE(std::find(names.begin(), names.end(), "grpc"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "llm"), names.end());

    MultimodalServerOptions options;
    EXPECT_NO_THROW(server_config::ValidateOptions(options, selection));
    EXPECT_THROW(server_config::ValidateOptions(options), std::runtime_error);
}

TEST(ConfigEmotionSectionTest, LoadsGenerationBudgetAndWeights) {
    ScopedTempDirectory tmp("emotion_generation_cfg");
    const auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "emotion": {
            "generation": {
                "max_tokens": 3072,
                "min_tokens": 256,
                "max_token_ratio": 1.5,
                "default_token_weight": 0.95,
                "high_intensity_threshold": 0.8,
                "high_intensity_multiplier": 1.2,
                "token_weights": {"neutral": 1.0, "curiosity": 1.25}
            }
        }
    })");

    auto options = Parse({"server", "--llm", "llm.gguf", "--config", config_file.string()});
    EXPECT_EQ(options.emotion_generation.max_tokens, 3072);
    EXPECT_EQ(options.emotion_generation.min_tokens, 256);
    EXPECT_DOUBLE_EQ(options.emotion_generation.max_token_ratio, 1.5);
    EXPECT_DOUBLE_EQ(options.emotion_generation.token_weights.at("curiosity"), 1.25);
}

TEST(ConfigLlmSectionTest, RejectsStrictTokenValidationWithoutTokenizer) {
    ScopedTempDirectory tmp("llm_validation_cfg");
    const auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "llm": {
            "response_validation": {"token_count_mode": "strict"}
        }
    })");

    EXPECT_THROW(
        Parse({"server", "--llm", "llm.gguf", "--config", config_file.string()}),
        std::runtime_error);
}

TEST(ConfigLlmSectionTest, LoadsAuditTokenValidationOptions) {
    ScopedTempDirectory tmp("llm_validation_audit_cfg");
    const auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "llm": {
            "response_validation": {
                "token_count_mode": "audit",
                "tokenizer_path": "tokenizer.json",
                "tokenizer_model": "deepseek-v3",
                "max_token_difference": 4
            }
        }
    })");

    auto options = Parse({"server", "--llm", "llm.gguf", "--config", config_file.string()});
    EXPECT_EQ(options.llm.response_token_count_mode, "audit");
    EXPECT_EQ(options.llm.response_tokenizer_path, tmp.path() / "tokenizer.json");
    EXPECT_EQ(options.llm.response_tokenizer_model, "deepseek-v3");
    EXPECT_EQ(options.llm.response_max_token_difference, 4u);
}

TEST(ConfigOptionParserTest, ParsesCliFallbackOptionsThroughSections) {
    auto options = Parse({
        "server",
        "--llm", "llm.gguf",
        "--bert", "bert.onnx",
        "--vit", "vit.onnx",
        "--mmproj", "mmproj.gguf",
        "--ngl", "12",
        "--provider", "cuda",
        "--cuda-device", "1",
        "--host", "0.0.0.0",
        "--port", "6000",
        "--grpc-num-cqs", "2",
        "--grpc-min-pollers", "3",
        "--grpc-max-pollers", "2",
        "--max-recv-mb", "128",
        "--max-send-mb", "32",
        "--stats-log-interval-seconds", "-1",
        "--slow-request-ms", "-5",
        "--auth-token", "token-123",
        "--auth-header", "x-test-auth",
        "--max-image-mb", "8",
        "--max-context-size", "2048",
        "--vlm-cache-enabled",
        "--vlm-cache-reuse-policy", "prompt_kv_vector",
        "--vlm-cache-persist",
        "--vlm-cache-dir", "cache/test",
        "--vlm-vector-cache-enabled",
        "--vlm-prompt-kv-cache-enabled",
        "--vlm-prompt-kv-backend", "memory",
        "--vram-warning-free-mb", "256",
        "--vram-unload-free-mb", "128"
    });

    EXPECT_EQ(options.llm_model, "llm.gguf");
    EXPECT_EQ(options.bert_model, "bert.onnx");
    EXPECT_EQ(options.vit_model, "vit.onnx");
    EXPECT_EQ(options.mmproj, "mmproj.gguf");
    EXPECT_EQ(options.n_gpu_layers, 12);
    EXPECT_EQ(options.bert_runtime.execution_provider, "cuda");
    EXPECT_EQ(options.bert_runtime.cuda_device_id, 1);
    EXPECT_EQ(options.grpc.host, "0.0.0.0");
    EXPECT_EQ(options.grpc.port, "6000");
    EXPECT_EQ(options.grpc.grpc_num_cqs, 2);
    EXPECT_EQ(options.grpc.grpc_min_pollers, 3);
    EXPECT_EQ(options.grpc.grpc_max_pollers, 3);
    EXPECT_EQ(options.grpc.max_receive_message_mb, 128);
    EXPECT_EQ(options.grpc.max_send_message_mb, 32);
    EXPECT_EQ(options.grpc.stats_log_interval_seconds, 0);
    EXPECT_EQ(options.grpc.slow_request_ms, 0);
    EXPECT_EQ(options.auth.token, "token-123");
    EXPECT_EQ(options.auth_source, "command-line");
    EXPECT_EQ(options.auth.metadata_key, "x-test-auth");
    EXPECT_EQ(options.limits.max_image_bytes, 8u * 1024u * 1024u);
    EXPECT_EQ(options.limits.max_context_size, 2048);
    EXPECT_TRUE(options.vlm_cache.enabled);
    EXPECT_EQ(options.vlm_cache.reuse_policy, "prompt_kv_vector");
    EXPECT_TRUE(options.vlm_cache.persist);
    EXPECT_TRUE(options.vlm_cache_vector.enabled);
    EXPECT_EQ(options.vlm_cache.cache_dir, std::filesystem::path("cache/test"));
    EXPECT_EQ(options.vram.warning_free_bytes, 256u * 1024u * 1024u);
    EXPECT_EQ(options.vram.unload_free_bytes, 128u * 1024u * 1024u);
}

TEST(ConfigOptionParserTest, LoadsConfigFileBeforeCliOverrides) {
    ScopedTempDirectory temp("agent_config_test");
    const auto config_path = WriteFile(
        temp.path() / "server.json",
        R"({
            "models": {
                "llm": "config.gguf",
                "bert": "config-bert.onnx",
                "n_gpu_layers": 4,
                "provider": "cpu"
            },
            "grpc": {
                "host": "127.0.0.2",
                "port": "50099",
                "max_receive_message_mb": 64,
                "num_cqs": 1
            },
            "auth": {
                "metadata_key": "x-config-auth"
            },
            "limits": {
                "min_context_size": 128,
                "max_context_size": 1024,
                "max_image_mb": 2
            },
            "vlm_cache": {
                "enabled": true,
                "reuse_policy": "prompt_kv_vector",
                "persist": false,
                "dir": "cache/from-config",
                "vector": {
                    "enabled": true,
                    "sim_threshold_high": 0.95,
                    "dir": "cache/from-config/vectors"
                },
                "prompt_kv": {
                    "enabled": true,
                    "backend": "memory",
                    "max_mb": 256
                }
            },
            "vram_guard": {
                "warning_free_mb": 512,
                "unload_free_mb": 256
            }
        })");

    ArgvBuilder argv(std::vector<std::string>{
        "server",
        "--config",
        config_path.string(),
        "--llm", "cli.gguf",
        "--port", "50100",
        "--max-context-size", "2048"
    });

    auto options = ParseMultimodalOptions(argv.argc(), argv.argv());

    EXPECT_EQ(options.llm_model, "cli.gguf");
    EXPECT_EQ(options.bert_model, "config-bert.onnx");
    EXPECT_EQ(options.n_gpu_layers, 4);
    EXPECT_EQ(options.bert_runtime.execution_provider, "cpu");
    EXPECT_EQ(options.grpc.host, "127.0.0.2");
    EXPECT_EQ(options.grpc.port, "50100");
    EXPECT_EQ(options.grpc.max_receive_message_mb, 64);
    EXPECT_EQ(options.grpc.grpc_num_cqs, 1);
    EXPECT_EQ(options.auth.metadata_key, "x-config-auth");
    EXPECT_EQ(options.limits.max_context_size, 2048);
    EXPECT_EQ(options.limits.max_image_bytes, 2u * 1024u * 1024u);
    EXPECT_TRUE(options.vlm_cache.enabled);
    EXPECT_EQ(options.vlm_cache.reuse_policy, "prompt_kv_vector");
    EXPECT_FALSE(options.vlm_cache.persist);
    EXPECT_EQ(options.vlm_cache.cache_dir, std::filesystem::path("cache/from-config"));
    EXPECT_TRUE(options.vlm_cache_vector.enabled);
    EXPECT_FLOAT_EQ(options.vlm_cache_vector.sim_threshold_high, 0.95f);
    EXPECT_EQ(options.vlm_cache_vector.vector_dir, std::filesystem::path("cache/from-config/vectors"));
    EXPECT_TRUE(options.vlm_prompt_kv_cache.enabled);
    EXPECT_EQ(options.vlm_prompt_kv_cache.backend, "memory");
    EXPECT_EQ(options.vlm_prompt_kv_cache.max_bytes, 256u * 1024u * 1024u);
    EXPECT_EQ(options.vram.warning_free_bytes, 512u * 1024u * 1024u);
    EXPECT_EQ(options.vram.unload_free_bytes, 256u * 1024u * 1024u);
}

TEST(ConfigOptionParserTest, RejectsPromptKvVectorPolicyWithoutRequiredCaches) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--vlm-cache-reuse-policy", "prompt_kv_vector"
        }),
        std::runtime_error);
}

TEST(ConfigOptionParserTest, ParsesTieredVlmCachePolicyAndNearThresholds) {
    auto options = Parse({
        "server",
        "--llm", "llm.gguf",
        "--vlm-cache-enabled",
        "--vlm-vector-cache-enabled",
        "--vlm-prompt-kv-cache-enabled",
        "--vlm-prompt-kv-backend", "memory",
        "--vlm-prompt-kv-near-enabled",
        "--vlm-prompt-kv-near-same-session-min-cosine", "0.998",
        "--vlm-prompt-kv-near-cross-session-min-cosine", "0.9995",
        "--vlm-prompt-kv-near-same-session-min-token-mean", "0.991",
        "--vlm-prompt-kv-near-cross-session-min-token-mean", "0.996",
        "--vlm-prompt-kv-near-same-session-min-token-p05", "0.96",
        "--vlm-prompt-kv-near-cross-session-min-token-p05", "0.985",
        "--vlm-prompt-kv-near-same-session-max-relative-l2", "0.14",
        "--vlm-prompt-kv-near-cross-session-max-relative-l2", "0.08",
        "--vlm-cache-reuse-policy", "tiered"
    });

    EXPECT_EQ(options.vlm_cache.reuse_policy, "tiered");
    EXPECT_TRUE(options.vlm_prompt_kv_cache.near_embedding_enabled);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_same_session_min_cosine, 0.998f);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_cross_session_min_cosine, 0.9995f);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_same_session_min_mean_token_cosine, 0.991f);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_cross_session_min_mean_token_cosine, 0.996f);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_same_session_min_p05_token_cosine, 0.96f);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_cross_session_min_p05_token_cosine, 0.985f);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_same_session_max_relative_l2, 0.14f);
    EXPECT_FLOAT_EQ(options.vlm_prompt_kv_cache.near_cross_session_max_relative_l2, 0.08f);
}

TEST(ConfigOptionParserTest, RejectsTieredPolicyWithoutNearPromptKv) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--vlm-cache-enabled",
            "--vlm-vector-cache-enabled",
            "--vlm-prompt-kv-cache-enabled",
            "--vlm-prompt-kv-backend", "memory",
            "--vlm-cache-reuse-policy", "tiered"
        }),
        std::runtime_error);
}

TEST(ConfigOptionParserTest, RejectsNearPromptKvWhenCrossSessionThresholdIsLooser) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--vlm-cache-enabled",
            "--vlm-vector-cache-enabled",
            "--vlm-prompt-kv-cache-enabled",
            "--vlm-prompt-kv-backend", "memory",
            "--vlm-prompt-kv-near-enabled",
            "--vlm-prompt-kv-near-same-session-min-cosine", "0.999",
            "--vlm-prompt-kv-near-cross-session-min-cosine", "0.998",
            "--vlm-cache-reuse-policy", "tiered"
        }),
        std::runtime_error);
}

TEST(ConfigOptionParserTest, RejectsNearPromptKvWhenCrossSessionTokenGateIsLooser) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--vlm-cache-enabled",
            "--vlm-vector-cache-enabled",
            "--vlm-prompt-kv-cache-enabled",
            "--vlm-prompt-kv-backend", "memory",
            "--vlm-prompt-kv-near-enabled",
            "--vlm-prompt-kv-near-same-session-min-token-p05", "0.98",
            "--vlm-prompt-kv-near-cross-session-min-token-p05", "0.95",
            "--vlm-cache-reuse-policy", "tiered"
        }),
        std::runtime_error);
}

TEST(ConfigOptionParserTest, RejectsUnknownVlmCacheReusePolicy) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--vlm-cache-reuse-policy", "unknown"
        }),
        std::runtime_error);
}

TEST(ConfigOptionParserTest, ResolvesAuthTokenFromFile) {
    ScopedTempDirectory temp("agent_auth_test");
    const auto token_path = WriteFile(temp.path() / "token.txt", "\xEF\xBB\xBF  file-token  \n");

    ArgvBuilder argv(std::vector<std::string>{
        "server",
        "--llm", "llm.gguf",
        "--auth-token-file",
        token_path.string()
    });

    auto options = ParseMultimodalOptions(argv.argc(), argv.argv());

    EXPECT_EQ(options.auth.token, "file-token");
    EXPECT_EQ(options.auth_source, "file");
}

TEST(ConfigOptionParserTest, RejectsMissingRequiredLlm) {
    EXPECT_THROW(
        Parse({
            "server",
            "--bert", "bert.onnx",
            "--llm-base-url", "https://api.example.com/v1"
        }),
        std::runtime_error);
}

TEST(ConfigOptionParserTest, RejectsInvalidSectionValidation) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--max-context-size", "64"
        }),
        std::runtime_error);

    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--vram-warning-free-mb", "128",
            "--vram-unload-free-mb", "256"
        }),
        std::runtime_error);

    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--vlm-cache-persist",
            "--vlm-cache-dir", ""
        }),
        std::runtime_error);
}

// LLM 配置解析与凭据优先级。

namespace {

void UnsetLlmEnv() {
#ifdef _WIN32
    _putenv_s("AGENT_LLM_API_KEY", "");
    _putenv_s("TEST_LLM_KEY", "");
#else
    unsetenv("AGENT_LLM_API_KEY");
    unsetenv("TEST_LLM_KEY");
#endif
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}

TEST(ConfigLlmSectionTest, DisabledWhenBaseUrlEmpty) {
    UnsetLlmEnv();
    auto opts = Parse({"server", "--llm", "llm.gguf"});
    EXPECT_TRUE(opts.llm.base_url.empty());
    EXPECT_TRUE(opts.llm.api_key.empty());
    EXPECT_FALSE(opts.llm.disable_tls_verify_on_windows);
}

TEST(ConfigLlmSectionTest, ApiKeyResolvedFromEnv) {
    UnsetLlmEnv();
    SetEnv("AGENT_LLM_API_KEY", "sk-from-env");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--llm-base-url", "https://api.example.com/v1",
    });
    EXPECT_EQ(opts.llm.base_url, "https://api.example.com/v1");
    EXPECT_EQ(opts.llm.api_key, "sk-from-env");

    UnsetLlmEnv();
}

TEST(ConfigLlmSectionTest, CustomEnvVarOverridesDefault) {
    UnsetLlmEnv();
    SetEnv("TEST_LLM_KEY", "sk-custom-env");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--llm-base-url", "https://api.example.com/v1",
        "--llm-api-key-env", "TEST_LLM_KEY",
    });
    EXPECT_EQ(opts.llm.api_key, "sk-custom-env");

    UnsetLlmEnv();
}

TEST(ConfigLlmSectionTest, ApiKeyResolvedFromFileWhenEnvMissing) {
    UnsetLlmEnv();
    ScopedTempDirectory tmp("llm_key");
    auto key_file = tmp.path() / "key.txt";
    WriteFile(key_file, "\xEF\xBB\xBF  sk-from-file\n");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--llm-base-url", "https://api.example.com/v1",
        "--llm-api-key-file", key_file.string(),
    });
    EXPECT_EQ(opts.llm.api_key, "sk-from-file");
}

TEST(ConfigLlmSectionTest, EnvTakesPrecedenceOverFile) {
    UnsetLlmEnv();
    SetEnv("AGENT_LLM_API_KEY", "sk-env-wins");

    ScopedTempDirectory tmp("llm_key");
    auto key_file = tmp.path() / "key.txt";
    WriteFile(key_file, "sk-from-file");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--llm-base-url", "https://api.example.com/v1",
        "--llm-api-key-file", key_file.string(),
    });
    EXPECT_EQ(opts.llm.api_key, "sk-env-wins");

    UnsetLlmEnv();
}

TEST(ConfigLlmSectionTest, ThrowsWhenBaseUrlSetButNoKey) {
    UnsetLlmEnv();
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--llm-base-url", "https://api.example.com/v1",
        }),
        std::runtime_error);
}

TEST(ConfigLlmSectionTest, ThrowsWhenKeyFileMissing) {
    UnsetLlmEnv();
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--llm-base-url", "https://api.example.com/v1",
            "--llm-api-key-file", "/nonexistent/path/key.txt",
        }),
        std::runtime_error);
}

TEST(ConfigLlmSectionTest, JsonConfigLoadsLlmSection) {
    UnsetLlmEnv();
    SetEnv("AGENT_LLM_API_KEY", "sk-json-test");

    ScopedTempDirectory tmp("llm_cfg");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "llm": {
            "base_url": "https://api.deepseek.com/v1",
            "model": "deepseek-chat",
            "timeout_ms": 45000,
            "max_retries": 5,
            "async_http_io_threads": 6,
            "http_keep_alive": true,
            "http_max_idle_connections": 96,
            "http_max_idle_connections_per_origin": 48,
            "http_idle_timeout_ms": 45000,
            "disable_tls_verify_on_windows": false,
            "ca_bundle_path": "certs/mozilla-ca-bundle.pem",
            "prompts": {
                "memory_extraction": "prompts/memory.txt",
                "fact_distillation": "prompts/fact.txt"
            }
        }
    })");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--config", config_file.string(),
    });

    EXPECT_EQ(opts.llm.base_url, "https://api.deepseek.com/v1");
    EXPECT_EQ(opts.llm.model, "deepseek-chat");
    EXPECT_EQ(opts.llm.timeout_ms, 45000);
    EXPECT_EQ(opts.llm.max_retries, 5);
    EXPECT_EQ(opts.llm.async_http_io_threads, 6u);
    EXPECT_TRUE(opts.llm.http_keep_alive);
    EXPECT_EQ(opts.llm.http_max_idle_connections, 96u);
    EXPECT_EQ(opts.llm.http_max_idle_connections_per_origin, 48u);
    EXPECT_EQ(opts.llm.http_idle_timeout_ms, 45000);
    EXPECT_EQ(opts.llm.api_key, "sk-json-test");
    EXPECT_FALSE(opts.llm.disable_tls_verify_on_windows);
    EXPECT_EQ(opts.llm.ca_bundle_path, "certs/mozilla-ca-bundle.pem");
    EXPECT_EQ(opts.llm.prompts.size(), 2u);
    EXPECT_EQ(opts.llm.prompts["memory_extraction"].string(), "prompts/memory.txt");

    EXPECT_TRUE(opts.config_file_path.is_absolute());

    UnsetLlmEnv();
}

TEST(ConfigSkillSectionTest, JsonConfigLoadsSkillManifests) {
    ScopedTempDirectory tmp("skill_cfg");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "skills": {"manifests": [{
            "skill_id": "vision.observe",
            "version": "1.0.0",
            "description": "observe",
            "input_schema": {"type": "object"},
            "output_schema": {"type": "object"},
            "executor": {"type": "native", "reference": "vision.observe"}
        }]}
    })");
    auto opts = Parse({"server", "--llm", "llm.gguf", "--config", config_file.string()});
    ASSERT_EQ(opts.skill_manifests.size(), 1u);
    EXPECT_EQ(opts.skill_manifests[0].skill_id, "vision.observe");
    EXPECT_EQ(opts.skill_manifests[0].executor.reference, "vision.observe");
}

TEST(ConfigSkillSectionTest, LoadsMatchingManifestFilesFromConfiguredDirectory) {
    ScopedTempDirectory tmp("skill_manifest_dir");
    const auto manifest_dir = tmp.path() / "skills";
    std::filesystem::create_directories(manifest_dir);
    WriteFile(manifest_dir / "vision_skill.json", R"({
        "skill_id": "vision.observe",
        "version": "2.0.0",
        "tool_name": "vision_observe",
        "description": "observe",
        "input_schema": {"type": "object"},
        "output_schema": {"type": "object"},
        "executor": {"type": "native", "reference": "vision.observe"}
    })");
    WriteFile(manifest_dir / "ignored.json", R"({
        "skill_id": "ignored.skill",
        "version": "1.0.0",
        "executor": {"type": "native", "reference": "ignored.skill"}
    })");
    const auto config_file = WriteFile(tmp.path() / "config.json", R"({
        "skills": {
            "enabled": true,
            "manifest_directory": "skills",
            "manifest_filename_regex": ".*_skill\\.json$"
        }
    })");
    auto options = Parse({"server", "--llm", "llm.gguf", "--config", config_file.string()});
    ASSERT_EQ(options.skill_manifests.size(), 1u);
    EXPECT_EQ(options.skill_manifests[0].skill_id, "vision.observe");
    EXPECT_EQ(options.skill_manifests[0].version, "2.0.0");
    EXPECT_EQ(options.skills.manifest_directory, manifest_dir);
}

TEST(ConfigLlmSectionTest, RejectsPerOriginIdleLimitAboveGlobalLimit) {
    UnsetLlmEnv();
    ScopedTempDirectory tmp("llm_bad_keep_alive_pool");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "llm": {
            "base_url": "http://127.0.0.1:18081/v1",
            "model": "mock-chat",
            "require_api_key": false,
            "http_keep_alive": true,
            "http_max_idle_connections": 16,
            "http_max_idle_connections_per_origin": 17
        }
    })");

    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--config", config_file.string(),
        }),
        std::runtime_error);
}

TEST(ConfigLlmSectionTest, CliOverridesJson) {
    UnsetLlmEnv();
    SetEnv("AGENT_LLM_API_KEY", "sk-test");

    ScopedTempDirectory tmp("llm_cfg");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "llm": {
            "base_url": "https://json.example.com/v1",
            "model": "json-model"
        }
    })");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--config", config_file.string(),
        "--llm-model", "cli-model",
        "--llm-timeout", "12345",
    });

    EXPECT_EQ(opts.llm.base_url, "https://json.example.com/v1");
    EXPECT_EQ(opts.llm.model, "cli-model");
    EXPECT_EQ(opts.llm.timeout_ms, 12345);

    UnsetLlmEnv();
}

TEST(ConfigGatewayAuthSectionTest, JsonLoadsGatewayAuthAndResolvesRelativeFiles) {
    ScopedTempDirectory tmp("gateway_auth_cfg");
    auto key_file = tmp.path() / "jwt_public.pem";
    auto private_key_file = tmp.path() / "jwt_private.pem";
    WriteFile(key_file, "-----BEGIN PUBLIC KEY-----\nTEST\n-----END PUBLIC KEY-----\n");
    WriteFile(private_key_file, "-----BEGIN PRIVATE KEY-----\nTEST\n-----END PRIVATE KEY-----\n");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "gateway_auth": {
            "enabled": true,
            "allow_dev_identity": false,
            "require_auth_for_api": true,
            "cookie_name": "agent_auth",
            "public_key_file": "jwt_public.pem",
            "private_key_file": "jwt_private.pem",
            "issuer": "agent-e2e",
            "audience": "agent-gateway",
            "clock_skew_seconds": 120,
            "token_ttl_seconds": 7200,
            "cookie_secure": true,
            "cookie_same_site": "Strict",
            "require_session_record": true,
            "auto_provision_session": false,
            "enable_dev_registration": true,
            "session_store_backend": "redis",
            "session_database_path": "gateway_auth.db",
            "redis_host": "127.0.0.1",
            "redis_port": "5000",
            "redis_pool_size": 16,
            "redis_command_timeout_ms": 2500,
            "redis_key_prefix": "agent:test:auth"
        }
    })");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--config", config_file.string(),
    });

    EXPECT_TRUE(opts.gateway_auth.enabled);
    EXPECT_FALSE(opts.gateway_auth.allow_dev_identity);
    EXPECT_TRUE(opts.gateway_auth.require_auth_for_api);
    EXPECT_EQ(opts.gateway_auth.cookie_name, "agent_auth");
    EXPECT_NE(opts.gateway_auth.public_key_pem.find("BEGIN PUBLIC KEY"), std::string::npos);
    EXPECT_NE(opts.gateway_auth.private_key_pem.find("BEGIN PRIVATE KEY"), std::string::npos);
    EXPECT_EQ(opts.gateway_auth.issuer, "agent-e2e");
    EXPECT_EQ(opts.gateway_auth.audience, "agent-gateway");
    EXPECT_EQ(opts.gateway_auth.clock_skew_seconds, 120);
    EXPECT_EQ(opts.gateway_auth.token_ttl_seconds, 7200);
    EXPECT_TRUE(opts.gateway_auth.cookie_secure);
    EXPECT_EQ(opts.gateway_auth.cookie_same_site, "Strict");
    EXPECT_TRUE(opts.gateway_auth.require_session_record);
    EXPECT_FALSE(opts.gateway_auth.auto_provision_session);
    EXPECT_TRUE(opts.gateway_auth.enable_dev_registration);
    EXPECT_EQ(opts.gateway_auth.session_store_backend, "redis");
    EXPECT_EQ(opts.gateway_auth.session_database_path, (tmp.path() / "gateway_auth.db").string());
    EXPECT_EQ(opts.gateway_auth.redis_host, "127.0.0.1");
    EXPECT_EQ(opts.gateway_auth.redis_port, "5000");
    EXPECT_EQ(opts.gateway_auth.redis_pool_size, 16);
    EXPECT_EQ(opts.gateway_auth.redis_command_timeout_ms, 2500);
    EXPECT_EQ(opts.gateway_auth.redis_key_prefix, "agent:test:auth");
}

TEST(ConfigGatewayAuthSectionTest, CliOverridesGatewayAuthJson) {
    ScopedTempDirectory tmp("gateway_auth_cli");
    auto key_file = tmp.path() / "jwt_public.pem";
    auto private_key_file = tmp.path() / "jwt_private.pem";
    WriteFile(key_file, "public-key");
    WriteFile(private_key_file, "private-key");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "gateway_auth": {
            "enabled": false,
            "cookie_name": "json_cookie"
        }
    })");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--config", config_file.string(),
        "--gateway-auth-required",
        "--gateway-auth-cookie", "cli_cookie",
        "--gateway-auth-public-key-file", key_file.string(),
        "--gateway-auth-private-key-file", private_key_file.string(),
        "--gateway-auth-issuer", "cli-issuer",
        "--gateway-auth-audience", "cli-aud",
        "--gateway-auth-token-ttl", "600",
        "--gateway-auth-cookie-secure",
        "--gateway-auth-session-store", "redis",
        "--gateway-auth-redis-host", "127.0.0.1",
        "--gateway-auth-redis-port", "5000",
        "--gateway-auth-session-db", (tmp.path() / "auth.db").string(),
        "--gateway-auth-require-session",
        "--gateway-auth-enable-dev-registration"
    });

    EXPECT_TRUE(opts.gateway_auth.enabled);
    EXPECT_TRUE(opts.gateway_auth.require_auth_for_api);
    EXPECT_FALSE(opts.gateway_auth.allow_dev_identity);
    EXPECT_EQ(opts.gateway_auth.cookie_name, "cli_cookie");
    EXPECT_EQ(opts.gateway_auth.public_key_pem, "public-key");
    EXPECT_EQ(opts.gateway_auth.private_key_pem, "private-key");
    EXPECT_EQ(opts.gateway_auth.issuer, "cli-issuer");
    EXPECT_EQ(opts.gateway_auth.audience, "cli-aud");
    EXPECT_EQ(opts.gateway_auth.token_ttl_seconds, 600);
    EXPECT_TRUE(opts.gateway_auth.cookie_secure);
    EXPECT_TRUE(opts.gateway_auth.require_session_record);
    EXPECT_FALSE(opts.gateway_auth.auto_provision_session);
    EXPECT_TRUE(opts.gateway_auth.enable_dev_registration);
    EXPECT_EQ(opts.gateway_auth.session_store_backend, "redis");
    EXPECT_EQ(opts.gateway_auth.redis_host, "127.0.0.1");
    EXPECT_EQ(opts.gateway_auth.redis_port, "5000");
}

TEST(ConfigGatewayAuthSectionTest, RejectsStrictSessionWithoutDatabase) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--gateway-auth-enabled",
            "--gateway-auth-require-session",
        }),
        std::runtime_error);
}

TEST(ConfigPersonaGatewaySectionTest, JsonLoadsE2EGatewayOptionsAndResolvesStaticRoot) {
    ScopedTempDirectory tmp("persona_gateway_cfg");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "persona_gateway": {
            "websocket_path": "/ws/session",
            "static_files": {
                "enabled": true,
                "root": "dist",
                "index_file": "index.html",
                "spa_fallback": true
            },
            "document_store": {
                "enabled": true,
                "root": "document",
                "database_path": "document/document_store.sqlite",
                "read_connection_count": 3,
                "write_connection_count": 1,
                "busy_timeout_ms": 1500,
                "retention_hours": 72,
                "cleanup_interval_seconds": 30,
                "enable_path_register_test_endpoint": true,
                "enable_path_analyze_test_endpoint": true
            },
            "compute_pool": {
                "worker_count": 4,
                "queue_capacity": 256,
                "scheduler": "session_affinity",
                "max_active_keys": 111,
                "max_outstanding_per_key": 7,
                "max_outstanding_per_fairness_key": 29,
                "max_outstanding_per_tenant": 101
            },
            "io_pool": {
                "worker_count": 2,
                "queue_capacity": 128
            },
            "llm_pool": {
                "worker_count": 12,
                "queue_capacity": 512,
                "scheduler": "session_affinity",
                "max_active_keys": 222,
                "max_outstanding_per_key": 5,
                "max_outstanding_per_fairness_key": 47,
                "max_outstanding_per_tenant": 303
            },
            "session_idle_timeout_minutes": 30,
            "session_max_recent_turns": 24,
            "session_max_active_sessions": 37,
            "runtime_recent_raw_turns": 10,
            "runtime_default_model": "e2e-model",
            "personas": {
                "dazhi": {
                    "description": "default teacher persona",
                    "traits": ["patient", "structured"],
                    "humorTendency": 0.2,
                    "emotionPrompts": {
                        "emotionMap": {
                            "neutral": "保持稳定教学节奏"
                        },
                        "emotionReliability": {
                            "neutral": 1.0
                        }
                    }
                },
                "xiaozhi": {
                    "description": "default assistant persona",
                    "empathyLevel": 0.9,
                    "emotionState": {
                        "noiseSigma": 0.0,
                        "persistToL4": false
                    }
                }
            },
            "request_filter": {
                "enabled": true,
                "reject_control_chars": true,
                "reject_suspicious_patterns": true
            }
        }
    })");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--config", config_file.string(),
    });

    EXPECT_EQ(opts.persona_gateway.websocket_path, "/ws/session");
    EXPECT_TRUE(opts.persona_gateway.static_files.enabled);
    EXPECT_EQ(opts.persona_gateway.static_files.root, tmp.path() / "dist");
    EXPECT_EQ(opts.persona_gateway.static_files.index_file, "index.html");
    EXPECT_TRUE(opts.persona_gateway.static_files.spa_fallback);
    EXPECT_TRUE(opts.persona_gateway.document_store.enabled);
    EXPECT_EQ(opts.persona_gateway.document_store.root, tmp.path() / "document");
    EXPECT_EQ(opts.persona_gateway.document_store.database_path, tmp.path() / "document/document_store.sqlite");
    EXPECT_EQ(opts.persona_gateway.document_store.read_connection_count, 3u);
    EXPECT_EQ(opts.persona_gateway.document_store.write_connection_count, 1u);
    EXPECT_EQ(opts.persona_gateway.document_store.busy_timeout_ms, 1500);
    EXPECT_EQ(opts.persona_gateway.document_store.retention_hours, 72);
    EXPECT_EQ(opts.persona_gateway.document_store.cleanup_interval_seconds, 30);
    EXPECT_TRUE(opts.persona_gateway.document_store.enable_path_register_test_endpoint);
    EXPECT_TRUE(opts.persona_gateway.document_store.enable_path_analyze_test_endpoint);
    EXPECT_EQ(opts.persona_gateway.compute_pool.worker_count, 4u);
    EXPECT_EQ(opts.persona_gateway.compute_pool.queue_capacity, 256u);
    EXPECT_EQ(opts.persona_gateway.compute_pool.scheduler, "session_affinity");
    EXPECT_EQ(opts.persona_gateway.compute_pool.max_active_keys, 111u);
    EXPECT_EQ(opts.persona_gateway.compute_pool.max_outstanding_per_key, 7u);
    EXPECT_EQ(opts.persona_gateway.compute_pool.max_outstanding_per_fairness_key, 29u);
    EXPECT_EQ(opts.persona_gateway.compute_pool.max_outstanding_per_tenant, 101u);
    EXPECT_EQ(opts.persona_gateway.io_pool.worker_count, 2u);
    EXPECT_EQ(opts.persona_gateway.io_pool.queue_capacity, 128u);
    ASSERT_TRUE(opts.persona_gateway.llm_pool.has_value());
    EXPECT_EQ(opts.persona_gateway.llm_pool->worker_count, 12u);
    EXPECT_EQ(opts.persona_gateway.llm_pool->queue_capacity, 512u);
    EXPECT_EQ(opts.persona_gateway.llm_pool->scheduler, "session_affinity");
    EXPECT_EQ(opts.persona_gateway.llm_pool->max_active_keys, 222u);
    EXPECT_EQ(opts.persona_gateway.llm_pool->max_outstanding_per_key, 5u);
    EXPECT_EQ(opts.persona_gateway.llm_pool->max_outstanding_per_fairness_key, 47u);
    EXPECT_EQ(opts.persona_gateway.llm_pool->max_outstanding_per_tenant, 303u);
    EXPECT_EQ(opts.persona_gateway.session_idle_timeout_minutes, 30);
    EXPECT_EQ(opts.persona_gateway.session_max_recent_turns, 24u);
    EXPECT_EQ(opts.persona_gateway.session_max_active_sessions, 37u);
    EXPECT_EQ(opts.persona_gateway.runtime_recent_raw_turns, 10u);
    EXPECT_EQ(opts.persona_gateway.runtime_default_model, "e2e-model");
    ASSERT_EQ(opts.persona_gateway.personas.size(), 2u);
    EXPECT_EQ(opts.persona_gateway.personas[0].persona_id, "dazhi");
    EXPECT_EQ(opts.persona_gateway.personas[0].description, "default teacher persona");
    EXPECT_EQ(opts.persona_gateway.personas[0].traits.size(), 2u);
    ASSERT_TRUE(opts.persona_gateway.personas[0].emotion_prompts.has_value());
    EXPECT_EQ(opts.persona_gateway.personas[0].emotion_prompts->emotion_map.at("neutral"), "保持稳定教学节奏");
    EXPECT_EQ(opts.persona_gateway.personas[1].persona_id, "xiaozhi");
    EXPECT_EQ(opts.persona_gateway.personas[1].empathy_level, 0.9);
    EXPECT_EQ(opts.persona_gateway.personas[1].emotion_state.noise_sigma, 0.0);
    EXPECT_FALSE(opts.persona_gateway.personas[1].emotion_state.persist_to_l4);
    EXPECT_TRUE(opts.persona_gateway.request_filter_enabled);
}

TEST(ConfigPersonaGatewaySectionTest, RejectsMultipleSqliteDocumentWriters) {
    ScopedTempDirectory tmp("persona_gateway_sqlite_writers_cfg");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "persona_gateway": {
            "document_store": {
                "enabled": true,
                "root": "document",
                "database_path": "document/document_store.sqlite",
                "write_connection_count": 2
            }
        }
    })");

    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--config", config_file.string(),
        }),
        std::runtime_error);
}

TEST(ConfigPersonaGatewaySectionTest, CliOverridesGatewayJson) {
    ScopedTempDirectory tmp("persona_gateway_cli");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "persona_gateway": {
            "websocket_path": "/ws/json",
            "request_filter": { "enabled": true }
        }
    })");

    auto opts = Parse({
        "server",
        "--llm", "llm.gguf",
        "--config", config_file.string(),
        "--gateway-ws-path", "/ws/cli",
        "--gateway-static-root", (tmp.path() / "dist").string(),
        "--gateway-document-store-root", (tmp.path() / "document").string(),
        "--gateway-document-store-db", (tmp.path() / "document/document_store.sqlite").string(),
        "--gateway-document-store-retention-hours", "96",
        "--gateway-document-store-cleanup-seconds", "45",
        "--gateway-document-store-enable-path-register-test",
        "--gateway-document-store-enable-path-analyze-test",
        "--gateway-compute-workers", "3",
        "--gateway-compute-queue", "300",
        "--gateway-io-workers", "2",
        "--gateway-io-queue", "200",
        "--gateway-llm-workers", "9",
        "--gateway-llm-queue", "900",
        "--gateway-session-idle-minutes", "45",
        "--gateway-session-max-recent-turns", "32",
        "--gateway-session-max-active", "41",
        "--gateway-runtime-recent-raw-turns", "12",
        "--gateway-runtime-model", "cli-model",
        "--gateway-filter-disabled"
    });

    EXPECT_EQ(opts.persona_gateway.websocket_path, "/ws/cli");
    EXPECT_TRUE(opts.persona_gateway.static_files.enabled);
    EXPECT_EQ(opts.persona_gateway.static_files.root, tmp.path() / "dist");
    EXPECT_TRUE(opts.persona_gateway.document_store.enabled);
    EXPECT_EQ(opts.persona_gateway.document_store.root, tmp.path() / "document");
    EXPECT_EQ(opts.persona_gateway.document_store.database_path, tmp.path() / "document/document_store.sqlite");
    EXPECT_EQ(opts.persona_gateway.document_store.retention_hours, 96);
    EXPECT_EQ(opts.persona_gateway.document_store.cleanup_interval_seconds, 45);
    EXPECT_TRUE(opts.persona_gateway.document_store.enable_path_register_test_endpoint);
    EXPECT_TRUE(opts.persona_gateway.document_store.enable_path_analyze_test_endpoint);
    EXPECT_EQ(opts.persona_gateway.compute_pool.worker_count, 3u);
    EXPECT_EQ(opts.persona_gateway.compute_pool.queue_capacity, 300u);
    EXPECT_EQ(opts.persona_gateway.io_pool.worker_count, 2u);
    EXPECT_EQ(opts.persona_gateway.io_pool.queue_capacity, 200u);
    ASSERT_TRUE(opts.persona_gateway.llm_pool.has_value());
    EXPECT_EQ(opts.persona_gateway.llm_pool->worker_count, 9u);
    EXPECT_EQ(opts.persona_gateway.llm_pool->queue_capacity, 900u);
    EXPECT_EQ(opts.persona_gateway.llm_pool->scheduler, "session_affinity");
    EXPECT_EQ(opts.persona_gateway.session_idle_timeout_minutes, 45);
    EXPECT_EQ(opts.persona_gateway.session_max_recent_turns, 32u);
    EXPECT_EQ(opts.persona_gateway.session_max_active_sessions, 41u);
    EXPECT_EQ(opts.persona_gateway.runtime_recent_raw_turns, 12u);
    EXPECT_EQ(opts.persona_gateway.runtime_default_model, "cli-model");
    EXPECT_FALSE(opts.persona_gateway.request_filter_enabled);
}

TEST(ConfigPersonaGatewaySectionTest, RejectsInvalidWebSocketPath) {
    ScopedTempDirectory tmp("persona_gateway_bad");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "persona_gateway": {
            "websocket_path": "ws/no-leading-slash"
        }
    })");

    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--config", config_file.string(),
        }),
        std::runtime_error);
}

TEST(ConfigPersonaGatewaySectionTest, LoadsStreamingLimitsAndHeartbeatPolicy) {
    ScopedTempDirectory tmp("persona_gateway_stream");
    const auto config = WriteFile(tmp.path() / "config.json", R"({
        "llm":{"enabled":false},
        "persona_gateway":{"streaming":{
            "max_pending_events":32,"max_pending_bytes":8192,"max_event_bytes":2048,
            "max_replay_turns":16,"max_replay_events":64,"max_replay_bytes":4096,
            "heartbeat_interval_ms":50,"idle_timeout_ms":250,"max_duration_ms":2000,
            "write_timeout_ms":500,"reconnect_delay_ms":25,"replay_retention_ms":1000,"require_pong":true
        }}
    })");
    auto opts = Parse({"test", "--llm", "llm.gguf", "--config", config.string()});
    const auto& value = opts.persona_gateway.streaming;
    EXPECT_EQ(value.transport.outbound.max_items, 32);
    EXPECT_EQ(value.transport.outbound.max_bytes, 8192);
    EXPECT_EQ(value.transport.max_event_bytes, 2048);
    EXPECT_EQ(value.transport.heartbeat_interval.count(), 50);
    EXPECT_EQ(value.transport.idle_timeout.count(), 250);
    EXPECT_EQ(value.transport.max_duration.count(), 2000);
    EXPECT_EQ(value.transport.write_timeout.count(), 500);
    EXPECT_EQ(value.transport.reconnect_delay_ms, 25);
    EXPECT_TRUE(value.transport.require_pong);
    EXPECT_EQ(value.max_replay_turns, 16);
    EXPECT_EQ(value.max_replay_events, 64);
    EXPECT_EQ(value.max_replay_bytes, 4096);
    EXPECT_EQ(value.replay_retention_ms, 1000);
}

TEST(ConfigPersonaGatewaySectionTest, RejectsUnboundedStreamingAndInvalidIdlePolicy) {
    ScopedTempDirectory tmp("persona_gateway_bad_stream");
    for (const auto* field : {"max_pending_events", "max_pending_bytes", "max_event_bytes", "max_replay_turns",
                             "max_replay_events", "max_replay_bytes", "replay_retention_ms"}) {
        const auto config = WriteFile(tmp.path() / "config.json", std::string("{\"llm\":{\"enabled\":false},") +
            "\"persona_gateway\":{\"streaming\":{\"" + field + "\":0}}}");
        try {
            Parse({"test", "--llm", "llm.gguf", "--config", config.string()});
            FAIL() << "zero streaming limit was accepted: " << field;
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find(field), std::string::npos);
        }
    }
    const auto config = WriteFile(tmp.path() / "config.json", R"({"llm":{"enabled":false},
        "persona_gateway":{"streaming":{"heartbeat_interval_ms":100,"idle_timeout_ms":50}}})");
    try {
        Parse({"test", "--llm", "llm.gguf", "--config", config.string()});
        FAIL() << "invalid streaming idle policy was accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("idle_timeout_ms"), std::string::npos);
    }
}

TEST(ConfigPersonaGatewaySectionTest, RejectsUnknownThreadPoolScheduler) {
    ScopedTempDirectory tmp("persona_gateway_bad_scheduler");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "persona_gateway": {
            "compute_pool": {
                "scheduler": "unknown"
            }
        }
    })");

    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--config", config_file.string(),
        }),
        std::runtime_error);
}

TEST(ConfigPersonaGatewaySectionTest, RejectsFifoLlmPoolScheduler) {
    ScopedTempDirectory tmp("persona_gateway_bad_llm_scheduler");
    auto config_file = tmp.path() / "config.json";
    WriteFile(config_file, R"({
        "persona_gateway": {
            "llm_pool": {
                "scheduler": "default_fifo"
            }
        }
    })");

    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--config", config_file.string(),
        }),
        std::runtime_error);
}

TEST(ConfigPersonaGatewaySectionTest, RejectsZeroActiveSessionLimit) {
    EXPECT_THROW(
        Parse({
            "server",
            "--llm", "llm.gguf",
            "--gateway-session-max-active", "0",
        }),
        std::runtime_error);
}
