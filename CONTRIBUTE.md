# AgentLoom 构建与贡献指南

本文档面向第一次克隆仓库的开发者，说明如何准备依赖、构建普通可执行文件、运行测试、安装 AgentLoom SDK，并验证下游 CMake consumer。构建脚本以 Release 配置为基础；Debug 构建只适合局部调试，不属于 CI 和发布验证路径。

## 1. 构建变体

### Core/SDK Release

这是 Linux CI、Windows CI 和本地默认脚本使用的配置：

```text
BERT_BUILD_TESTS=ON
BERT_BUILD_MULTIMODAL_INFERENCE_SERVER=OFF
BERT_USE_ONNXRUNTIME_GPU=OFF
AGENTLOOM_BUILD_LOCAL_LLM=OFF
AGENTLOOM_BUILD_MEDIA=OFF
```

它会构建 SDK、Gateway、Emotion Server、文档/向量/语义缓存服务和核心测试，不要求 llama.cpp、OpenCV 或 GStreamer。

### Media

Media 变体需要 OpenCV，并且相关 GStreamer 目标需要可用的 GStreamer SDK。Windows 通过导入库动态链接 GStreamer；不要把 GStreamer 的 `bin` 目录整体复制进发布包。运行时由 MSI 安装、系统 PATH 和插件路径提供。

### Local LLM / 多模态推理

Local LLM 需要一个与当前编译器、配置和 CRT ABI 匹配的 llama.cpp/mtmd 构建目录。多模态推理同时要求 Local LLM 和 Media。llama.cpp 不是由 AgentLoom 的默认 Core/SDK 流程自动编译的外部输入。

## 2. Windows 环境

### 工具链要求

- Visual Studio 2026/v145，包含 C++ 桌面开发组件；也可以使用仓库明确支持的 VS2022/v143 组合。
- 支持目标 generator 的 CMake。VS2026 使用 `Visual Studio 18 2026`，CMake 版本必须能在 `cmake --help` 中列出该 generator。
- PowerShell 5.1 或更高版本。
- Python 3.12+、Rust stable、Git。

Windows 构建必须保持 CMake generator、MSVC 工具集、vcpkg triplet 和预编译依赖 ABI 一致。不要把 v143 依赖放进 v145 构建目录，也不要复用不同工具集的 CMake cache。

### 从克隆到 Core/SDK 构建

在仓库根目录执行：

```powershell
# 1. 下载并准备 Core/SDK 依赖、Faiss、MKL 和 SQLite runtime
.\windows\scripts\prepare_deps.ps1

# 2. 可选的显式依赖检查
.\windows\scripts\validate_dependencies.ps1

# 3. Release + 测试配置。默认关闭 Local LLM 和 Media
.\windows\scripts\configure.ps1 `
  -BuildDir build\x64-Release-Tests-v145 `
  -Tests `
  -Generator "Visual Studio 18 2026" `
  -Triplet x64-windows-release

# 4. 构建默认脚本声明的全部目标
.\windows\scripts\build.ps1 `
  -BuildDir build\x64-Release-Tests-v145 `
  -Config Release

# 5. 运行 CTest；默认使用构建目录内全部已注册测试
.\windows\scripts\test.ps1 `
  -BuildDir build\x64-Release-Tests-v145 `
  -Config Release
```

Windows `test.ps1` 默认会在 CTest 通过后调用 `verify_package.ps1`，完成 SDK install 和独立 consumer 验证。只运行 CTest 时显式传入 `-SkipPackage`；也可以单独执行：

```powershell
.\windows\scripts\verify_package.ps1 `
  -BuildDir build\x64-Release-Tests-v145 `
  -Generator "Visual Studio 18 2026"
```

CI 为了不依赖外部 Redis 服务，会使用测试标签并排除 Redis 集成测试：

```powershell
.\windows\scripts\test.ps1 `
  -BuildDir build\gha-windows-tests `
  -Config Release `
  -Label ci `
  -Exclude "ReloadBatchCycle|RedisV2Batches"
```

Redis 集成测试自身会先执行 PING/PONG 探测。Redis 未运行时测试会通过 `GTEST_SKIP()` 跳过，不应把服务未启动误判成编译失败。

### Windows 全功能Target

准备 Media 依赖：

```powershell
.\windows\scripts\prepare_deps.ps1 -Media
```

启用 Media：

