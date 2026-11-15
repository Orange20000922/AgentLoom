#pragma once

#include "../../core/coroutine_task.h"
#include "../../skill/skill_turn_plan.h"

namespace agent::runtime {

struct AgentTaskRequest {
    std::string task;
    // 由宿主绑定；模型参数不能授予工具权限或修改业务 Session/trace。
    std::string trusted_context;
    skill::SkillToolCallContext context;
    std::vector<std::string> allowed_skill_ids;
    std::chrono::milliseconds timeout{30000};
};

struct AgentTaskResult {
    std::string run_id;
    std::string summary;
    std::size_t model_requests = 0;
    std::size_t tool_calls = 0;
    std::size_t total_tokens = 0;
};

struct AgentTaskOptions {
    std::string model;
    std::size_t max_active_tasks = 8;
    std::size_t max_model_requests = 16;
    std::size_t max_tool_calls = 64;
    std::size_t max_context_bytes = 256 * 1024;
    std::size_t max_total_tokens = 32768;
    int max_output_tokens = 2048;
    std::chrono::milliseconds max_duration{60000};
};

class IAgentTaskLease {
public:
    virtual ~IAgentTaskLease() = default;
    virtual void Cancel() noexcept = 0;
    virtual std::string RunId() const = 0;
};

class IAgentTaskRuntime {
public:
    using Completion = std::function<void(core::Result<AgentTaskResult>)>;
    virtual ~IAgentTaskRuntime() = default;
    // 丢弃租约取消任务；宿主应保留租约直到完成。拒绝接纳时不调用 completion。
    virtual core::Result<std::shared_ptr<IAgentTaskLease>> Start(
        AgentTaskRequest request, Completion completion) = 0;
    virtual void Close() noexcept = 0;
    // 包含已逻辑取消、仍等待底层回调收尾的执行体。
    virtual std::size_t ActiveTasks() const = 0;
};

class AgentTaskRuntime final : public IAgentTaskRuntime {
public:
    AgentTaskRuntime(std::shared_ptr<llm::IAsyncLlmClient> llm,
        std::shared_ptr<const skill::ISkillRegistry> registry,
        std::shared_ptr<const skill::ISkillExecutorFactory> factory,
        std::shared_ptr<skill::ISkillDeadlineScheduler> deadlines,
        std::shared_ptr<core::ThreadPool> pool, AgentTaskOptions options = {});
    ~AgentTaskRuntime() override;
    core::Result<std::shared_ptr<IAgentTaskLease>> Start(
        AgentTaskRequest request, Completion completion) override;
    void Close() noexcept override;
    std::size_t ActiveTasks() const override;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// 可信绑定器把委派调用关联到业务快照；不依赖 PersonaInteraction。
class ISubagentContextProvider {
public:
    virtual ~ISubagentContextProvider() = default;
    virtual core::Result<AgentTaskRequest> Prepare(
        const skill::SkillExecutionRequest& request) const = 0;
};

skill::SkillManifest SubagentSkillManifest();
class SubagentSkillExecutor final : public skill::ISkillExecutor {
public:
    SubagentSkillExecutor(std::shared_ptr<IAgentTaskRuntime> runtime,
        std::shared_ptr<const ISubagentContextProvider> context);
    ~SubagentSkillExecutor() override;
    core::Result<service::persona::SkillSessionSnapshot> Start(
        const skill::SkillExecutionRequest&, skill::SkillExecutionCallbacks) override;
    core::Status Cancel(std::string_view execution_id) override;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
