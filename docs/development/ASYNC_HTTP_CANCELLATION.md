# 异步 HTTP 取消与关闭的生命周期约束

## 问题与根因

普通 HTTP/HTTPS 请求可能在取消、超时或客户端关闭之后，继续收到已经排队的成功完成回调。取消底层 resolver/socket 不能把已经就绪的成功回调改成 `operation_aborted`。若错误处理先判断 `!error`，终态请求仍会继续执行 DNS → connect → handshake → write → read 链路，访问已经释放或转移的连接。

此外，普通请求的 resolver、deadline 和取消原本使用 operation strand，而 connect、TLS handshake、HTTP write/read 的完成处理未绑定该 strand。多个 IO 线程下，取消可能与这些处理并发操作同一 stream。关闭时立即重置连接还会提前销毁 Beast 组合操作仍然借用的 stream。

该竞态已在 Linux Release 环境中复现：原有 `ShutdownCancelsInflightAndWaitsForCallback` 重复执行时发生 SIGSEGV，调试栈位于普通 HTTP 请求的 DNS 成功回调。上游 CI 的单次执行通过，不能排除这一调度窗口；目前没有证据表明它只会发生在 Linux。

## 修复约束

- 所有传输完成处理先检查 operation 是否已经完成，成功回调也必须遵守终态。
- 普通 HTTP/HTTPS 的 connect、handshake、write/read 完成处理绑定既有 operation strand，与取消和 deadline 串行执行。
- 取消只关闭连接，不立刻释放连接对象。待处理 handler 持有 operation，operation 继续持有 stream，直到组合操作和其完成处理全部退出。
- 成功读取且允许 keep-alive 时仍按既有规则归还连接；终态检查防止晚到处理继续访问已经归还的连接。

修复复用已有 io_context、strand、连接池与取消入口，不新增线程池、公开接口或平台专用实现。连接保持到最后一个 handler 退出，与已有 streaming 路径的生命周期策略一致。

## 回归验证

`http_client_tests` 继续运行原有 HTTP 测试，并增加 `PlainAndTls/AsyncBeastHttpCancellationTest`：

- HTTP 和 HTTPS 各使用四个 IO 线程，连续 64 轮提交四个请求后立即关闭，检查每个请求只回调一次。此用例扩大调度覆盖范围，不保证每次都制造同一种回调顺序。
- 本地测试服务器收到首字节后保持连接而不响应，分别覆盖 HTTP 请求等待响应、TLS 请求等待握手响应时的取消、超时与关闭。释放调用方句柄后，客户端仍须安全排空 IO 回调并只报告一次终态。

测试服务器位于对应 GTest 文件内，使用动态分配的 loopback 端口，不依赖外网或服务端证书。可使用 GTest 原生重复模式进行压力验证：

```text
http_client_tests --gtest_filter=*AsyncBeastHttpCancellationTest.*:AsyncBeastHttpClientTest.ShutdownCancelsInflightAndWaitsForCallback --gtest_repeat=100 --gtest_break_on_failure
```

在 Linux 上同时运行 Release 重复测试和 ASan/UBSan 检查：消毒器会改变调度，不能用消毒器未复现来替代 Release 验证。完整 SDK 与应用集成结果仍以对应 CI 为准。
