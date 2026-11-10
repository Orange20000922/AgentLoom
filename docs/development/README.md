# Development Environment

> 状态：当前规范

## 1. Windows C++ 基线

| 项目 | 要求 |
|---|---|
| CMake | 必须支持 `Visual Studio 18 2026` generator |
| Generator | Visual Studio 18 2026，x64 |
| Compiler | MSVC v145，与 vcpkg 二进制 ABI 一致 |
| vcpkg triplet | `x64-windows` |
| Windows install root | 仓库根 `vcpkg_installed` |
| Windows binary cache | `build/vcpkg-binary-cache-windows` |
| 推荐测试 build | 独立的 VS2026/v145 Release 测试目录 |

配置前执行 `cmake --version` 和 `cmake --help`，确认 PATH 中解析到的 CMake 支持目标 generator。不要复用 VS2022/v143 build 目录。

已有 Ninja/Makefiles IDE 缓存可使用 `windows/scripts/configure.ps1 -Ide`
重新配置并生成 `build-ide/compile_commands.json`。该模式只 configure，保留缓存中的编译器和依赖配置，
不执行编译或链接。可通过 `-BuildDir` 指定其他缓存，通过 `-ExtraCMakeArgs` 更新过期的 cache 项。
clangd 的 `CompilationDatabase` 指向该目录；若需要 protobuf 头，另在本机 `.clangd` 中配置已有生成头目录。

## 2. Linux/WSL 隔离

- Linux install root 使用 `build/linux-vcpkg-installed`；
- Linux binary cache 使用 `build/vcpkg-binary-cache`；
- WSL/Linux 脚本不得写仓库根 `vcpkg_installed`；
- Linux 性能数据必须单独记录内核、发行版、文件系统、编译器和模型 Provider。

## 3. CUDA 验证

Release 目录需要与 CMakeCache 匹配的 ONNX Runtime CUDA、CUDA、cuBLAS 和 cuDNN 运行时。启动后必须从日志确认模型实际使用 CUDA，例如：

```text
TextEmbedding provider=cuda
BERT requested_provider=cuda, active_provider=cuda
```

DLL 存在、GPU 可见或 health check 成功，都不能替代真实推理预热。模型进程重启后应先完成单请求 warm-up，再收集稳定态数据。

## 4. 配置与路径

- 文档和 `.example` 文件只使用仓库相对路径或占位符；
- 本机实际模型、依赖、数据库和凭据使用 ignored 配置；
- 涉及本地依赖目录的构建配置采用 ignored 实际文件加可提交样例；
- 所有文本、配置、测试输入和报告默认使用 UTF-8。

## 5. 推荐验证顺序

```text
检查 CMakeCache / ABI / CUDA runtime / Redis
-> Release 增量构建
-> Persona / Emotion / Vector 聚焦测试
-> 服务 health
-> 真实单请求 CUDA warm-up
-> Fake 基线
-> 真实容量阶梯
-> 极限轮与结构化拒绝
-> 停止服务并审计报告、日志和 dump
```

AgentLoom Gateway 的可复用压测流程由项目 Skill `.agents/skills/agentloom-e2e-benchmark/` 维护。
