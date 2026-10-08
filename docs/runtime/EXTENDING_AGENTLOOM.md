# Extending AgentLoom

本文档说明下游项目如何复用 AgentLoom 核心库、基础 Server 和扩展接口，同时保持领域代码与开源 Runtime 解耦。

## 1. CMake 依赖

源码集成可以通过 `add_subdirectory()` 使用构建树 target：

```cmake
add_subdirectory(path/to/AgentLoom)

target_link_libraries(my_agent_backend PRIVATE
    AgentLoom::core
    AgentLoom::runtime
    AgentLoom::gateway_foundation
    AgentLoom::persona_interaction
    AgentLoom::gateway_routing
)
```

可用的主要别名包括：

```text
AgentLoom::core
AgentLoom::net
AgentLoom::tls
AgentLoom::http_client
AgentLoom::config
AgentLoom::storage
AgentLoom::vector
AgentLoom::vector_storage
AgentLoom::semantic_cache
AgentLoom::memory
AgentLoom::document
AgentLoom::llm
AgentLoom::models
AgentLoom::cache
AgentLoom::ipc
AgentLoom::media
AgentLoom::media_inference
AgentLoom::runtime
AgentLoom::gateway_foundation
AgentLoom::persona_interaction
AgentLoom::gateway_routing
AgentLoom::gateway
AgentLoom::reference_gateway
AgentLoom::service
```

`AgentLoom::gateway` 是 `gateway_foundation`、`persona_interaction` 和 `gateway_routing` 的兼容聚合，不依赖参考认证、Document、Classroom、参考 Route 或参考 Server。只有在启用 `AGENTLOOM_BUILD_REFERENCE_GATEWAY` 时才导出 `AgentLoom::reference_gateway`；该 target 适用于 AgentLoom 的参考产品组合，不是下游 Runtime 的默认依赖。

这些 alias 不改变内部 `agent_*` target，方便现有工程逐步迁移。

安装式复用会导出相同的 `AgentLoom::...` target，并安装静态库、公共头文件、生成的 protobuf/gRPC 头及版本文件：

```powershell
cmake --install build/x64-Release `
  --config Release --prefix build/agentloom-package
```

消费端项目：

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyAgent LANGUAGES CXX)

find_package(AgentLoom CONFIG REQUIRED)

add_executable(my_agent main.cpp)
target_compile_features(my_agent PRIVATE cxx_std_20)
target_link_libraries(my_agent PRIVATE
    AgentLoom::core
    AgentLoom::runtime
    AgentLoom::gateway_foundation
    AgentLoom::persona_interaction
    AgentLoom::gateway_routing
)
```

配置消费端时，需要将 AgentLoom 安装前缀和 vcpkg package root 加入 `CMAKE_PREFIX_PATH`。公共头文件以安装前缀为根，例如：

```cpp
#include <AgentLoom/core/result.h>
#include <AgentLoom/service/persona/persona_interaction.h>
#include <AgentLoom/service/gateway/gateway_lifecycle.h>
#include <AgentLoom/service/gateway/gateway_routing.h>
```

`AgentLoomConfig.cmake` 会查找 Protobuf、gRPC、spdlog、OpenSSL、Redis client、libzip、pugixml、Faiss、OpenCV 和 nlohmann-json。构建 AgentLoom 时使用的本地预编译依赖路径会作为消费端默认值写入 config；迁移安装包或使用另一套依赖时，可以在 `find_package()` 前覆盖：

```cmake
set(AgentLoom_ONNXRUNTIME_ROOT "<onnxruntime-package>")
set(AgentLoom_LLAMA_CPP_INCLUDE_DIRS "<llama-includes>")
set(AgentLoom_LLAMA_CPP_LIBRARIES "<llama-libraries>")
set(AgentLoom_GSTREAMER_ROOT "<gstreamer-sdk>")
set(AgentLoom_BOOST_ROOT "<boost-root>")
```

SQLite 和 HuggingFace tokenizer C API 还可以通过 `AgentLoom_SQLITE_*` 与 `AgentLoom_HF_TOKENIZERS_*` 变量覆盖。静态库不会隔离 STL、CRT 或第三方库 ABI；Windows 下 AgentLoom、消费端和 vcpkg 依赖必须保持 generator、MSVC 工具集和 triplet 一致。

## 2. 推理服务边界

BERT/VLM 模型实现默认通过独立 gRPC Server 复用：