```powershell
.\windows\scripts\configure.ps1 `
  -BuildDir build\x64-Release-Media-v145 `
  -Tests `
  -Media `
  -GStreamerRoot "<gstreamer-msvc-x64-root>" `
  -Generator "Visual Studio 18 2026"
```

启用 Local LLM：

```powershell
.\windows\scripts\configure.ps1 `
  -BuildDir build\x64-Release-Llm-v145 `
  -Tests `
  -LocalLlm `
  -LlamaCppRoot "<llama.cpp-source-root>" `
  -LlamaCppBuild "<llama.cpp-build-root>" `
  -Generator "Visual Studio 18 2026"
```

启用完整多模态推理：

```powershell
.\windows\scripts\configure.ps1 `
  -BuildDir build\x64-Release-Inference-v145 `
  -Tests `
  -Inference `
  -GPU `
  -LlamaCppRoot "<llama.cpp-source-root>" `
  -LlamaCppBuild "<llama.cpp-build-root>" `
  -GStreamerRoot "<gstreamer-msvc-x64-root>" `
  -Generator "Visual Studio 18 2026"
```

`-GStreamerRoot` 应指向包含 `bin\gst-inspect-1.0.exe`、`include\` 和 `lib\` 的 SDK 根目录。省略它时，CMake 会尝试常见安装路径和 `GSTREAMER_1_0_ROOT_MSVC_X86_64` 环境变量。

### Windows SDK 安装与 consumer 验证

安装 SDK：

```powershell
cmake --install build\x64-Release-Tests-v145 `
  --config Release `
  --prefix build\windows-agentloom-install
```

安装目录包含：

- `include\AgentLoom\` 公共头文件；
- `lib\*.lib` AgentLoom 静态库、Faiss 和 ONNX Runtime import library；
- `bin\onnxruntime.dll`、`bin\sqlite3.dll`、`bin\faiss.dll`、`bin\mkl_rt.2.dll`；
- `lib\cmake\AgentLoom\AgentLoomConfig.cmake` 和 target export 文件。

验证独立 consumer：

```powershell
cmake `
  -DAGENTLOOM_SOURCE_DIR="$PWD" `
  -DAGENTLOOM_PACKAGE_BUILD_DIR="$PWD\build\x64-Release-Tests-v145" `
  -DAGENTLOOM_INSTALL_PREFIX="$PWD\build\windows-agentloom-install" `
  -DAGENTLOOM_CONSUMER_BUILD_DIR="$PWD\build\windows-package-consumer" `
  -DAGENTLOOM_CONSUMER_GENERATOR="Visual Studio 18 2026" `
  -DAGENTLOOM_CONSUMER_BUILD_TYPE=Release `
  -DAGENTLOOM_CONSUMER_TOOLCHAIN_FILE="$PWD\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -DAGENTLOOM_CONSUMER_TRIPLET=x64-windows-release `
  -DAGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR="$PWD\vcpkg_installed" `
  -DAGENTLOOM_CONSUMER_BOOST_ROOT="$PWD\deps\boost_1_85_0" `
  -P cmake\AgentLoomPackageConsumerTest.cmake
```

consumer 脚本会重新构建 `agentloom_sdk`，执行 `cmake --install`，扫描安装的 CMake 文件确保没有泄漏源码树或构建树路径，然后配置、构建并运行 `tests/package/consumer`。Windows release-only triplet 的实际安装目录可能是 `vcpkg_installed\x64-windows`；consumer 验证会自动从 `x64-windows-release` 回退到该目录。

## 3. Linux / WSL2 环境

### 工具链和 Core/SDK 构建

```bash
# 1. 安装编译工具、Rust stable 和私有 Python 环境
bash linux/scripts/bootstrap_toolchain.sh

# 2. 下载 ONNX Runtime、SQLite、Boost、Eigen、Faiss，并安装固定 baseline 的 vcpkg
bash linux/scripts/prepare_deps.sh

# 3. Release + Core/SDK 配置，Local LLM 和 Media 默认关闭
bash linux/scripts/configure.sh

# 4. 构建 SDK、服务、测试和 E2E 目标
bash linux/scripts/build.sh

# 5. 检查普通 ELF 可执行文件和运行时依赖
bash linux/scripts/check_artifacts.sh

