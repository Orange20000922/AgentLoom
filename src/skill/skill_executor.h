#pragma once

#include "skill_prompt_compiler.h"
#include "skill_deadline.h"
#include "../service/persona/skill_session_manager.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <stop_token>
#include <atomic>

namespace agent::skill {

struct SkillExecutionRequest {
    SkillCall call;
    std::string session_id;
    std::string user_uuid;
    std::string persona_id;
    std::string trace_id;
    // executor 以此标识 Cancel 的资源；模型 call_id 只用于协议关联，不能作全局资源 ID。
    std::string execution_id;
};

struct SkillExecutionCallbacks {
    std::function<void(SkillResult)> on_result;
    std::function<void(service::persona::SkillObservation)> on_observation;
};

class ISkillExecutor {
public:
    virtual ~ISkillExecutor() = default;
    virtual core::Result<service::persona::SkillSessionSnapshot> Start(
        const SkillExecutionRequest& request,
        SkillExecutionCallbacks callbacks) = 0;
    virtual core::Status Cancel(std::string_view execution_id) = 0;
};

class ISkillExecutorFactory {
public:
    virtual ~ISkillExecutorFactory() = default;
    virtual core::Status Register(std::string type, std::shared_ptr<ISkillExecutor> executor) = 0;
    virtual core::Status RegisterReference(std::string type, std::string reference,
                                           std::shared_ptr<ISkillExecutor> executor) = 0;
    virtual core::Result<std::shared_ptr<ISkillExecutor>> Resolve(
        const SkillManifest& manifest) const = 0;
};

class InMemorySkillExecutorFactory final : public ISkillExecutorFactory {
public:
    core::Status Register(std::string type, std::shared_ptr<ISkillExecutor> executor) override;
    core::Status RegisterReference(std::string type, std::string reference,
                                   std::shared_ptr<ISkillExecutor> executor) override;
    core::Result<std::shared_ptr<ISkillExecutor>> Resolve(
        const SkillManifest& manifest) const override;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ISkillExecutor>> executors_;
    std::unordered_map<std::string, std::shared_ptr<ISkillExecutor>> references_;
};

class ISkillInvocationService {
public:
    virtual ~ISkillInvocationService() = default;
    virtual core::Result<service::persona::SkillSessionSnapshot> Invoke(
        SkillExecutionRequest request, SkillExecutionCallbacks callbacks) = 0;
    virtual core::Status Cancel(std::string_view execution_id) = 0;
};

class SkillInvocationService final : public ISkillInvocationService,
                                     public std::enable_shared_from_this<SkillInvocationService> {
public:
    SkillInvocationService(std::shared_ptr<const ISkillRegistry> registry,
                           std::shared_ptr<const ISkillExecutorFactory> factory,
                           std::shared_ptr<service::persona::ISkillSessionManager> sessions);
    core::Result<service::persona::SkillSessionSnapshot> Invoke(
        SkillExecutionRequest request, SkillExecutionCallbacks callbacks) override;
    core::Status Cancel(std::string_view execution_id) override;

private:
    std::shared_ptr<const ISkillRegistry> registry_;
    std::shared_ptr<const ISkillExecutorFactory> factory_;
    std::shared_ptr<service::persona::ISkillSessionManager> sessions_;
    mutable std::mutex active_mutex_;
    struct ActiveExecution {
        std::shared_ptr<ISkillExecutor> executor;
        std::function<void(SkillResult)> complete;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    std::unordered_map<std::string, ActiveExecution> active_executions_;
};

class ISkillToolPolicy;

struct SkillToolRequirement {
    std::string skill_id;
    std::string version;
};

struct SkillToolCallContext {
    std::string session_id;
    std::string user_uuid;
    std::string persona_id;
    std::string trace_id;
    std::chrono::milliseconds timeout{30000};
    // nullopt 兼容旧直接调用方；显式空集合拒绝所有工具，Runtime 始终传入本轮声明集合。
    std::optional<std::vector<std::string>> allowed_tool_names;
    std::size_t max_result_bytes = 64 * 1024;
    ToolResultFormat result_format = ToolResultFormat::LegacyInline;
    std::shared_ptr<const ISkillToolPolicy> policy;
    // 总工具阶段 deadline 与取消；同一 Turn 的预取和模型工具调用共享可信控制。
    std::shared_ptr<ISkillDeadlineScheduler> deadline_scheduler;
    std::stop_token cancellation;
    std::vector<std::string> required_tool_names;
};

// 业务身份、审批发布、来源版本和引用由消费者校验；此接口不拥有 Session 或调度资源。
class ISkillToolPolicy {
public:
    virtual ~ISkillToolPolicy() = default;
    virtual core::Status Authorize(
        const SkillToolCallContext& context, const SkillCall& call,
        const SkillManifest& manifest) const = 0;
    virtual core::Status ValidateResult(
        const SkillToolCallContext& context, const SkillCall& call,
        const SkillManifest& manifest, const SkillResult& result) const = 0;
    // 声明资格与具体调用授权分开；不替代执行时基于真实参数的 Authorize。
    virtual core::Status AuthorizeDeclaration(const SkillToolCallContext&,
        const SkillManifest&) const { return core::Status::Ok(); }
    // 仅可信宿主实现可指定本轮必需工具；不从用户 JSON 或 LLM 文本提取策略。
    virtual core::Result<std::vector<SkillToolRequirement>> RequiredTools(
        const SkillToolCallContext&) const { return std::vector<SkillToolRequirement>{}; }
    virtual core::Result<std::string> BindPrefetchArguments(
        const SkillToolCallContext&, const SkillManifest&, std::string_view) const {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "prefetch requires a trusted argument binder");
    }
};

class ISkillToolCallCoordinator {
public:
    using Completion = std::function<void(core::Result<std::vector<llm::ChatMessage>>) >;
    virtual ~ISkillToolCallCoordinator() = default;
    virtual core::Result<std::vector<llm::ChatMessage>> Execute(
        const llm::ChatCompletionResponse& response,
        const SkillToolCallContext& context) = 0;
    /// 异步执行工具调用；返回后不阻塞调用线程，完成回调恰好调用一次。
    virtual core::Status ExecuteAsync(
        const llm::ChatCompletionResponse& response,
        SkillToolCallContext context,
        Completion completion) = 0;
};

class SkillToolCallCoordinator final : public ISkillToolCallCoordinator {
public:
    SkillToolCallCoordinator(std::shared_ptr<const ISkillRegistry> registry,
                             std::shared_ptr<ISkillInvocationService> invocation);
    core::Result<std::vector<llm::ChatMessage>> Execute(
        const llm::ChatCompletionResponse& response,
        const SkillToolCallContext& context) override;
    core::Status ExecuteAsync(
        const llm::ChatCompletionResponse& response,
        SkillToolCallContext context,
        Completion completion) override;

private:
    std::shared_ptr<const ISkillRegistry> registry_;
    std::shared_ptr<ISkillInvocationService> invocation_;
};

} // namespace agent::skill
