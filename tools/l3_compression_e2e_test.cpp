// E2E test for L3 long-term memory compression.
//
// Reads ./tools/l3_compression_e2e_test.json (gitignored) which should contain:
//   {
//     "llm": {
//       "base_url": "https://api.deepseek.com/v1",
//       "api_key_file": "llm_smoke_test.key",
//       "model": "deepseek-chat",
//       "timeout_ms": 30000,
//       "max_retries": 1
//     },
//     "redis": {
//       "host": "127.0.0.1",
//       "port": 5000
//     },
//     "embedding": {
//       "tokenizer_path": "onnx_models/minilm/tokenizer.json",
//       "model_path": "onnx_models/minilm/model.onnx",
//       "dimension": 384,
//       "execution_provider": "auto"
//     }
//   }
//
// Not part of CTest — run manually:
//   .\build\x64-Release-Tests\Release\l3_compression_e2e_test.exe tools\l3_compression_e2e_test.json
//
// Validates: Redis L0 batch fetch → LLM compression → Embedding → SQLite vector storage → retrieval

#include "long_term_memory_compressor.h"
#include "openai_llm_client.h"
#include "beast_http_client.h"
#include "tls_context.h"
#include "redis_connection_pool.h"
#include "semantic_cache_pipeline.h"
#include "sqlite_vector_repository.h"
#include "vector_partition_registry.h"
#include "sqlite_connection_pool.h"
#include "vector_index_manager.h"
#include "embedding_pipeline.h"
#include "hf_tokenizer.h"
#include "onnx_text_embedding_model.h"
#include "crash_dump.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

int Fail(const std::string& msg) {
    std::cerr << "[e2e] FAIL: " << msg << std::endl;
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    crash_dump::Install(".");

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config.json>\n";
        std::cerr << "       (reads llm.*, redis.*, and embedding.* from config.json)\n";
        return 2;
    }

    const std::string config_path = argv[1];

    // Parse config JSON
    std::string llm_base_url, llm_api_key, llm_model;
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string tokenizer_path, model_path, execution_provider = "auto";
    int embedding_dimension = 384;

    try {
        std::ifstream f(config_path);
        if (!f.is_open()) {
            return Fail("Cannot open config file: " + config_path);
        }
        json config = json::parse(f);

        // LLM config
        if (!config.contains("llm")) {
            return Fail("config missing 'llm' section");
        }
        llm_base_url = config["llm"]["base_url"].get<std::string>();
        llm_model = config["llm"]["model"].get<std::string>();

        std::string api_key_file = config["llm"]["api_key_file"].get<std::string>();
        fs::path key_path = api_key_file;
        if (key_path.is_relative()) {
            key_path = fs::path(config_path).parent_path() / key_path;
        }
        std::ifstream key_f(key_path);
        if (!key_f.is_open()) {
            return Fail("Cannot open API key file: " + key_path.string());
        }
        std::getline(key_f, llm_api_key);
        if (llm_api_key.empty()) {
            return Fail("API key file is empty");
        }

        // Redis config
        if (config.contains("redis")) {
            if (config["redis"].contains("host")) {
                redis_host = config["redis"]["host"].get<std::string>();
            }
            if (config["redis"].contains("port")) {
                redis_port = config["redis"]["port"].get<int>();
            }
        }

        // Embedding config
        if (!config.contains("embedding")) {
            return Fail("config missing 'embedding' section");
        }
        tokenizer_path = config["embedding"]["tokenizer_path"].get<std::string>();
        model_path = config["embedding"]["model_path"].get<std::string>();
        embedding_dimension = config["embedding"]["dimension"].get<int>();
        if (config["embedding"].contains("execution_provider")) {
            execution_provider = config["embedding"]["execution_provider"].get<std::string>();
        }

    } catch (const std::exception& e) {
        return Fail(std::string("config parse: ") + e.what());
    }

    std::cout << "[e2e] LLM base_url: " << llm_base_url << "\n";
    std::cout << "[e2e] LLM model:    " << llm_model << "\n";
    std::cout << "[e2e] API key:      " << llm_api_key.substr(0, 8) << "...\n";
    std::cout << "[e2e] Redis:        " << redis_host << ":" << redis_port << "\n";
    std::cout << "[e2e] Tokenizer:    " << tokenizer_path << "\n";
    std::cout << "[e2e] Model:        " << model_path << "\n";
    std::cout << "[e2e] Dimension:    " << embedding_dimension << "\n";
    std::cout << "[e2e] Provider:     " << execution_provider << "\n\n";

    // Setup TLS context
    agent::net::TlsClientOptions tls_opts;