# 6. 运行测试并验证安装包 consumer
bash linux/scripts/test.sh
```

Linux 使用 `build/linux-vcpkg-installed`，Windows 使用仓库根 `vcpkg_installed`。两者不可互换。Linux 默认使用 `x64-linux-release` 和 `build/vcpkg-binary-cache`，Windows 使用 `x64-windows-release` 和 `build/vcpkg-binary-cache-windows`。

### Linux Media 和 VLM

Media：

```bash
bash linux/scripts/bootstrap_toolchain.sh --media
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh --media
bash linux/scripts/build.sh
```

VLM/多模态推理：

```bash
export LLAMA_CPP_CUDA_URL="<llama.cpp-cuda-binary-url>"
bash linux/scripts/bootstrap_toolchain.sh --inference
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh --inference
bash linux/scripts/build.sh --inference
bash linux/scripts/check_artifacts.sh --inference
```

CUDA binary 至少需要 `libllama.so`、`libmtmd.so`、`libggml.so`、`libggml-base.so` 和 `libggml-cuda.so`。脚本使用预编译 binary，不会替开发者安装或调用 CUDA toolkit。

## 4. 贡献前的完整检查

提交构建系统、CMake、依赖脚本或公共 SDK 修改前，至少完成对应平台的以下检查：

```text
1. 版本化输入校验通过；
2. 依赖脚本在已有依赖和空依赖目录下都能正确处理；
3. Release configure 成功；
4. 普通目标和测试目标构建成功；
5. CTest 通过，外部服务测试只按约定跳过；
6. cmake --install 成功；
7. 安装目录包含必要 DLL/SO，未批量复制 GStreamer 等系统 SDK；
8. 独立 package consumer 能 find_package、编译并运行；
9. 生成的 CMake package 不包含本机源码路径和构建路径；
10. 文档中的命令与当前脚本参数保持一致。
```

Windows 可运行：

```powershell
.\.github\scripts\validate-build-inputs.ps1
.\windows\scripts\prepare_deps.ps1 -SkipVcpkg
.\windows\scripts\validate_dependencies.ps1 -SkipVcpkg
```

Linux 可运行：

```bash
bash .github/scripts/validate-build-inputs.sh
bash -n linux/scripts/*.sh
python3 -m py_compile .github/scripts/extract-conda-package.py
```

## 5. 外部依赖和缓存原则

- `deps/`、`vcpkg/`、`vcpkg_installed/` 和 build 输出不提交到 Git。
- vcpkg 版本来自 `vcpkg.json` 的 `builtin-baseline`，不要用未锁定的最新 master 替代它。
- Faiss/MKL `.conda` 归档使用固定 URL 和 SHA-256；归档先缓存，解包目录按 CMake 约定规范化。
- GStreamer 使用 import library 动态链接，运行时由系统安装提供；不要复制整个 `bin`/plugin 树。
- 改变依赖版本、URL、哈希或 triplet 后，应递增相应 CI archive cache version，并重新验证 SDK consumer。

## 6. 常见问题

### 找不到 Visual Studio 18 2026 generator

```powershell
cmake --version
cmake --help | Select-String "Visual Studio 18 2026"
```

如果旧版 CMake 位于 PATH 前面，先将支持 VS2026 的 CMake 放到 PATH 前面，再删除旧 build 目录重新配置。

### MSVC STL/ABI 链接错误

确认项目、vcpkg 二进制包和外部 llama.cpp 使用同一个 MSVC 工具集。不要复用 v143 的 build 目录构建 v145；使用独立目录，例如 `build\x64-Release-Tests-v145`。

### CMake 找不到 Protobuf 或其他 vcpkg 包

确认 consumer 使用了正确的 toolchain file、`VCPKG_INSTALLED_DIR` 和 release triplet。release-only triplet 的物理目录可能去掉 `-release` 后缀。

### 构建包突然变成数 GB

检查是否把 GStreamer 或 OpenCV 的整个安装目录复制到了输出目录。SDK 安装只应复制 AgentLoom 必需的 import library、runtime DLL 和公共头文件。

### Redis 测试被跳过

这是预期行为：先启动 Redis，再单独运行被排除的测试；如果 Redis 没有运行，测试会在 PING/PONG 探测失败后跳过。

## 7. 代码约定

业务代码应继续遵守仓库 `AGENTS.md` 中的 RAII、`core::Status`、日志、UTF-8、跨平台和测试约定。构建系统修改也应提供脚本级验证和至少一次对应平台的 Release 构建证据。
