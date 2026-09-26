# Windows Build Scripts

这些脚本镜像了 `linux/scripts/` 的结构，用于 Windows 平台的本地开发和 CI 构建。

## 前置要求

- **Visual Studio 2022** (v143) 或 **Visual Studio 2026** (v145)
- **CMake 3.20+**（建议使用支持 VS 2026 generator 的版本）
- **PowerShell 5.1+**
- **Rust toolchain**（用于 HuggingFace Tokenizers FFI）
- **Python 3.12+**

## 脚本说明

### `prepare_deps.ps1`
准备和下载外部依赖。

脚本会从 `vcpkg.json` 读取 `builtin-baseline`，在仓库内自动 bootstrap
`vcpkg\vcpkg.exe`；不依赖开发者机器上的固定 vcpkg 路径。

```powershell
# 下载 Core/SDK Release 依赖
.\windows\scripts\prepare_deps.ps1

# 额外准备 Media 所需的 OpenCV
.\windows\scripts\prepare_deps.ps1 -Media

# 跳过 vcpkg 依赖（假设已安装）
.\windows\scripts\prepare_deps.ps1 -SkipVcpkg

# 强制重新下载
.\windows\scripts\prepare_deps.ps1 -Force
```

### `validate_dependencies.ps1`
验证所有依赖是否正确安装。

```powershell
# 快速验证
.\windows\scripts\validate_dependencies.ps1

# 详细验证输出
.\windows\scripts\validate_dependencies.ps1 -Detailed

# 跳过 vcpkg 验证
.\windows\scripts\validate_dependencies.ps1 -SkipVcpkg
```

### `configure.ps1`
配置 CMake 构建。

```powershell
# 默认配置（Core/SDK Release，不含测试）
.\windows\scripts\configure.ps1

# 启用测试
.\windows\scripts\configure.ps1 -Tests

# 自定义构建目录
.\windows\scripts\configure.ps1 -BuildDir build\custom -Tests

# 启用 Local LLM 并指定 llama.cpp 路径
.\windows\scripts\configure.ps1 `
  -LocalLlm `
  -LlamaCppRoot "<llama.cpp-source-root>" `
  -LlamaCppBuild "<llama.cpp-build-root>"

# 指定 GStreamer SDK 路径
.\windows\scripts\configure.ps1 `
  -Media `
  -GStreamerRoot "<gstreamer-sdk-root>" `
  -Tests

# 启用完整 GPU 多模态推理；-Inference 同时启用 Local LLM 和 Media
.\windows\scripts\configure.ps1 `
  -GPU -Inference `
  -LlamaCppRoot "<llama.cpp-source-root>" `
  -LlamaCppBuild "<llama.cpp-build-root>" `
  -GStreamerRoot "<gstreamer-sdk-root>"
```

`-GStreamerRoot` 应指向包含 `bin\gst-inspect-1.0.exe`、`include\` 和 `lib\` 的
GStreamer MSVC x64 安装根目录。省略该参数时，CMake 会自动探测常见安装路径，或读取
`GSTREAMER_1_0_ROOT_MSVC_X86_64` 环境变量。GStreamer 通过导入库动态链接，运行时 DLL
由系统安装提供，不会被复制进构建产物。

### `build.ps1`
构建项目。

```powershell
# 构建所有目标
.\windows\scripts\build.ps1

# 指定并行度
.\windows\scripts\build.ps1 -Jobs 8

# 构建特定目标
.\windows\scripts\build.ps1 -Target agent_gateway_server

# 自定义构建目录
.\windows\scripts\build.ps1 -BuildDir build\custom

# 详细输出
.\windows\scripts\build.ps1 -Verbose
```

### `test.ps1`
运行测试。

```powershell
# 运行所有已构建测试
.\windows\scripts\test.ps1

# 运行特定标签的测试
.\windows\scripts\test.ps1 -Label core

# 按测试名称正则排除测试
.\windows\scripts\test.ps1 -Exclude "redis|media"

# 按 CTest 标签排除测试
.\windows\scripts\test.ps1 -ExcludeLabel integration

# 自定义构建目录
.\windows\scripts\test.ps1 -BuildDir build\custom

# 详细输出
.\windows\scripts\test.ps1 -Verbose

# 只运行 CTest，跳过 SDK package consumer 验证
.\windows\scripts\test.ps1 -SkipPackage
```

测试默认还会安装 SDK 并构建独立的 package consumer；需要只运行 CTest 时使用 `-SkipPackage`。

也可以单独验证已配置构建目录的安装包：

```powershell
.\windows\scripts\verify_package.ps1 `
  -BuildDir build\x64-Release-Tests-v145 `
  -Generator "Visual Studio 18 2026"
