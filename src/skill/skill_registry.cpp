#include "skill_registry.h"
#include "skill_manifest_json.h"
#include "skill_prompt_compiler.h"

#include <nlohmann/json.hpp>
#include <mutex>
#include <regex>
#include <unordered_set>
#include <algorithm>

namespace agent::skill {

namespace {
std::string DefaultToolName(std::string_view skill_id) {
    std::string result;
    result.reserve(skill_id.size());
    for (const char value : skill_id) {
        result.push_back((value >= 'A' && value <= 'Z') ||
                         (value >= 'a' && value <= 'z') ||
                         (value >= '0' && value <= '9') || value == '_' || value == '-'
                             ? value : '_');
    }
    return result;
}
}

core::Status ValidateManifest(const SkillManifest& manifest) {
    if (manifest.execution == ToolExecution::Prefetch && (!manifest.read_only || manifest.requires_confirmation)) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "prefetch requires a read-only tool");
    }
    if (manifest.skill_id.empty() || manifest.version.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "skill_id and version are required");
    }
    const auto tool_name = manifest.tool_name.empty()
        ? DefaultToolName(manifest.skill_id) : manifest.tool_name;
    if (!std::regex_match(tool_name, std::regex(R"(^[A-Za-z0-9_-]+$)"))) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "tool_name must contain only letters, digits, underscore or hyphen");
    }
    try {
        const auto input = nlohmann::json::parse(manifest.input_schema_json);
        const auto output = nlohmann::json::parse(manifest.output_schema_json);
        if (!input.is_object() || !output.is_object()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "skill schemas must be JSON objects");
        }
    } catch (const std::exception& error) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   std::string("invalid skill schema: ") + error.what());
    }
    if (auto status = ValidateSkillSchema(manifest.input_schema_json); !status.ok()) return status;
    if (auto status = ValidateSkillSchema(manifest.output_schema_json); !status.ok()) return status;
    if (manifest.max_parallel_per_session == 0) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "max_parallel_per_session must be positive");
    }
    if (manifest.executor.type.empty() || manifest.executor.reference.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "skill executor type and reference are required");
    }
    try {
        if (!nlohmann::json::parse(manifest.executor.configuration_json).is_object()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "skill executor configuration must be an object");
        }
    } catch (const nlohmann::json::exception& error) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   std::string("invalid skill executor configuration: ") + error.what());
    }
    return core::Status::Ok();
}

core::Status InMemorySkillRegistry::Register(SkillManifest manifest) {
    if (manifest.tool_name.empty()) manifest.tool_name = DefaultToolName(manifest.skill_id);
    if (auto status = ValidateManifest(manifest); !status.ok()) return status;
    std::unique_lock lock(mutex_);
    const auto key = Key(manifest.skill_id, manifest.version);
    if (manifests_.contains(key)) {
        return core::Status::Error(core::ErrorCode::AlreadyExists,
                                   "skill manifest already registered");
    }
    for (const auto& [_, existing] : manifests_) {
        if (existing.tool_name == manifest.tool_name) {
            return core::Status::Error(core::ErrorCode::AlreadyExists,
                                       "skill tool name already registered");
        }
    }
    manifests_.emplace(key, std::move(manifest));
    return core::Status::Ok();
}

core::Status InMemorySkillRegistry::RegisterAll(const std::vector<SkillManifest>& manifests) {
    std::vector<SkillManifest> normalized;
    normalized.reserve(manifests.size());
    std::unordered_set<std::string> keys;
    std::unordered_set<std::string> tool_names;
    for (const auto& source : manifests) {
        auto manifest = source;
        if (manifest.tool_name.empty()) manifest.tool_name = DefaultToolName(manifest.skill_id);
        if (auto status = ValidateManifest(manifest); !status.ok()) return status;
        if (!keys.insert(Key(manifest.skill_id, manifest.version)).second ||
            !tool_names.insert(manifest.tool_name).second) {
            return core::Status::Error(core::ErrorCode::AlreadyExists,
                                       "duplicate Skill manifest or tool name in batch");
        }
        normalized.push_back(std::move(manifest));
    }
    std::unique_lock lock(mutex_);
    for (const auto& manifest : normalized) {
        if (manifests_.contains(Key(manifest.skill_id, manifest.version))) {
            return core::Status::Error(core::ErrorCode::AlreadyExists,
                                       "skill manifest already registered");
        }
        for (const auto& [_, existing] : manifests_) {
            if (existing.tool_name == manifest.tool_name) {
                return core::Status::Error(core::ErrorCode::AlreadyExists,
                                           "skill tool name already registered");
            }
        }
    }
    for (auto& manifest : normalized) manifests_.emplace(Key(manifest.skill_id, manifest.version), std::move(manifest));
    return core::Status::Ok();
}

core::Status InMemorySkillRegistry::RegisterJson(std::string_view manifest_json) {
    try {
        const auto body = nlohmann::json::parse(manifest_json);
        if (!body.is_object()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument, "skill manifest must be an object");
        }
        return Register(ParseSkillManifestJson(body));
    } catch (const std::exception& error) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   std::string("invalid skill manifest JSON: ") + error.what());
    }
}

core::Result<std::optional<SkillManifest>> InMemorySkillRegistry::Find(
    std::string_view skill_id, std::string_view version) const {
    std::shared_lock lock(mutex_);
    if (!version.empty()) {
        auto it = manifests_.find(Key(skill_id, version));
        if (it == manifests_.end()) return std::optional<SkillManifest>{};
        return std::optional<SkillManifest>{it->second};
    }
    std::optional<SkillManifest> found;
    for (const auto& [key, manifest] : manifests_) {
        if (manifest.skill_id != skill_id) continue;
        if (found) return core::Status::Error(core::ErrorCode::FailedPrecondition,
            "multiple skill versions require an explicit version");
        found = manifest;
    }
    return found;
}

core::Result<SkillManifest> InMemorySkillRegistry::Require(
    std::string_view skill_id, std::string_view version) const {
    auto result = Find(skill_id, version);
    if (!result.ok()) return result.status();
    if (!result.value().has_value()) {
        return core::Status::Error(core::ErrorCode::NotFound, "skill manifest not found");
    }
    return *result.value();
}

core::Result<SkillManifest> InMemorySkillRegistry::RequireByToolName(std::string_view tool_name) const {
    std::shared_lock lock(mutex_);
    for (const auto& [_, manifest] : manifests_) {
        if (manifest.tool_name == tool_name) return manifest;
    }
    return core::Status::Error(core::ErrorCode::NotFound, "skill tool name not found");
}

core::Result<std::vector<SkillManifest>> InMemorySkillRegistry::List() const {
    std::shared_lock lock(mutex_);
    std::vector<SkillManifest> result;
    for (const auto& [_, manifest] : manifests_) result.push_back(manifest);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.skill_id < b.skill_id;
    });
    return result;
}

std::string InMemorySkillRegistry::Key(std::string_view skill_id, std::string_view version) {
    return std::string(skill_id) + "@" + std::string(version);
}

} // namespace agent::skill
