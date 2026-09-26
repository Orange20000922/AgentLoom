# AgentLoom

> A composable C++20 agent runtime, gateway, and multimodal inference infrastructure project

[中文](README.md) | English

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green.svg)](https://cmake.org/)
[![Tests](https://img.shields.io/badge/CTest-E2E%20%2B%20unit-brightgreen.svg)](#tests)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

## Overview

AgentLoom is a C++20 server-side runtime for agent systems. It packages an HTTP/WebSocket gateway, persona and session runtime, semantic cache, long-term memory, document analysis, real-time media paths, and BERT/VLM inference as composable libraries and basic server implementations.

The project originated from an educational agent backend, but the open-source boundary is domain-neutral. AgentLoom owns reusable runtime code, protocols, and server foundations. Downstream projects own personas, prompts, domain evaluation, datasets, and product orchestration. Domain report evaluation is injected through `IReportEvaluator`; no organization-specific evaluator is included.

The current version is `0.1.0` and remains under active development. It is suitable for downstream development, system integration, and production-oriented engineering validation, but absolute ABI stability is not guaranteed.

## Core Features

- **Composable architecture**: Core capabilities exposed as CMake targets—integrate as libraries or deploy as standalone servers.
- **Fully asynchronous chat hot path**: Strict same-session ordering with continuation-based user/AI emotion, L0 lookup, cloud LLM, and memory store; external waits release business workers while different sessions progress concurrently.
- **Real-time multimodal sensing**: Complete pipeline from browser camera WebRTC input, GStreamer decoding, OpenCV dynamic frame sampling, to VLM inference.
- **Inference cost optimization**: Semantic cache, prompt KV cache, and VLM result cache coordinate to reduce redundant inference overhead; VRAM guard supports low-memory scenarios and OOM degradation.
- **Clear process boundaries**: BERT and VLM inference as independent gRPC processes supporting containerization and independent scaling; gateway connects via gRPC or shared-memory IPC.
- **Production-grade engineering**: Maintained unit, cross-process E2E, and capacity tests cover concurrency, error recovery, resource cleanup, and structured overload rejection; unified error model via `core::Status`/`Result`.

## Implemented Capabilities

- **Gateway runtime**: Unified HTTP/WebSocket entry, JWT/cookie authentication, static file hosting, request filtering, backpressure, and runtime maintenance tasks.
- **Agent runtime**: Persona, session, skill session, multi-persona scheduling, deferred session lanes, asynchronous emotion/LLM/memory continuations, proactive speech, trace, and emotion state persistence.
- **Model services**: ONNX Runtime BERT emotion inference, and llama.cpp/mtmd-based streaming and synchronous VLM inference.
- **Emotion fusion**: BERT backbone combined with keyword evidence, updating V-A state through configurable fusion head, confidence, and margin gate.
- **Memory and cache**: Redis/SQLite L0 memory, L3 compressed memory, Exact/Faiss retrieval, L0 embedding micro-batches, asynchronous lookup/store, VLM result cache, and image-prefix prompt KV cache based on llama.cpp sequence state.
- **Document pipeline**: DOCX/PPTX OOXML extraction, managed file storage, chunk analysis, metadata, and LLM/semantic caches.
- **Real-time multimodal input**: WebRTC signaling (offer/answer/ICE/resume), GStreamer `webrtcbin` media pipeline, OpenCV dynamic frame sampling (MOG2/histogram/edge/EMA/cooldown), key frame JPEG/PNG encoding (NVIDIA/VAAPI/D3D11/QSV hardware acceleration with software fallback).
- **Frame inference pipeline**: Shared-memory frame IPC (MPMC sequence ring, RAII claim, epoch recovery) + gRPC IPC control plane (grant/revoke/probe, lease coordination, cross-process fault recovery) form a data-plane/control-plane-separated transport layer; upper layers compose ordered admission, mmap disk spool overflow replay, private backlog, VLM coordinator, and execution-level sealed aggregation (running → sealing → replay → aggregate) into a controlled key-frame inference lifecycle.
- **Infrastructure**: `core::Status`/`Result`, RAII handle wrappers, memory pools, thread pools, object pools, thread-safe queues, keyed serial executor (per-key serialization, session affinity ordering), task group (structured concurrency), TLS context, and concurrent HTTP client.

## Runtime Boundaries

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
              |                   (VLM inference · frame coordinator)
              |
              +== shared memory (data plane) + gRPC (control plane) ==> frame inference pipeline
```

Model inference defaults to independent processes with protobuf/gRPC contracts as the reuse boundary. Key frame transmission adopts separated shared-memory data plane + gRPC control plane: large frames use shared memory zero-copy, while grant/revoke/epoch lifecycle and fault recovery use gRPC control signaling. Gateway, persona, session, memory, media, and IPC can also be directly composed into downstream source builds via CMake targets.

### Asynchronous Turn Sequence

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

The deferred lane remains held until commit to preserve same-session order. Emotion, memory, and LLM waits return workers to the pool so other sessions can progress. A CUDA service health check proves process availability only; production readiness also requires a real inference warm-up.

### Basic Servers

| Target | Current responsibility |
| --- | --- |
| `agent_gateway_server` | HTTP/WebSocket gateway, combining persona, auth, memory, documents, skills, and static frontend |
| `emotion_inference_server` | ONNX BERT emotion inference gRPC server |
| `multimodal_inference_server` | BERT + llama.cpp/mtmd VLM gRPC server |
| `persona_gateway_e2e_server` | Gateway E2E server for manual integration verification |

## Reusable CMake Targets

Downstream projects using `add_subdirectory()` can link stable build-tree aliases:

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

`AgentLoom::gateway` is a compatibility aggregate of the three common Gateway components above. It does not bring in JWT/cookies, Document, Classroom, reference routes, or the reference server. Reference-product consumers can additionally link `AgentLoom::reference_gateway` when `AGENTLOOM_BUILD_REFERENCE_GATEWAY` is enabled.

Available aliases include `core`, `net`, `tls`, `http_client`, `config`, `storage`, `vector_storage`, `vector`, `semantic_cache`, `memory`, `document`, `conversation`, `llm`, `models`, `cache`, `ipc`, `media_inference`, `media`, `runtime`, `gateway_foundation`, `persona_interaction`, `gateway_routing`, `gateway`, `reference_gateway` (when the reference Gateway is enabled), `grpc_runtime`, and `service`.

The static libraries, public and generated headers, runtime dependencies, and CMake config package can also be installed for independent downstream consumption:

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

Add the installation prefix to `CMAKE_PREFIX_PATH`. SQLite, ONNX Runtime, Faiss, Eigen, and tokenizer artifacts are resolved inside the installation prefix; Boost, gRPC, Protobuf, OpenSSL, and other standard dependencies are discovered as CMake packages. Downstream projects do not need the AgentLoom source or build tree.

For ConfigSection registrars stored in static libraries, retain the full archives with the installed cross-platform helper:

```cmake
agentloom_link_whole_archive(my_agent AgentLoom::config)
agentloom_link_whole_archive(my_agent my_config_sections)
```

Consumers that only need selected configuration sections can load and validate `ConfigSectionSelection::Only({"llm", "gateway"})`; unselected server-specific sections do not impose validation requirements.

## Build

Requirements:

- Visual Studio 2026/v145 on Windows, or GCC 11+/Clang 14+ on Linux
- CMake 3.20+
- vcpkg manifest dependencies: gRPC, Protobuf, OpenSSL, spdlog, Redis clients, Boost.Asio/Redis/Interprocess, libzip, pugixml, and nlohmann/json; GTest for tests
- Prebuilt or external ONNX Runtime, llama.cpp with mtmd, OpenCV, SQLite, Faiss, Eigen, MKL, and HuggingFace Tokenizers C API packages
- **Toolchain note:** Windows builds must align the CMake generator, MSVC toolset, CRT configuration, and vcpkg/prebuilt dependency ABI. If dependencies are built with v145, the host must also use VS2026/v145. The ABI guard rejects known Debug/Release CRT conflicts, and release builds can enable `AGENT_LLAMA_STRICT_TOOLSET_ABI=ON` to require toolset alignment.

The local `deps/` and `vcpkg_installed/` directories are not distributed with the source. The dependency scripts download and verify the Core/SDK inputs, including ONNX Runtime, SQLite, Boost, Eigen, Faiss, and MKL. llama.cpp and GStreamer are optional external SDKs. See [`CONTRIBUTE.md`](CONTRIBUTE.md) for the complete contributor workflow.

### Windows

Use a CMake version that supports the VS2026 generator. Check the executable resolved from PATH with `cmake --version` and `cmake --help`. The recommended Core/SDK Release flow is:

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

Windows CI uses the same Core/SDK switches: `AGENTLOOM_BUILD_LOCAL_LLM=OFF` and
`AGENTLOOM_BUILD_MEDIA=OFF`. See [`windows/README.md`](windows/README.md) and
[`CONTRIBUTE.md`](CONTRIBUTE.md) for Media, Local LLM, and full multimodal configurations.

Install the SDK and validate a downstream consumer:

```powershell
cmake --install build\x64-Release-All-v145 `
  --config Release --prefix build\agentloom-install
.\windows\scripts\verify_package.ps1 `
  -BuildDir build\x64-Release-All-v145 `
  -InstallDir build\agentloom-install `
  -ConsumerBuildDir build\windows-package-consumer `
  -Generator "Visual Studio 18 2026"
```

### Linux / WSL2

The default path builds the Core/SDK Release profile, CPU runtime, gateway, emotion server, and tests. Local LLM and Media are disabled by default.

```bash
bash linux/scripts/bootstrap_toolchain.sh
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh
bash linux/scripts/build.sh
bash linux/scripts/check_artifacts.sh
bash linux/scripts/test.sh

# Optional VLM inference target
bash linux/scripts/configure.sh --inference
bash linux/scripts/build.sh --inference
```

Linux dependencies are installed under `build/linux-vcpkg-installed`, separate from the Windows root `vcpkg_installed/`.
`linux/scripts/test.sh` also installs the SDK and builds an independent package consumer by default; pass `--skip-package` to run only CTest.

To verify the installed package separately:

```bash
bash linux/scripts/verify_package.sh
```

## Configuration and Startup

Configuration uses JSON sections with CLI overrides. Treat `src/config/sections/` and the public examples as the source of truth:

| Process | Example |
| --- | --- |
| Shared ConfigSection field reference | [`config.example.json`](config.example.json) |
| Gateway | [`config/agent_gateway.example.json`](config/agent_gateway.example.json) |
| Multimodal inference | [`config/server.example.json`](config/server.example.json) |
| Container emotion inference | [`config/emotion.container.example.json`](config/emotion.container.example.json) |

Inject API keys and authentication tokens through environment variables or local files. Do not commit them in JSON. Local E2E paths belong in ignored `config/e2e_test.json`; `.clangd.example` is provided for local language-server setup.

```powershell
# The current gateway entry uses the positional path for startup context and
# --config for the shared configuration loader.
build\x64-Release\Release\agent_gateway_server.exe `
  config\agent_gateway.example.json `
  --config config\agent_gateway.example.json `
  --no-stdin-stop

build\x64-Release\Release\multimodal_inference_server.exe `
  --config config\server.example.json

build\x64-Release\Release\emotion_inference_server.exe `
  --config config\emotion.container.example.json
```

Public examples contain placeholder model paths. The gateway example expects `AGENT_LLM_API_KEY` by default. Authentication, Redis, embedding, emotion analysis, L0/L3 memory, and document caches remain deployment-configurable.

## Source Layout

| Path | Responsibility |
| --- | --- |
| `src/core` | `Status`/`Result`, RAII, memory/object/thread pools, concurrent queues, keyed serial executor, and task group |
| `src/net` | HTTP/WebSocket server, TLS, HTTP client, connection management, and backpressure |
| `src/config` | JSON/CLI section registry, parsing, and cross-section validation |
| `src/models` | ONNX Runtime, llama.cpp/mtmd, runner pools, and model lifecycle |
| `src/service` | Persona/session runtime, gateway routes, scheduling, and inference services |
| `src/semantic_cache` | Redis pools, L0 adapters, policies, and context risk detection |
| `src/storage` / `src/vector` | SQLite, vector metadata, embeddings, and Exact/Faiss indexes |
| `src/memory` / `src/document` | L3 compression, OOXML analysis, and document caches |
| `src/media` / `src/ipc` | WebRTC/GStreamer, frame encoding/sampling, shared-memory data plane and gRPC control plane, ordered admission, disk spool replay, and VLM coordination |
| `src/server` | Gateway and gRPC process entries, logging, status mapping, and runtime statistics |

## Tests

CMake tests cover core infrastructure, TLS/HTTP, asynchronous LLM/emotion, configuration, SQLite, vector retrieval and batch coordination, semantic cache, documents, memory, media/IPC, persona/gateway behavior, and gRPC boundaries. Cross-process E2E and standalone benchmark targets are also included. The exact count evolves with the codebase and is not hard-coded in this README.

```bash
bash linux/scripts/configure.sh
bash linux/scripts/build.sh
bash linux/scripts/test.sh
```

## Extension Boundary

| Layer | Recommended owner | Content |
| --- | --- | --- |
| Domain business | Downstream project | Personas, prompts, evaluation, datasets, and product orchestration |
| Runtime hot path | AgentLoom libraries | Sessions, caches, vector retrieval, memory, media, and IPC |
| Service entry | AgentLoom basic servers | HTTP/WebSocket/WebRTC, gRPC, auth, configuration, and lifecycle |
| Model backend | Separate process/service | BERT, VLM, vLLM, or OpenAI-compatible backends |

Business extensions should use `I...` interfaces. AgentLoom provides base session/training reports and the `IReportEvaluator` extension point, but no school-, organization-, or product-specific metrics and weights.

## Documentation

See [docs/README.md](docs/README.md) for the complete categorized index. Recommended entry points:

- [Architecture overview](docs/architecture/ARCHITECTURE_VISUAL.md)
- [Conversation cache and inference strategy](docs/data/CONVERSATION_CACHE_AND_INFERENCE_STRATEGY.md)
- [Configuration system](docs/runtime/CONFIG_SYSTEM.md)
- [Deployment guide](docs/runtime/DEPLOYMENT.md)
- [Frontend/backend API protocol](docs/gateway/FRONTEND_BACKEND_API_PROTOCOL.md)
- [Extending AgentLoom](docs/runtime/EXTENDING_AGENTLOOM.md)
- [Security engineering standard](docs/security/SECURITY_ENGINEERING_STANDARD.md)

## Contributing and License

Run relevant tests, update affected documentation, and follow [AGENTS.md](AGENTS.md) before submitting changes. Report security issues according to [SECURITY.md](SECURITY.md); do not disclose credentials or unresolved vulnerabilities in public issues.

AgentLoom is licensed under the [MIT License](LICENSE).
