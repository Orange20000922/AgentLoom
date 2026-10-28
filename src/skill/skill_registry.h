#pragma once

#include "skill_manifest.h"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agent::skill {

class ISkillRegistry {
public:
    virtual ~ISkillRegistry() = default;
    virtual core::Status Register(SkillManifest manifest) = 0;
    virtual core::Status RegisterAll(const std::vector<SkillManifest>& manifests) = 0;
    virtual core::Status RegisterJson(std::string_view manifest_json) = 0;
    virtual core::Result<std::optional<SkillManifest>> Find(
        std::string_view skill_id, std::string_view version = {}) const = 0;
    virtual core::Result<SkillManifest> Require(
        std::string_view skill_id, std::string_view version = {}) const = 0;
    virtual core::Result<SkillManifest> RequireByToolName(std::string_view tool_name) const = 0;
    virtual core::Result<std::vector<SkillManifest>> List() const = 0;
};

class InMemorySkillRegistry final : public ISkillRegistry {
public:
    core::Status Register(SkillManifest manifest) override;
    core::Status RegisterAll(const std::vector<SkillManifest>& manifests) override;
    core::Status RegisterJson(std::string_view manifest_json) override;
    core::Result<std::optional<SkillManifest>> Find(
        std::string_view skill_id, std::string_view version = {}) const override;
    core::Result<SkillManifest> Require(
        std::string_view skill_id, std::string_view version = {}) const override;
    core::Result<SkillManifest> RequireByToolName(std::string_view tool_name) const override;
    core::Result<std::vector<SkillManifest>> List() const override;

private:
    static std::string Key(std::string_view skill_id, std::string_view version);
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SkillManifest> manifests_;
};

} // namespace agent::skill