```

## 典型工作流

### 首次构建

```powershell
# 1. 准备依赖
.\windows\scripts\prepare_deps.ps1

# 2. 验证依赖（可选）
.\windows\scripts\validate_dependencies.ps1

# 3. 配置构建（启用测试）
.\windows\scripts\configure.ps1 -Tests

# 4. 构建
.\windows\scripts\build.ps1

# 5. 运行测试
.\windows\scripts\test.ps1
```

### 增量构建

```powershell
# 修改代码后重新构建
.\windows\scripts\build.ps1

# 运行测试
.\windows\scripts\test.ps1 -Label core
```

### CI 模拟

```powershell
# 使用 CI 相同的构建目录
.\windows\scripts\configure.ps1 `
  -BuildDir build\gha-windows-tests `
  -Tests `
  -Triplet x64-windows-release

.\windows\scripts\build.ps1 `
  -BuildDir build\gha-windows-tests

.\windows\scripts\test.ps1 `
  -BuildDir build\gha-windows-tests `
  -Exclude "ReloadBatchCycle|RedisV2Batches"
```

## 依赖版本

脚本会自动下载以下依赖（版本与 `linux/scripts/common.sh` 同步）：

- **ONNX Runtime**: 1.17.1
- **SQLite**: 3.53.1 (amalgamation 3530100)
- **Boost**: 1.85.0
- **Eigen**: 5.0.1
- **Faiss**: 1.14.1 CPU（Conda 二进制包，固定 SHA-256）
- **MKL Runtime**: 2023.1.0（Conda 二进制包，固定 SHA-256）
- **OpenCV**: 4.10.0（仅 `-Media`）

SQLite 的 Windows DLL 和模块定义文件也由脚本下载，不依赖本机已有的 `deps` 内容。
llama.cpp 与 GStreamer 是显式外部 SDK：前者通过 `-LlamaCppRoot/-LlamaCppBuild` 指定，
后者通过 `-GStreamerRoot` 或 `GSTREAMER_1_0_ROOT_MSVC_X86_64` 指定。

vcpkg 依赖由 `vcpkg.json` 管理：
- gRPC
- Protobuf
- OpenSSL
- spdlog
- Boost 库（Asio、Redis、Interprocess）
- libzip
- pugixml
- nlohmann/json
- GTest（测试）

## 工具集约定

**重要**：Windows 构建必须保持 CMake generator、MSVC 工具集和 vcpkg/预编译依赖的 ABI 一致。

- 如果 vcpkg 依赖由 **v145** 编译，项目也必须使用 **VS2026/v145**
- 如果 vcpkg 依赖由 **v143** 编译，项目也必须使用 **VS2022/v143**

混用会导致 STL/ABI 链接错误（如 `__std_find_first_not_of_trivial_pos_1` 未定义）。

详见 `CLAUDE.md` 的 **构建/依赖约定** 章节。

## 常见问题

### CMake 找不到 VS 2026 generator

确保使用的 CMake 支持 `Visual Studio 18 2026`：

```powershell
# 检查 CMake 版本
cmake --version

# 检查支持的 generator
cmake --help | Select-String "Visual Studio"
```

如果 `D:\Strawberry\c\bin\cmake.exe` 版本过旧（3.29.2），使用：

```powershell
$env:PATH = "C:\Program Files\CMake\bin;$env:PATH"
cmake --version
```

### vcpkg 依赖安装失败

```powershell
# 清理 vcpkg 缓存
Remove-Item -Recurse -Force vcpkg_installed, build\vcpkg-binary-cache-windows

# 重新准备依赖
.\windows\scripts\prepare_deps.ps1 -Force
```

### 链接错误：STL 符号未定义

检查工具集一致性：

```powershell
# 查看 vcpkg 依赖的工具集
Get-Content vcpkg_installed\x64-windows-release\vcpkg\info\*.list | Select-String "v14"

# 确保 CMake 配置使用相同工具集
# 参见 configure.ps1 的 -Generator 参数
```

## 与 Linux 脚本的对应关系

| Linux | Windows | 说明 |
|-------|---------|------|
| `bootstrap_toolchain.sh` | *系统自带* | Windows 开发者通常已安装 VS |
| `prepare_deps.sh` | `prepare_deps.ps1` | 依赖准备 |
| `configure.sh` | `configure.ps1` | CMake 配置 |
| `build.sh` | `build.ps1` | 编译 |
| `test.sh` | `test.ps1` | 测试 |
| `verify_package.sh` | `verify_package.ps1` | SDK install 与 package consumer |
| N/A | `validate_dependencies.ps1` | Windows 专用依赖验证 |

## 参考

- Linux 构建文档: `linux/README.md`
- 项目构建文档: `README.md` 的 **构建** 章节
- 工具集约定: `CLAUDE.md` 的 **构建/依赖约定**
