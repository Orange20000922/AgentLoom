#pragma once

#include "skill_manifest.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace agent::skill {

// 从 JSON object 解析 SkillManifest 字段；缺失字段保留默认值，完整性校验交给 ValidateManifest。
// 以 inline 定义收敛到本头，供 InMemorySkillRegistry::RegisterJson 与 skills 配置段共用，
// 避免两处重复的字段映射，也不引入跨库链接依赖。
inline SkillManifest ParseSkillManifestJson(const nlohmann::json& object) {
    SkillManifest manifest;
    if (object.contains("exposure")) {
        const auto value = object.at("exposure").get<std::string>();
        if (value == "always") manifest.exposure = ToolExposure::Always;
        else if (value != "recall") throw std::invalid_argument("unsupported tool exposure");
    }
    if (object.contains("execution")) {
        const auto value = object.at("execution").get<std::string>();
        if (value == "prefetch") manifest.execution = ToolExecution::Prefetch;
        else if (value != "model") throw std::invalid_argument("unsupported tool execution");
    }
    if (object.contains("read_only")) manifest.read_only = object.at("read_only").get<bool>();
    if (object.contains("requires_confirmation")) manifest.requires_confirmation = object.at("requires_confirmation").get<bool>();
    if (object.contains("skill_id") && object["skill_id"].is_string()) manifest.skill_id = object["skill_id"].get<std::string>();
    if (object.contains("version") && object["version"].is_string()) manifest.version = object["version"].get<std::string>();
    if (object.contains("tool_name") && object["tool_name"].is_string()) manifest.tool_name = object["tool_name"].get<std::string>();
    if (object.contains("kind") && object["kind"].is_string()) manifest.kind = object["kind"].get<std::string>();
    if (object.contains("description") && object["description"].is_string()) manifest.description = object["description"].get<std::string>();
    if (object.contains("input_schema")) manifest.input_schema_json = object["input_schema"].dump();
    if (object.contains("output_schema")) manifest.output_schema_json = object["output_schema"].dump();
    if (object.contains("prompt_instruction") && object["prompt_instruction"].is_string()) manifest.prompt_instruction = object["prompt_instruction"].get<std::string>();
    if (object.contains("keywords") && object["keywords"].is_array()) {
        for (const auto& keyword : object["keywords"]) {
            if (keyword.is_string()) manifest.keywords.push_back(keyword.get<std::string>());
        }
    }
    if (object.contains("negative_keywords") && object["negative_keywords"].is_array()) {
        for (const auto& keyword : object["negative_keywords"]) {
            if (keyword.is_string()) manifest.negative_keywords.push_back(keyword.get<std::string>());
        }
    }
    if (object.contains("l4_payload") && object["l4_payload"].is_string()) manifest.l4_payload = object["l4_payload"].get<std::string>();
    if (object.contains("intent") && object["intent"].is_string()) manifest.intent = object["intent"].get<std::string>();
    if (object.contains("executor") && object["executor"].is_object()) {
        const auto& executor = object["executor"];
        if (executor.contains("type") && executor["type"].is_string()) manifest.executor.type = executor["type"].get<std::string>();
        if (executor.contains("reference") && executor["reference"].is_string()) manifest.executor.reference = executor["reference"].get<std::string>();
        if (executor.contains("configuration")) manifest.executor.configuration_json = executor["configuration"].dump();
    }
    return manifest;
}

} // namespace agent::skill
