// Manual smoke test for the LLM client against a real OpenAI-compatible API.
//
// Reads ./tools/llm_smoke_test.json (gitignored) which should contain:
//   {
//     "llm": {
//       "base_url": "https://api.deepseek.com/v1",
//       "api_key_file": "tools/llm_smoke_test.key",
//       "model": "deepseek-chat",
//       "timeout_ms": 30000,
//       "max_retries": 1,
//       "prompts": { "smoke": "tools/llm_smoke_prompt.txt" }
//     }
//   }
//
// Not part of CTest — run manually:
//   .\build\x64-Release-Tests\Release\llm_smoke_test.exe tools\llm_smoke_test.json
//
// Validates: TLS handshake against a public CA, real API auth, real JSON
// response shape, and the full config → prompt → request pipeline.

#include "option_parser.h"
#include "server_options.h"
#include "openai_llm_client.h"
#include "beast_http_client.h"
#include "tls_context.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

int Fail(const std::string& msg) {
    std::cerr << "[smoke] FAIL: " << msg << std::endl;
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config.json> [user_prompt] [max_tokens]\n";
        std::cerr << "       (reads llm.* from config.json, sends one chat completion)\n";
        return 2;
    }

    const std::string config_path = argv[1];
    const std::string user_prompt = (argc >= 3) ? argv[2]
                                                : "你好，用一句话介绍你自己。";
    int max_tokens = 0;
    if (argc >= 4) {
        try {
            max_tokens = std::stoi(argv[3]);
        } catch (const std::exception&) {
            return Fail("max_tokens must be a non-negative integer");
        }
        if (max_tokens < 0) return Fail("max_tokens must be a non-negative integer");
    }

    MultimodalServerOptions options;
    try {
        std::vector<std::string> fake_args = {
            "smoke", "--llm", "smoke.gguf", "--config", config_path,
        };
        std::vector<char*> argv_storage;
        for (auto& s : fake_args) argv_storage.push_back(s.data());
        options = ParseMultimodalOptions(
            static_cast<int>(argv_storage.size()), argv_storage.data());
    } catch (const std::exception& e) {
        return Fail(std::string("config parse: ") + e.what());
    }

    if (options.llm.base_url.empty()) {
        return Fail("config has no llm.base_url");
    }
    if (options.llm.api_key.empty()) {
        return Fail("api_key was not resolved (check env var or api_key_file)");
    }

    std::cout << "[smoke] base_url: " << options.llm.base_url << "\n";
    std::cout << "[smoke] model:    " << options.llm.model << "\n";
    std::cout << "[smoke] api_key:  resolved (redacted)\n";
    std::cout << "[smoke] prompts:  " << options.llm.prompts.size() << " loaded\n";

    // TLS verification:
    //   Linux / macOS: default TlsClientOptions triggers
    //                  set_default_verify_paths() → system trust store works
    //                  out of the box, no config needed.
    //   Windows: vcpkg OpenSSL has no default trust store.  A configured
    //            Mozilla-compatible CA bundle is therefore required.
    agent::net::TlsClientOptions tls_opts;
#ifdef _WIN32
    if (!options.llm.ca_bundle_path.empty()) {
        std::filesystem::path bundle = options.llm.ca_bundle_path;
        if (bundle.is_relative() && !options.config_file_path.empty()) {
            bundle = options.config_file_path.parent_path() / bundle;
        }
        tls_opts.ca_bundle_path = bundle.string();
        std::cout << "[smoke] CA bundle: " << tls_opts.ca_bundle_path << "\n";
    } else {
        return Fail(
            "llm.ca_bundle_path is required on Windows; run "
            "tools\\update_mozilla_ca_bundle.ps1 first");
    }
#else
    std::cout << "[smoke] TLS verify: system trust store\n";
#endif
    auto tls_r = agent::net::TlsContext::CreateClient(tls_opts);
    if (!tls_r) {
        return Fail("TLS context: " + tls_r.status().message());
    }
    agent::net::BeastHttpClientOptions http_opts;
    http_opts.tls_context = std::move(tls_r).value();
    auto http_r = agent::net::BeastHttpClient::Create(std::move(http_opts));
    if (!http_r) {
        return Fail("HTTP client: " + http_r.status().message());
    }
    auto http_client = std::move(http_r).value();
    
    agent::llm::LlmPromptStore prompt_store;
    auto ps = prompt_store.Load(options.llm.prompts,
                                 options.config_file_path.parent_path());
    if (!ps.ok()) {
        return Fail("prompt store: " + ps.message());
    }

    agent::llm::OpenAiLlmClientOptions llm_opts;
    llm_opts.base_url = options.llm.base_url;
    llm_opts.api_key = options.llm.api_key;
    llm_opts.default_model = options.llm.model;
    llm_opts.timeout_ms = options.llm.timeout_ms;
    llm_opts.retry_policy.max_retries = options.llm.max_retries;

    auto client_r = agent::llm::OpenAiLlmClient::Create(std::move(llm_opts), *http_client);
    if (!client_r) {
        return Fail("LLM client: " + client_r.status().message());
    }
    auto client = std::move(client_r).value();

    agent::llm::ChatCompletionRequest req;
    if (prompt_store.Has("smoke")) {
        req.messages.push_back({agent::llm::ChatRole::System,
                                 prompt_store.Get("smoke").value()});
    }
    req.messages.push_back({agent::llm::ChatRole::User, user_prompt});
    req.max_tokens = max_tokens;

    std::cout << "[smoke] sending request..." << std::endl;
    auto start = std::chrono::steady_clock::now();
    auto resp = client->Complete(req);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (!resp.ok()) {
        return Fail("Complete: " + resp.status().message());
    }

    const auto& r = resp.value();
    std::cout << "[smoke] OK in " << elapsed.count() << " ms\n";
    std::cout << "[smoke] id:     " << r.id << "\n";
    std::cout << "[smoke] model:  " << r.model << "\n";
    std::cout << "[smoke] tokens: prompt=" << r.prompt_tokens
              << " completion=" << r.completion_tokens
              << " total=" << r.total_tokens << "\n";
    std::cout << "[smoke] finish_reason: " << r.finish_reason << "\n";
    std::cout << "[smoke] reply_bytes: " << r.content.size() << "\n";
    std::cout << "[smoke] reasoning_bytes: "
              << (r.reasoning_content ? r.reasoning_content->size() : 0) << "\n";
    return 0;
}
