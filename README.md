# AgentLoom

> 可组合的 Agent 服务端基础设施 —— HTTP/WebSocket Gateway、Persona Runtime、语义记忆、实时视觉输入与 BERT/VLM 推理，C++20 实现

中文 | [English](README_EN.md)

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green.svg)](https://cmake.org/)
[![Tests](https://img.shields.io/badge/CTest-E2E%20%2B%20unit-brightgreen.svg)](#测试)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

## 项目定位

AgentLoom 是一个面向服务端智能体的 C++20 Runtime。它把 HTTP/WebSocket Gateway、Persona 与 Session Runtime、语义缓存、长期记忆、文档分析、实时媒体链路以及 BERT/VLM 推理组织成可组合的库目标和基础 Server。

项目源自教育智能体后端，但开源边界不绑定教育产品：通用 Runtime、协议和基础 Server 留在 AgentLoom；Persona、Prompt、领域评估、数据集与产品编排由下游项目持有。领域报告评估可通过 `IReportEvaluator` 注入，仓库不包含特定组织的评估器实现。

当前版本为 `0.1.0`。接口仍在活跃开发中，适合用于二次开发、系统集成和面向生产的工程验证，不保证ABI的绝对稳定。

## 核心特性

- **可组合架构**：核心能力以 CMake target 形式暴露，既可作为库集成到下游源码工程，也可以作为独立 Server 部署。
- **全链路异步对话热路径**：同一 Session 严格保序，用户/AI Emotion、L0 Lookup、Cloud LLM 和 Memory Store 通过 continuation 推进；外部等待不占用业务 worker，不同 Session 可并行执行。
- **实时多模态感知**：从浏览器摄像头 WebRTC 输入、GStreamer 解码、OpenCV 动态抽帧到 VLM 推理的完整链路。
- **推理成本优化**：语义缓存、Prompt KV Cache 与 VLM 结果缓存协同，降低重复推理开销；VRAM guard 支持低显存与 OOM 降级。
- **进程边界清晰**：BERT 与 VLM 推理作为独立 gRPC 进程，支持容器化与独立扩缩容；Gateway 通过 gRPC 或共享内存 IPC 对接。
- **生产工程质量**：持续维护的单元、跨进程 E2E 和容量压测覆盖并发路径、错误恢复、资源释放与结构化拒绝；`core::Status`/`Result` 统一错误模型。

## 已实现能力

- **Gateway Runtime**：统一 HTTP/WebSocket 入口、JWT/cookie 认证、静态文件托管、请求过滤、背压和运行时维护任务。
- **Agent Runtime**：Persona、Session、Skill Session、多人格调度、主动发言状态机、deferred Session lane、异步 Emotion/LLM/Memory continuation、trace 和情绪状态持久化。
- **模型服务**：ONNX Runtime BERT 情绪推理，以及基于 llama.cpp/mtmd 的流式与同步 VLM 推理。
- **情感融合**：BERT 主干结合关键词等证据，通过可配置 fusion head、置信度和 margin gate 更新 V-A 状态。
- **记忆与缓存**：Redis/SQLite L0 记忆、L3 压缩记忆、Exact/Faiss 向量检索、L0 embedding micro-batch、异步 Lookup/Store，以及基于 llama.cpp sequence state 的 image-prefix Prompt KV Cache（memory/Redis 双后端）。
- **文档链路**：DOCX/PPTX OOXML 提取、受管文件存储、分块分析、元数据和 LLM/语义缓存。
- **连续对话分块**：复用批量 embedding 与 SIMD 内积，以滚动块向量、时间代价和有界动态规划生成互不重叠的 DialogueBlock；上下文化 embedding 模式显式版本化。
- **云任务并行执行**：可分解任务通过有界 worker pool 并发调用 OpenAI-compatible/本地 LLM，使用 ordered bitmap window 对乱序完成和失败终态进行保序归并。
- **实时多模态输入**：WebRTC signaling（offer/answer/ICE/resume）、GStreamer `webrtcbin` media pipeline、OpenCV 动态抽帧（MOG2/直方图/边缘变化/EMA/cooldown）、关键帧 JPEG/PNG 编码（NVIDIA/VAAPI/D3D11/QSV 硬件加速与软件回退）。
- **帧推理链路**：共享内存帧 IPC（MPMC sequence ring、RAII claim、epoch 重建）+ gRPC IPC 控制面（grant/revoke/probe、lease 协调、跨进程故障恢复）组成数据面/控制面分离的传输层；上层由有序准入、mmap 磁盘 spool 溢出回放、私有 backlog、VLM coordinator 和执行级封口聚合（running → sealing → replay → aggregate）构成受控的关键帧推理生命周期。
- **基础设施**：`core::Status`/`Result`、RAII 句柄封装、内存池、线程池、对象池、线程安全队列、keyed serial executor（按 key 串行、会话亲和保序）、task group（结构化并发）、TLS context 和并发 HTTP client。

## 运行时边界

```text
Browser / Downstream Application
              |
              v
 agent_gateway_server  <---->  OpenAI-compatible / local LLM
   HTTP · WebSocket              backend
   Persona · Session
   Memory · Document
   Media · Skill
              |
              +---- gRPC ----> emotion_inference_server
              |
              +---- gRPC ----> multimodal_inference_server
              |                   (VLM 推理 · 帧 coordinator)
              |
              +== shared memory (数据面) + gRPC (控制面) ==> 帧推理链路
```

模型推理默认以独立进程和 protobuf/gRPC 协议作为复用边界。关键帧传输采用共享内存数据面 + gRPC 控制面分离：大体积帧走共享内存零拷贝，grant/revoke/epoch 生命周期与故障恢复走 gRPC 控制信令。Gateway、Persona、Session、Memory、Media 与 IPC 也可以通过 CMake target 直接组合到下游源码工程。

### 异步 Turn 时序

```text
Session admission / deferred lane
  -> User Emotion async
  -> L0/L3 Memory Lookup async
  -> Prompt Building
  -> LLM async
  -> AI Emotion async
  -> Memory Admission async
  -> Session commit
  -> HTTP/WebSocket completion
```

Deferred lane 从 admission 持有到 commit，保证同一 Session 不乱序；Emotion、Memory、LLM 等待期间 worker 会返回池中处理其他 Session。CUDA 服务的 health check 只证明进程存活，生产 readiness 还必须完成真实推理 warm-up。

### 基础 Server

| Target | 当前职责 |
| --- | --- |
| `agent_gateway_server` | HTTP/WebSocket Gateway，组合 Persona、认证、记忆、文档、Skill 与静态前端 |
| `emotion_inference_server` | ONNX BERT 情绪推理 gRPC Server |
| `multimodal_inference_server` | BERT + llama.cpp/mtmd VLM gRPC Server |
| `persona_gateway_e2e_server` | 用于手动集成验证的 Gateway E2E Server |

### 可复用 CMake 目标

下游项目使用 `add_subdirectory()` 时可以链接稳定别名：

```cmake
add_subdirectory(path/to/AgentLoom)

target_link_libraries(my_agent PRIVATE
    AgentLoom::core
    AgentLoom::runtime
    AgentLoom::gateway_foundation
    AgentLoom::persona_interaction
    AgentLoom::gateway_routing
)
```

`AgentLoom::gateway` 是上述三项通用 Gateway 组件的兼容聚合 target，不会引入 JWT/Cookie、Document、Classroom、参考 Route 或参考 Server。参考产品需要完整参考 Gateway 时，可在启用 `AGENTLOOM_BUILD_REFERENCE_GATEWAY` 后额外链接 `AgentLoom::reference_gateway`。

当前公开别名包括 `core`、`net`、`tls`、`http_client`、`config`、`storage`、`vector_storage`、`vector`、`semantic_cache`、`memory`、`document`、`conversation`、`llm`、`models`、`cache`、`ipc`、`media_inference`、`media`、`runtime`、`gateway_foundation`、`persona_interaction`、`gateway_routing`、`gateway`、`reference_gateway`（仅启用参考 Gateway 时）和 `service`。

也可以安装静态库、头文件和 CMake package 后通过 `find_package()` 复用：

```powershell
cmake --install build/x64-Release `
  --config Release --prefix build/agentloom-package
```

```cmake
find_package(AgentLoom CONFIG REQUIRED)

target_link_libraries(my_agent PRIVATE
    AgentLoom::core
    AgentLoom::config
    AgentLoom::runtime
    AgentLoom::gateway_foundation
    AgentLoom::persona_interaction
    AgentLoom::gateway_routing
)
```

消费端将安装前缀加入 `CMAKE_PREFIX_PATH`，公共头文件使用 `#include <AgentLoom/core/result.h>` 形式。安装包会从自身前缀定位 SQLite、ONNX Runtime、Faiss、Eigen 和 tokenizer 依赖，并通过标准 CMake package 查找 Boost、gRPC、Protobuf、OpenSSL 等依赖；消费端不需要引用 AgentLoom 的源码树或构建树。

静态注册的 ConfigSection 需要保留整个静态库，可使用安装包提供的跨平台辅助函数：

```cmake
agentloom_link_whole_archive(my_agent AgentLoom::config)
agentloom_link_whole_archive(my_agent my_config_sections)
```

只需要部分配置 section 的程序可用 `ConfigSectionSelection::Only({"llm", "gateway"})` 选择性加载和校验，未选中的服务器专用 section 不会施加参数约束。静态库仍要求消费端使用兼容的编译器、C++ Runtime 和第三方依赖 ABI；完整依赖定位与覆盖变量见 [扩展 AgentLoom](docs/runtime/EXTENDING_AGENTLOOM.md)。

## 构建

### 依赖

- Visual Studio 2026/v145（Windows），或 GCC 11+/Clang 14+（Linux）
- CMake 3.20+
- vcpkg manifest 依赖：gRPC、Protobuf、OpenSSL、spdlog、Redis clients、Boost.Asio/Redis/Interprocess、libzip、pugixml、nlohmann/json；测试另需 GTest
- 预编译/外部依赖：ONNX Runtime、llama.cpp（含 mtmd）、OpenCV、SQLite、Faiss、Eigen、MKL 和 HuggingFace Tokenizers C API
- **工具链说明**：Windows 本地构建必须保持 CMake generator、MSVC 工具集、CRT 配置和 vcpkg/预编译依赖 ABI 一致。若依赖由 v145 构建，宿主也必须使用 VS2026/v145；ABI guard 会拒绝已知的 Debug/Release CRT 冲突，正式构建可用 `AGENT_LLAMA_STRICT_TOOLSET_ABI=ON` 强制工具集一致。

仓库的 `deps/` 与 `vcpkg_installed/` 是本地依赖目录，不随源码分发。依赖准备脚本会下载并校验 Core/SDK 所需的 ONNX Runtime、SQLite、Boost、Eigen、Faiss、MKL 等输入；llama.cpp 和 GStreamer 是按需提供的外部 SDK。完整贡献者流程见 [`CONTRIBUTE.md`](CONTRIBUTE.md)。

### Windows

VS2026/v145 必须使用支持 `Visual Studio 18 2026` generator 的 CMake。配置前先用 `cmake --version` 和 `cmake --help` 核对 PATH。推荐使用仓库脚本构建 Core/SDK Release：

```powershell
.\windows\scripts\prepare_deps.ps1
.\windows\scripts\configure.ps1 `
  -BuildDir build\x64-Release-All-v145 `
  -Tests `
  -Generator "Visual Studio 18 2026" `
  -Triplet x64-windows-release

.\windows\scripts\build.ps1 `
  -BuildDir build\x64-Release-All-v145 `
  -Config Release

.\windows\scripts\test.ps1 `
  -BuildDir build\x64-Release-All-v145 `
  -Label ci `
  -Exclude "ReloadBatchCycle|RedisV2Batches"
```

Windows CI 使用同一套 Core/SDK Release 开关：`AGENTLOOM_BUILD_LOCAL_LLM=OFF`、
`AGENTLOOM_BUILD_MEDIA=OFF`。启用 Media、Local LLM 或完整多模态推理时，参见
[`windows/README.md`](windows/README.md) 和 [`CONTRIBUTE.md`](CONTRIBUTE.md)。

安装 SDK 并验证下游 consumer：

```powershell
cmake --install build\x64-Release-All-v145 `
  --config Release --prefix build\agentloom-install
.\windows\scripts\verify_package.ps1 `
  -BuildDir build\x64-Release-All-v145 `
  -InstallDir build\agentloom-install `
  -ConsumerBuildDir build\windows-package-consumer `
  -Generator "Visual Studio 18 2026"
```

AgentLoom 会从 `LLAMA_CPP_BUILD/CMakeCache.txt` 推断预编译 llama.cpp 的配置。多配置 VS 工程在构建 `agent_models` 前执行 ABI guard：Release、RelWithDebInfo 和 MinSizeRel 归为 Release CRT，Debug 归为 Debug CRT；两侧类别不同会直接终止构建，而不是继续链接可能崩溃的 runtime。

若外部 llama.cpp 构建目录没有 `CMakeCache.txt`，可显式指定：

```powershell
-DLLAMA_CPP_PREBUILT_CONFIG=Release
```

MSVC 工具集目录版本不一致默认给出 CMake warning。需要在 CI 或正式发布构建中强制一致时启用：

```powershell
-DAGENT_LLAMA_STRICT_TOOLSET_ABI=ON
```

#### Windows HTTPS CA bundle

AgentLoom 的 HTTPS 客户端使用 OpenSSL。Windows 上的 vcpkg OpenSSL 不保证自动读取 Windows Certificate Store，因此访问公网云 API 时应显式携带 Mozilla CA bundle；这是一组公开的服务端信任根，不是客户端证书，也不包含私钥。

仓库提供下载和 SHA-256 校验脚本。脚本通过 Windows 自身的 HTTPS 信任链下载 curl 官方发布的 Mozilla CA Extract：

```powershell
.\tools\update_mozilla_ca_bundle.ps1
```

默认输出到 `config/certs/mozilla-ca-bundle.pem`。Gateway 配置如下，路径相对于配置文件目录：

```json
{
  "llm": {
    "ca_bundle_path": "certs/mozilla-ca-bundle.pem",
    "disable_tls_verify_on_windows": false
  }
}
```

Windows 上启用云 LLM 且保持证书校验时，`ca_bundle_path` 是必需项；缺失或不可读会在客户端初始化阶段失败，不再自动退化为 `verify_none`。部署包应携带该 PEM，并按固定发布周期或安全公告更新。Linux 未配置该字段时仍使用系统 CA trust store。

### Linux / WSL2

默认脚本构建 Core/SDK Release、CPU Runtime、Gateway、Emotion Server 和测试；Local LLM 与 Media 默认关闭：

```bash
bash linux/scripts/bootstrap_toolchain.sh
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh
bash linux/scripts/build.sh
bash linux/scripts/check_artifacts.sh
bash linux/scripts/test.sh

# 可选：启用 VLM inference target
bash linux/scripts/configure.sh --inference
bash linux/scripts/build.sh --inference
```

Linux 使用独立的 `build/linux-vcpkg-installed`，不会复用或写入 Windows 的仓库根 `vcpkg_installed/`。
`linux/scripts/test.sh` 默认还会安装 SDK 并构建独立 package consumer；仅运行 CTest 时传入 `--skip-package`。

安装目录也可以单独生成：

```bash
bash linux/scripts/verify_package.sh
```

## 配置与启动

配置系统使用 JSON section，并允许 CLI 覆盖。实际字段以 `src/config/sections/` 和公开样例为准：

| 进程 | 配置样例 |
| --- | --- |
| 通用 ConfigSection 字段参考 | [`config.example.json`](config.example.json) |
| Gateway | [`config/agent_gateway.example.json`](config/agent_gateway.example.json) |
| Multimodal inference | [`config/server.example.json`](config/server.example.json) |
| Container emotion inference | [`config/emotion.container.example.json`](config/emotion.container.example.json) |

API Key 和认证 token 应通过环境变量或本地文件注入，不要写入受 Git 跟踪的 JSON。模型路径同理使用本地配置；`config/e2e_test.json` 和 `.clangd` 已忽略，仓库分别提供 example 文件。

```powershell
# Gateway：当前入口同时使用位置参数定位工作目录，并由 --config 加载统一配置
build\x64-Release\Release\agent_gateway_server.exe `
  config\agent_gateway.example.json `
  --config config\agent_gateway.example.json `
  --no-stdin-stop

# Multimodal gRPC inference
build\x64-Release\Release\multimodal_inference_server.exe `
  --config config\server.example.json

# Emotion gRPC inference；按需覆盖模型和端口
build\x64-Release\Release\emotion_inference_server.exe `
  --config config\emotion.container.example.json
```

VLM 缓存支持三种复用顺序：

- `result_vector`：Exact Result Cache → Vector Cache → Prompt KV/Fresh，兼容原有最低延迟策略。
- `prompt_kv_vector`：Prompt KV → Vector Cache → Fresh。该模式禁止 Exact Result Cache 的直接命中与 stale fallback；VLMCache 仅作为 Vector Index 的结果载荷仓库。
- `tiered`：Exact KV（encoded hash/完整视觉 token embedding，可跨 session）→ 同 session Near KV → Exact Result Cache → 同 session Vector Cache → 跨 session Near KV → 跨 session Vector Cache → Fresh。所有近似缓存均优先同流，跨流 Near KV 必须使用不低于同流的相似度阈值。

连续视觉上下文希望保留重新生成能力时，可使用：

```powershell
build\x64-Release\Release\multimodal_inference_server.exe `
  --config config\e2e_test.json `
  --vlm-cache-reuse-policy prompt_kv_vector `
  --vlm-vector-cache-enabled `
  --vlm-prompt-kv-cache-enabled `
  --vlm-prompt-kv-backend memory `
  --vlm-prompt-kv-max-mb 512
```

`prompt_kv_vector` 要求 VLM cache、Vector Cache 和 Prompt KV Cache 同时启用；配置不完整时服务会在启动阶段直接报错，避免静默退化到 Exact Result Cache。

完整分级模式还需启用 Near KV：

```powershell
build\x64-Release\Release\multimodal_inference_server.exe `
  --config config\e2e_test.json `
  --vlm-cache-reuse-policy tiered `
  --vlm-vector-cache-enabled `
  --vlm-prompt-kv-cache-enabled `
  --vlm-prompt-kv-near-enabled `
  --vlm-prompt-kv-near-same-session-min-cosine 0.99 `
  --vlm-prompt-kv-near-same-session-min-token-mean 0.99 `
  --vlm-prompt-kv-near-same-session-min-token-p05 0.95 `
  --vlm-prompt-kv-near-same-session-max-relative-l2 0.15 `
  --vlm-prompt-kv-near-cross-session-min-cosine 0.995 `
  --vlm-prompt-kv-near-cross-session-min-token-mean 0.995 `
  --vlm-prompt-kv-near-cross-session-min-token-p05 0.98 `
  --vlm-prompt-kv-near-cross-session-max-relative-l2 0.10
```

`tiered` 保留 Exact Result Cache，用于确定性结果复用和降低重复生成导致的幻觉波动。Exact KV key 不包含 `session_id`；Near KV 与 Vector Cache 保存来源 session，并优先选择同 session 候选。Near KV 使用完整视觉 token embedding 的组合 gate：全局 cosine、per-token cosine 均值、per-token cosine P05 和 relative L2 必须同时满足阈值；`token min` 与 `max abs error` 仅作为诊断指标，避免少数边缘 patch 阻断轻微编解码失真下的安全复用。跨 session 的四项 gate 必须不宽松于同 session。

公开样例包含占位模型路径，运行前必须改为本机文件。Gateway 样例默认要求 `AGENT_LLM_API_KEY`；认证、Redis、embedding、emotion analyzer、L0/L3 memory 和 document cache 都可以按部署环境配置。

## 模块结构

| 目录 | 职责 |
| --- | --- |
| `src/core` | `Status`/`Result`、RAII、内存/对象/线程池、并发队列、keyed serial executor 与 task group |
| `src/net` | HTTP/WebSocket Server、TLS、HTTP client、连接管理和背压 |
| `src/config` | JSON/CLI section registry、配置解析与跨 section 校验 |
| `src/models` | ONNX Runtime、llama.cpp/mtmd、runner pool 和模型生命周期 |
| `src/service` | Persona/Session Runtime、Gateway routes、调度与 inference service |
| `src/semantic_cache` | Redis 连接池、L0 adapter、缓存策略和 context risk detector |
| `src/storage` / `src/vector` | SQLite、向量元数据、embedding pipeline 与 Exact/Faiss index |
| `src/memory` / `src/document` | L3 压缩记忆、OOXML 文档分析与缓存 |
| `src/conversation` | 连续话轮数据模型、上下文化 embedding 输入与有界动态规划分块 |
| `src/media` / `src/ipc` | WebRTC/GStreamer、帧编码抽样、共享内存数据面与 gRPC 控制面、有序准入、磁盘 spool 回放与 VLM coordination |
| `src/server` | Gateway 与 gRPC 进程入口、日志、状态映射和运行时统计 |

## 测试

当前 CMake 测试覆盖 core、TLS/HTTP、异步 LLM/Emotion、配置、SQLite、向量检索与 batch coordinator、语义缓存、文档、记忆、Media/IPC、Persona/Gateway 和 gRPC 边界，并包含跨进程 E2E 与独立 benchmark target。测试数量随功能演进，不在 README 固定硬编码。

```bash
bash linux/scripts/configure.sh
bash linux/scripts/build.sh
bash linux/scripts/test.sh
```

## 扩展边界

| 层 | 推荐归属 | 内容 |
| --- | --- | --- |
| 领域业务 | 下游项目 | Persona、Prompt、领域评估、数据集与产品编排 |
| Runtime 热路径 | AgentLoom 库 | Session、缓存、向量检索、记忆、Media 与 IPC |
| 服务入口 | AgentLoom 基础 Server | HTTP/WebSocket/WebRTC、gRPC、认证、配置与生命周期 |
| 模型后端 | 独立进程/外部服务 | BERT、VLM、vLLM 或 OpenAI-compatible backend |

业务扩展优先通过 `I...` 接口注入。AgentLoom 提供基础训练/会话报告和 `IReportEvaluator` 扩展点，不包含特定学校、组织或商业项目的指标、权重与实现。

## 文档

完整文档目录见 [docs/README.md](docs/README.md)。建议从以下内容开始：

- [架构总览](docs/architecture/ARCHITECTURE_VISUAL.md)
- [对话缓存与推理策略](docs/data/CONVERSATION_CACHE_AND_INFERENCE_STRATEGY.md)
- [配置系统](docs/runtime/CONFIG_SYSTEM.md)
- [部署指南](docs/runtime/DEPLOYMENT.md)
- [Frontend/Backend API 协议](docs/gateway/FRONTEND_BACKEND_API_PROTOCOL.md)
- [扩展 AgentLoom](docs/runtime/EXTENDING_AGENTLOOM.md)
- [安全工程规范](docs/security/SECURITY_ENGINEERING_STANDARD.md)

## 贡献与许可

提交更改前请运行相关测试、同步受影响文档，可以参考 [AGENTS.md](AGENTS.md) 的工程约定。安全问题请参阅 [SECURITY.md](SECURITY.md)，不要在公开 Issue 中披露凭据或未修复漏洞。

AgentLoom 使用 [MIT License](LICENSE)。
