#pragma once
#include "result.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace agent::service::persona {
enum class ContextSource { L0, L3, L4, Observation, Trusted, Legacy, ToolResult, Count };
struct ContextItem {
    ContextSource source = ContextSource::Legacy;
    std::string record_id;
    std::string content;
    std::string provenance_json = "{}";
    bool required = false;
    bool trusted_instruction = false;
};
struct ContextSourceBudget {
    std::size_t max_bytes = 64 * 1024;
    bool allow_drop_optional = false;
};
struct ContextBudgetReport {
    std::array<std::size_t, static_cast<std::size_t>(ContextSource::Count)> input_bytes{};
    std::array<std::size_t, static_cast<std::size_t>(ContextSource::Count)> retained_bytes{};
    std::array<std::size_t, static_cast<std::size_t>(ContextSource::Count)> counts{};
    std::vector<std::string> dropped_ids;
    std::vector<std::string> duplicate_ids;
};
using ContextBudgets = std::array<ContextSourceBudget, static_cast<std::size_t>(ContextSource::Count)>;
core::Result<std::string> CompileContext(const std::vector<ContextItem>& items,
    const ContextBudgets& budgets, ContextBudgetReport& report);

struct TrustedContextRequest {
    std::string tenant_id;
    std::string user_id;
    std::string session_id;
    std::string persona_id;
    std::string trace_id;
};
class ITrustedContextProvider {
public:
    virtual ~ITrustedContextProvider() = default;
    // 只读取宿主已就绪的紧凑快照；外部 RAG I/O 通过预取 executor 完成。
    virtual core::Result<std::vector<ContextItem>> GetContext(const TrustedContextRequest&) const = 0;
};
struct LlmCallObservation {
    std::string trace_id;
    std::size_t tool_round = 0;
    ContextBudgetReport context;
    std::size_t tool_schema_bytes = 0;
    std::vector<std::string> tools_declared;
    std::vector<std::string> required_tools;
    std::vector<std::string> satisfied_tools;
    std::vector<std::string> tools_recalled;
    std::vector<std::string> tool_calls;
    std::vector<std::string> degraded_reasons;
    int max_tokens = 0;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t total_tokens = 0;
    std::string finish_reason;
    core::Status status;
};
class IPersonaRuntimeObserver {
public:
    virtual ~IPersonaRuntimeObserver() = default;
    virtual void PromptReady(const LlmCallObservation&) const = 0;
    virtual void LlmCompleted(const LlmCallObservation&) const = 0;
    // 含已完成调用的累计 usage；失败状态也保留本轮来源与必需工具验收信息。
    virtual void TurnCompleted(const LlmCallObservation&) const {}
};
}
