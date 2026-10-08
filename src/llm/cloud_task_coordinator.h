#pragma once

#include "logger_adapter.h"
#include "llm_client.h"
#include "ordered_bitmap_window.h"
#include "thread_pool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent::llm {

struct CloudTaskId {
    std::string tenant_id;
    std::string user_uuid;
    std::string session_id;
    std::string task_id;
};

struct CloudTaskChunk {
    CloudTaskId task;
    std::uint64_t session_sequence = 0;
    std::uint64_t chunk_sequence = 0;
    std::uint64_t final_chunk_sequence = 0;
    std::size_t source_begin = 0;
    std::size_t source_end = 0;
    std::string route_alias;
    ChatCompletionRequest request;
    std::uint32_t attempt = 0;
};

struct CloudTaskChunkResult {
    CloudTaskId task;
    std::uint64_t session_sequence = 0;
    std::uint64_t chunk_sequence = 0;
    std::uint32_t attempt = 0;
    std::optional<ChatCompletionResponse> response;
    core::Status status = core::Status::Ok();
};

struct CloudTaskSummary {
    CloudTaskId task;
    std::size_t chunk_count = 0;
    std::size_t completed_chunks = 0;
    std::size_t successful_chunks = 0;
    std::size_t failed_chunks = 0;
    core::Status status = core::Status::Ok();
};

class ICloudTaskResultSink {
public:
    virtual ~ICloudTaskResultSink() = default;

    virtual core::Status OnChunk(CloudTaskChunkResult result) = 0;
    virtual core::Status OnCompleted(CloudTaskSummary summary) = 0;
};

class ICloudTaskOperation {
public:
    virtual ~ICloudTaskOperation() = default;

    virtual void Cancel() noexcept = 0;
};

struct CloudTaskCoordinatorOptions {
    std::size_t worker_count = 4;
    std::size_t max_chunks = 256;
    std::size_t queue_capacity = 0;
    std::size_t max_pending_tasks = 8;
    std::string thread_pool_name = "cloud-task-workers";
};

struct CloudTaskCoordinatorSnapshot {
    std::size_t submitted_chunks = 0;
    std::size_t completed_chunks = 0;
    std::size_t successful_chunks = 0;
    std::size_t failed_chunks = 0;
    std::size_t rejected_chunks = 0;
    bool running = false;
};

class ICloudTaskCoordinator {
public:
    virtual ~ICloudTaskCoordinator() = default;

    virtual core::Result<std::vector<CloudTaskChunkResult>> Execute(
        std::vector<CloudTaskChunk> chunks) = 0;

    virtual core::Result<std::future<core::Result<std::vector<CloudTaskChunkResult>>>> ExecuteAsync(
        std::vector<CloudTaskChunk> chunks) = 0;

    virtual core::Result<std::shared_ptr<ICloudTaskOperation>> StartAsync(
        std::vector<CloudTaskChunk> chunks,
        std::shared_ptr<ICloudTaskResultSink> sink) = 0;

    virtual CloudTaskCoordinatorSnapshot Snapshot() const = 0;
};

class CloudTaskCoordinator final : public ICloudTaskCoordinator {
public:
    CloudTaskCoordinator(
        std::shared_ptr<ILlmClient> llm_client,
        CloudTaskCoordinatorOptions options = {},
        core::LoggerAdapter logger = {});
    CloudTaskCoordinator(
        std::shared_ptr<IAsyncLlmClient> llm_client,
        CloudTaskCoordinatorOptions options = {},
        core::LoggerAdapter logger = {});
    ~CloudTaskCoordinator() override;

    CloudTaskCoordinator(const CloudTaskCoordinator&) = delete;
    CloudTaskCoordinator& operator=(const CloudTaskCoordinator&) = delete;

    core::Result<std::vector<CloudTaskChunkResult>> Execute(
        std::vector<CloudTaskChunk> chunks) override;

    core::Result<std::future<core::Result<std::vector<CloudTaskChunkResult>>>> ExecuteAsync(
        std::vector<CloudTaskChunk> chunks) override;

    core::Result<std::shared_ptr<ICloudTaskOperation>> StartAsync(
        std::vector<CloudTaskChunk> chunks,
        std::shared_ptr<ICloudTaskResultSink> sink) override;

    CloudTaskCoordinatorSnapshot Snapshot() const override;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}