#ifdef _WIN32
    tls_opts.verify_mode = agent::net::TlsVerifyMode::None;
    std::cout << "[e2e] TLS verify: disabled (Windows)\n";
#else
    std::cout << "[e2e] TLS verify: system trust store\n";
#endif

    auto tls_r = agent::net::TlsContext::CreateClient(tls_opts);
    if (!tls_r) {
        return Fail("TLS context: " + tls_r.status().message());
    }

    // Setup HTTP client
    agent::net::BeastHttpClientOptions http_opts;
    http_opts.tls_context = std::move(tls_r).value();
    auto http_r = agent::net::BeastHttpClient::Create(std::move(http_opts));
    if (!http_r) {
        return Fail("HTTP client: " + http_r.status().message());
    }
    auto http_client = std::move(http_r).value();

    // Setup LLM client
    agent::llm::OpenAiLlmClientOptions llm_opts;
    llm_opts.base_url = llm_base_url;
    llm_opts.api_key = llm_api_key;
    llm_opts.default_model = llm_model;
    llm_opts.timeout_ms = 30000;
    llm_opts.retry_policy.max_retries = 1;

    auto client_r = agent::llm::OpenAiLlmClient::Create(std::move(llm_opts), *http_client);
    if (!client_r) {
        return Fail("LLM client: " + client_r.status().message());
    }
    auto llm_client = std::move(client_r).value();
    std::cout << "[e2e] LLM client created\n";

    // Setup Redis pool
    agent::semantic_cache::RedisPoolOptions redis_opts;
    redis_opts.host = redis_host;
    redis_opts.port = std::to_string(redis_port);
    redis_opts.pool_size = 4;
    auto redis_pool = std::make_shared<agent::semantic_cache::RedisConnectionPool>(redis_opts);
    auto redis_start = redis_pool->Start();
    if (!redis_start.ok()) {
        return Fail("Redis pool start: " + redis_start.message());
    }
    std::cout << "[e2e] Redis pool started\n";

    // Setup SQLite connection pool
    const std::string test_db_path = "test_l3_e2e.db";
    std::error_code ec;
    fs::remove(test_db_path, ec);
    fs::remove(test_db_path + "-wal", ec);
    fs::remove(test_db_path + "-shm", ec);

    storage::sqlite::SqliteConnectionPoolOptions pool_opts;
    pool_opts.path = test_db_path;
    pool_opts.read_connection_count = 4;
    pool_opts.enable_wal = true;
    auto sqlite_pool = std::make_shared<storage::sqlite::SqliteConnectionPool>(pool_opts);
    auto pool_start = sqlite_pool->Start();
    if (!pool_start.ok()) {
        return Fail("SQLite pool start: " + pool_start.message());
    }
    std::cout << "[e2e] SQLite pool started\n";

    // Setup vector repository
    auto vector_repo = std::make_shared<agent::vector_storage::SqliteVectorRepository>(sqlite_pool);
    auto schema_status = vector_repo->EnsureSchema();
    if (!schema_status.ok()) {
        return Fail("Vector repo schema: " + schema_status.message());
    }
    std::cout << "[e2e] Vector repository schema created\n";

    // Create collection
    agent::vector_storage::CollectionDescriptor coll;
    coll.name = "l3_memory_e2e";
    coll.embedding_model_fingerprint = "minilm-l6-v2";
    coll.tokenizer_fingerprint = "minilm-l6-v2";
    coll.pooling_strategy = "mean";
    coll.normalization = "l2";
    coll.dimension = static_cast<size_t>(embedding_dimension);
    coll.corpus_version = "v1";
    coll.policy_version = "v1";
    auto coll_r = vector_repo->EnsureCollection(coll);
    if (!coll_r.ok()) {
        return Fail("EnsureCollection: " + coll_r.status().message());
    }
    int64_t collection_id = coll_r.value();
    std::cout << "[e2e] Collection created, id=" << collection_id << "\n";

    // Setup partition registry
    auto partition_registry = std::make_shared<agent::vector_storage::PartitionRegistry>(vector_repo);
    std::cout << "[e2e] Partition registry created\n";

    // Load tokenizer
    auto tokenizer_r = vector::HfTokenizer::LoadFromFile(tokenizer_path);
    if (!tokenizer_r.ok()) {
        return Fail("Load tokenizer: " + tokenizer_r.status().message());
    }
    auto tokenizer = std::make_shared<vector::HfTokenizer>(std::move(tokenizer_r).value());
    std::cout << "[e2e] Tokenizer loaded\n";

    // Load embedding model
    vector::EmbeddingModelOptions model_opts;
    model_opts.model_path = model_path;
    model_opts.pooling = vector::PoolingStrategy::Mean;
    model_opts.normalize = true;
    model_opts.execution_provider = execution_provider;
    model_opts.allow_cpu_fallback = true;
    model_opts.expected_dimension = static_cast<size_t>(embedding_dimension);

    auto model_r = vector::OnnxTextEmbeddingModel::Load(model_opts);
    if (!model_r.ok()) {
        return Fail("Load embedding model: " + model_r.status().message());
    }
    auto embedding_model = std::shared_ptr<vector::IEmbeddingModel>(std::move(model_r).value());
    std::cout << "[e2e] Embedding model loaded, provider="
              << static_cast<vector::OnnxTextEmbeddingModel*>(embedding_model.get())->GetActiveExecutionProvider() << "\n";

    // Create embedding pipeline
    auto embedding_pipeline = std::make_shared<vector::EmbeddingPipeline>(
        tokenizer, embedding_model, vector::EmbeddingPipelineOptions{});
    std::cout << "[e2e] Embedding pipeline created\n";

    // Create vector index manager
    agent::vector::IndexManagerOptions index_opts;
    index_opts.max_resident_partitions = 8;
    index_opts.backend = "exact";
    index_opts.log_hydration = true;
    auto index_manager = std::make_shared<agent::vector::VectorIndexManager>(
        vector_repo, partition_registry, static_cast<size_t>(embedding_dimension), index_opts);
    std::cout << "[e2e] Vector index manager created\n";

    // Create L3 compressor
    agent::memory::LongTermMemoryCompressorOptions compressor_opts;
    compressor_opts.redis_pool = redis_pool;
    compressor_opts.llm_client = std::move(llm_client);
    compressor_opts.vector_repo = vector_repo;
    compressor_opts.partition_registry = partition_registry;
    compressor_opts.index_manager = index_manager;
    compressor_opts.embedding_pipeline = embedding_pipeline;
    compressor_opts.collection_id = collection_id;
    compressor_opts.mode = agent::memory::CompressorMode::Production;
    compressor_opts.max_records_per_batch = 50;
    compressor_opts.compression_temperature = 0.1f;
    compressor_opts.compression_max_tokens = 800;

    auto compressor_r = agent::memory::LongTermMemoryCompressor::Create(std::move(compressor_opts));
    if (!compressor_r.ok()) {
        return Fail("L3 compressor: " + compressor_r.status().message());
    }
    auto compressor = std::move(compressor_r).value();
    std::cout << "[e2e] L3 compressor created\n\n";

    // Step 1: Seed Redis with test L0 records
    const std::string test_user = "e2e-test-user";
    const std::string test_date = "2026-05-27";
    const int64_t test_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<storage::CacheRecord> test_records = {
        {std::vector<float>(384, 0.1f), "用户问：什么是C++模板？", "模板是C++的泛型编程机制..."},
        {std::vector<float>(384, 0.2f), "用户问：如何使用智能指针？", "智能指针包括unique_ptr、shared_ptr..."},
        {std::vector<float>(384, 0.3f), "用户问：RAII是什么？", "RAII是资源获取即初始化..."},
    };

    std::cout << "[e2e] Seeding " << test_records.size() << " L0 records to Redis...\n";

    std::string batch_key = "cache:batch:" + test_user + ":" + std::to_string(test_timestamp);

    std::unordered_map<std::string, std::string> field_values;
    for (size_t i = 0; i < test_records.size(); ++i) {
        const auto& record = test_records[i];
        std::string serialized = agent::semantic_cache::SerializeCacheRecord(record);
        field_values[std::to_string(i)] = serialized;
    }

    auto hmset_result = redis_pool->HMSet(batch_key, field_values);
    if (!hmset_result.ok()) {
        return Fail("Redis HMSET: " + hmset_result.status().message());
    }

    auto expire_status = redis_pool->Set(batch_key + ":ttl", "1", std::chrono::seconds(86400));
    if (!expire_status.ok()) {
        return Fail("Redis EXPIRE: " + expire_status.message());
    }

    std::cout << "[e2e] Seeded batch key: " << batch_key << " with " << field_values.size() << " records\n\n";

    // Step 2: Run L3 compression
    std::cout << "[e2e] Running L3 compression (LLM + Embedding)...\n";
    agent::memory::MemoryOwner owner{"default-tenant", test_user};
    auto start = std::chrono::steady_clock::now();
    auto compress_result = compressor->CompressDailyMemory(owner, test_date);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (!compress_result.ok()) {
        return Fail("L3 compression: " + compress_result.status().message());
    }

    int compressed_count = compress_result.value();
    std::cout << "[e2e] Compressed " << compressed_count << " records in "
              << elapsed.count() << " ms\n\n";

    if (compressed_count != static_cast<int>(test_records.size())) {
        return Fail("expected " + std::to_string(test_records.size()) +
                    " records, got " + std::to_string(compressed_count));
    }

    // Step 3: Retrieve L3 summary
    std::cout << "[e2e] Retrieving L3 summary...\n";
    auto summary_result = compressor->GetDailySummary(owner, test_date);
    if (!summary_result.ok()) {
        return Fail("GetDailySummary: " + summary_result.status().message());
    }

    const auto& summary = summary_result.value();
    std::cout << "[e2e] Retrieved summary:\n";
    std::cout << "  user_uuid: " << summary.user_uuid << "\n";
    std::cout << "  date:      " << summary.date << "\n";
    std::cout << "  timestamp: " << summary.timestamp << "\n";
    std::cout << "  source_record_count: " << summary.source_record_count << "\n";
    std::cout << "  summary:\n" << summary.summary << "\n";

    if (summary.user_uuid != test_user) {
        return Fail("user_uuid mismatch");
    }
    if (summary.date != test_date) {
        return Fail("date mismatch");
    }
    if (summary.source_record_count != static_cast<int>(test_records.size())) {
        return Fail("source_record_count mismatch");
    }
    if (summary.summary.empty()) {
        return Fail("summary is empty");
    }

    // Step 4: Test GetUserSummaries
    std::cout << "\n[e2e] Testing GetUserSummaries...\n";
    auto summaries_result = compressor->GetUserSummaries(owner, 10);
    if (!summaries_result.ok()) {
        return Fail("GetUserSummaries: " + summaries_result.status().message());
    }

    const auto& summaries = summaries_result.value();
    std::cout << "[e2e] Retrieved " << summaries.size() << " summaries\n";
    if (summaries.size() != 1) {
        return Fail("expected 1 summary, got " + std::to_string(summaries.size()));
    }

    // Step 5: Test semantic search
    std::cout << "\n[e2e] Testing semantic search...\n";
    auto search_result = compressor->SearchFacts(owner, "C++智能指针", 3);
    if (!search_result.ok()) {
        return Fail("SearchFacts: " + search_result.status().message());
    }

    const auto& search_results = search_result.value();
    std::cout << "[e2e] Found " << search_results.size() << " facts\n";
    for (size_t i = 0; i < search_results.size(); ++i) {
        std::cout << "  [" << (i+1) << "] " << search_results[i].payload << "\n";
    }

    if (search_results.empty()) {
        return Fail("SearchFacts returned no results");
    }

    // Cleanup
    std::cout << "\n[e2e] Cleaning up Redis test data...\n";
    auto del_result = redis_pool->Del({batch_key, batch_key + ":ttl"});
    if (!del_result.ok()) {
        std::cout << "[e2e] Warning: failed to delete test batch: " << del_result.status().message() << "\n";
    }

    std::cout << "[e2e] Tearing down...\n";
    compressor.reset();
    index_manager.reset();
    embedding_pipeline.reset();
    embedding_model.reset();
    tokenizer.reset();
    partition_registry.reset();
    vector_repo.reset();
    sqlite_pool->Close();
    sqlite_pool.reset();
    redis_pool->Shutdown();
    redis_pool.reset();

    fs::remove(test_db_path, ec);
    fs::remove(test_db_path + "-wal", ec);
    fs::remove(test_db_path + "-shm", ec);
    fs::remove(test_db_path + "-journal", ec);

    std::cout << "\n[e2e] ✓ ALL TESTS PASSED\n";
    return 0;
}
