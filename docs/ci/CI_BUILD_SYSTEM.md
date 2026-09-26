# CI 与本地构建系统

本文档说明 Linux、Windows 的 Release 构建闭环，以及 CI 与本地脚本之间的对应关系。

## 一致性原则

- Linux CI 与 Windows CI 使用相同的 Core/SDK Release 能力边界、测试标签和 SDK consumer 验证。
- `linux/scripts` 与 `windows/scripts` 使用相同的依赖版本原则和功能开关语义。
- CI 固定使用版本化 vcpkg baseline 和 release-only triplet。
- 本地依赖目录是生成内容；版本、URL、哈希和目录结构由版本化脚本定义。
- Linux 与 Windows 的 vcpkg install root、binary cache 和预编译二进制不得混用。

## 构建配置

### Core/SDK Release

这是 Linux CI、Windows CI 和本地脚本的默认配置：

```cmake
BERT_BUILD_TESTS=ON
BERT_BUILD_MULTIMODAL_INFERENCE_SERVER=OFF
BERT_USE_ONNXRUNTIME_GPU=OFF
AGENTLOOM_BUILD_LOCAL_LLM=OFF
AGENTLOOM_BUILD_MEDIA=OFF
```

该配置构建 SDK、核心服务和 CI 测试，不要求 llama.cpp、OpenCV 或 GStreamer。

### Media

Media 配置额外要求 OpenCV。Windows 的 GStreamer 通过安装 SDK 的导入库动态链接，运行时由系统安装提供 DLL 和插件；构建系统不会把完整 GStreamer 目录复制到产物中。

### Local LLM 与多模态推理

Local LLM 配置使用外部构建好的 llama.cpp。开发者必须提供源码根目录和 ABI 匹配的构建目录。多模态推理同时启用 Local LLM 和 Media。

## 依赖矩阵

| 依赖 | Linux | Windows | Core/SDK 必需 |
|---|---|---|---|
| vcpkg manifest | 固定 baseline | 固定 baseline | 是 |
| ONNX Runtime | 官方 Linux 归档 | 官方 Windows 归档 | 是 |
| SQLite | amalgamation 静态编译 | amalgamation + DLL/DEF | 是 |
| Boost 1.85 | 源码头文件归档 | 源码头文件归档 | 是 |
| Eigen 5.0.1 | 源码归档 | 源码归档 | 是 |
| Faiss | conda-forge 1.10.0 OpenBLAS | PyTorch channel 1.14.1 CPU | 是 |
| BLAS runtime | 系统 OpenBLAS | MKL 2023.1 runtime | 是 |
| OpenCV | 系统开发包 | 官方 Windows 包 | 仅 Media |
| GStreamer | 系统 SDK | MSI/MSVC SDK | 仅相关 Media 目标 |
| llama.cpp | 外部 CUDA binary | 外部兼容构建 | 仅 Local LLM |

Faiss 和 Windows MKL 的 `.conda` 归档通过 `.github/scripts/extract-conda-package.py` 解包。脚本只提取 `pkg-*.tar.zst` payload，并使用安全 tar 过滤器。CI 和本地脚本在使用固定 URL 时同时验证 SHA-256。

## CI 闭环

两个平台的 CI 都执行以下阶段：

1. checkout，并验证版本化构建输入；
2. 检查 runner 磁盘空间；
3. 配置 Python、Rust 和编译器缓存；
4. 验证 HuggingFace Tokenizers Rust FFI；
5. 从 `vcpkg.json` 读取 baseline，准备 release-only vcpkg 依赖；
6. 恢复或下载平台二进制依赖，并验证实际头文件、导入库和 runtime；
7. 配置 Core/SDK Release；
8. 构建 `agentloom_sdk` 和带 `ci` 标签的测试目标；
9. 运行测试，Redis 集成测试在服务不可用时由测试自身执行 PING/PONG 后跳过；
10. 安装 SDK，扫描生成的 CMake package，禁止泄漏源码机路径；
11. 配置、构建并测试独立的 `find_package(AgentLoom)` consumer。

Linux 使用 Ninja 和 ccache；Windows 使用可用的 Visual Studio generator 和 sccache。两者使用各自的平台 cache key 和目录。

## 本地 Core/SDK 构建

### Windows

```powershell
.\windows\scripts\prepare_deps.ps1
.\windows\scripts\configure.ps1 -Tests
.\windows\scripts\build.ps1
.\windows\scripts\test.ps1
```

Media 构建先准备 OpenCV，再显式启用功能：

```powershell
.\windows\scripts\prepare_deps.ps1 -Media
.\windows\scripts\configure.ps1 -Tests -Media -GStreamerRoot "<gstreamer-sdk-root>"
```

多模态推理构建：

```powershell
.\windows\scripts\configure.ps1 `
  -Tests -Inference -GPU `
  -LlamaCppRoot "<llama.cpp-source-root>" `
  -LlamaCppBuild "<llama.cpp-build-root>" `
  -GStreamerRoot "<gstreamer-sdk-root>"
```

### Linux/WSL2

```bash
bash linux/scripts/bootstrap_toolchain.sh
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh
bash linux/scripts/build.sh
bash linux/scripts/test.sh
```

Media 构建：

```bash
bash linux/scripts/bootstrap_toolchain.sh --media
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh --media
```

多模态推理构建需要先设置兼容的 llama.cpp CUDA binary URL：

```bash
export LLAMA_CPP_CUDA_URL="<llama-cuda-binary-url>"
bash linux/scripts/bootstrap_toolchain.sh --inference
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh --inference
bash linux/scripts/build.sh --inference
```

## 版本升级检查

升级外部依赖时应同时完成：

1. 更新 CI 与对应平台本地脚本中的版本和 URL；
2. 更新固定 SHA-256；
3. 调整规范化解包目录和 CMake cache path；
4. 递增对应平台的 dependency archive cache version；
5. 在空依赖目录下验证下载和解包；
6. 运行 Release configure、构建、CI 测试与 SDK consumer；
7. 确认安装产物只包含必要 runtime，不批量复制系统 SDK。
