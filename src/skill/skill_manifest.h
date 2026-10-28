#pragma once

#include "../core/result.h"

#include <cstddef>
#include <string>
#include <vector>

namespace agent::skill {

struct SkillExecutorSpec {
    std::string type = "native";
    std::string reference;
    std::string configuration_json = "{}";
};

enum class ExecutionMode {
    Exclusive,
    SerialQueue,
    BoundedParallel,
    StreamWindowed,
};

enum class ToolExposure { Recall, Always };
enum class ToolExecution { Model, Prefetch };

struct SkillManifest {
    std::string skill_id;
    // OpenAI-compatible function name；与可含点号的 skill_id 分离。
    std::string tool_name;
    std::string version;
    std::string kind = "native";
    std::string description;
    std::string input_schema_json = R"({"type":"object"})";
    std::string output_schema_json = R"({"type":"object"})";
    std::string prompt_instruction;
    // 触发该工具的关键词；由 provider 编译为确定性正则兜底通道，与向量召回互补。
    std::vector<std::string> keywords;
    // 否定排除关键词：命中即取消该工具的正则触发（向量召回不受影响）。
    std::vector<std::string> negative_keywords;
    // L4 工具记忆种子语义描述（embedding 用）；空则回退 description。
    std::string l4_payload;
    // 意图标识：用于派生 L4 memory_hash 后缀（可选）。
    std::string intent;
    ExecutionMode execution_mode = ExecutionMode::Exclusive;
    std::size_t max_parallel_per_session = 1;
    bool requires_confirmation = false;
    SkillExecutorSpec executor;
    ToolExposure exposure = ToolExposure::Recall;
    ToolExecution execution = ToolExecution::Model;
    // 只读是预取的必要条件，仍须可信参数绑定、schema 和消费者授权。
    bool read_only = false;
};

core::Status ValidateManifest(const SkillManifest& manifest);

} // namespace agent::skill