```text
commercial gateway
  -> generated protobuf/gRPC client
  -> emotion_inference_server / multimodal_inference_server
```

建议交付：

- Server 可执行文件；
- protobuf/gRPC 生成头和客户端协议；
- example 配置；
- 所需动态库、模型和许可证清单；
- health check、deadline、认证 metadata 和 trace 约定。

不建议让业务 Gateway 直接链接推理 Server 内部的可变模型 context。模型、GPU 驱动、CUDA 和故障恢复应保持独立生命周期。

## 3. 领域评估扩展

AgentLoom 不包含特定组织的指标、权重或教学评估实现。下游可以实现 `IReportEvaluator`：

```cpp
class CommercialReportEvaluator final
    : public agent::service::gateway::IReportEvaluator {
public:
    core::Result<nlohmann::json> Evaluate(
        const agent::service::gateway::ReportEvaluationRequest& request) override;
};
```

provider 自行持有数据库、缓存和领域配置：

```cpp
agent::service::gateway::PersonaGatewayServerDependencies dependencies;
dependencies.report_evaluator = std::make_shared<CommercialReportEvaluator>(/* ... */);
```

未配置 provider 时，Gateway 仍返回基础 session metrics。provider 失败时，Gateway 记录安全日志并保留基础报告，不让领域评估失败破坏 session 主链路。

## 4. 其他扩展接口

常见扩展点包括：

- `llm::ILlmClient`：本地或 OpenAI-compatible LLM；
- `persona::IEmotionAnalyzer`：BERT gRPC、融合层或领域模型；
- `persona::IMemoryContextProvider`：L0/L3/L4 上下文；
- `persona::IToolMemoryProvider`：工具记忆和 Skill 触发；
- `persona::ISkillSessionManager`：有限工具会话；
- `gateway::IPersonaMetadataStore`：账号级 Persona 元数据；
- `document::IDocumentEmbeddingProvider`：文档 embedding；
- `document::IDocumentLlmChunkCache`：文档 chunk LLM cache；
- `media::IVlmVisionClient`：VLM coordinator 的真实推理 adapter。

新增业务模块应优先增加窄接口，避免让核心库依赖领域配置、具体数据库 schema 或产品路由。

## 5. 本地配置

### Persona Turn 取消

下游通过 `IPersonaInteraction::CancelTurn(PersonaSessionQuery)` 按 Session 请求取消当前异步 Turn。
`trusted_user_uuid` 沿用 Session owner 校验；`trace_id` 是取消请求的 trace，不等于原 Turn trace。
返回 OK 表示取消请求已接纳，原提交 callback 在在途 Provider 回调收口后恰好返回一次 `Cancelled`；
重复请求在 Turn 仍在途时返回 OK，收口后无当前 Turn 返回 `NotFound`。排队中的后续 Turn 不受影响。

```cpp
auto status = interaction.CancelTurn({session_id, cancel_trace_id, authenticated_user_uuid});
```

LLM 请求通过原有 `IAsyncLlmOperation::Cancel()` 主动取消；emotion、memory 和工具 coordinator
尚无通用取消句柄，需等待已接纳回调，再阻止后续阶段和 Session 提交。
取消不回滚已经发生的工具副作用或记忆写入，不关闭 Session；同步 LLM 路径不提供该异步取消能力。
Runtime 的操作索引在整个异步 Turn 生命周期内保留 Session，工具 follow-up 的句柄单独登记并安全清理。
SDK 与消费者需一起重编译，自行实现 `IPersonaInteraction` 的下游需实现新增 `CancelTurn`。

机器相关配置不进入 Git：

```text
.clangd
CMakeCache.txt
CMakePresets.json.bak-*
config/e2e_test.json
tools/llm_smoke_test.json
tools/llm_smoke_test.key
```

公开仓只保留 `.example` 文件。商业项目可以维护自己的私有配置仓或部署系统，不应把真实 key、模型绝对路径和合作方配置提交回 AgentLoom。

## 6. 兼容性建议

- 下游锁定 AgentLoom commit/tag，并记录 MSVC/vcpkg/CMake ABI 组合。
- 公共接口变化应通过编译期 consumer test 验证。
- 进程协议优先保持向后兼容；新增 protobuf 字段使用可选语义。
- 不跨 runner/context 共享可变 llama KV state。
- 不把单帧 VLM 输出直接写入长期事实或情绪状态机。
- 对下游 evaluator、LLM、memory 和 media adapter 覆盖成功、失败、超时和关闭路径。
