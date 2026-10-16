# 静态注册库的整库链接

`agentloom_link_whole_archive(target library)` 用于保留配置段、Route、Skill 等静态库里没有显式符号引用的注册对象。源码构建的 `link_whole_archive` 委托同一实现，安装包也导出该 helper。

## 重复定义的触发条件

若最终目标同时通过普通依赖和整库依赖引入同一静态库，旧 ELF 实现可能生成以下顺序：

```text
libconfig.a --whole-archive libconfig.a --no-whole-archive
```

第一次普通扫描已经提取的对象，在第二次强制展开时再次装载，GNU ld 因此报告 `multiple definition`。Application 的 `ets_algorithms_frozen_e2e` 曾触发这个问题。上游原有 package consumer 没有覆盖相同的直接/传递依赖组合，所以单次上游 CI 成功不能排除它。

CMake 3.28 的原生 `LINK_LIBRARY:WHOLE_ARCHIVE` 配合 feature override，在重复传递依赖场景也可能产生两次强制展开，不能单靠替换为该表达式解决。

## 实现约束

- 普通 `target_link_libraries` 保留构建依赖和传递链接接口。
- `target_link_options` 把强制装载放在普通 archive 扫描之前；ELF 的开启标志、单个 archive 路径和关闭标志作为一个选项传入，不影响其它库。
- helper 解析 ALIAS 并在最终目标记录真实库身份，重复调用只添加一次强制装载选项。
- MSVC 使用 `/WHOLEARCHIVE`，Apple 使用 `-force_load`，其它现有平台分支使用 GNU 风格选项，由 CMake 的 `LINKER:` 语法适配编译器驱动。

修复仅改变链接参数的生成位置与重复调用处理，保持原有静态注册、公共接口和运行时生命周期，并保留 CMake 3.20 下限。源码 helper 与安装包 helper 现在共享实现，避免平台分支分叉。源码构建中 Foundation 会提前引用稍后才定义的 `agent_config`；对此内部 wrapper 延后到当前目录配置结束时调用公共 helper，保留已有目标定义顺序。公共 helper 仍严格检查目标存在。

## 回归

运行 `python tests/tools/whole_archive_cmake_test.py -v`。该测试只使用 CMake、C++ 编译器和 CTest，不需要 SDK 或第三方依赖；它在系统临时目录真实构建、安装、导入一个静态注册库，再运行四类 consumer：本地 ALIAS、已安装 IMPORTED/ALIAS、源码内部 wrapper、先引用后定义的库。测试组合普通直接/传递依赖、重复 helper 调用及 forward reference，并检查未显式引用的静态注册恰好执行一次。

这是构建系统的链接集成测试，测试程序只输出注册次数，由 CTest 检查；不依赖 GTest，也不建立业务断言框架。Linux/Windows CI 在安装大型依赖之前运行该回归。可通过 `AGENTLOOM_TEST_CMAKE` 指定待验证的 CMake 可执行文件。
