# Callback 生命周期与优雅停服

> 文档版本：v1.0
> 更新日期：2026-10-10

运行时将调度配额释放与 callback 生命周期分开。Session lane 和 deferred quota 继续在最终业务
callback 之前释放，避免慢响应或背压阻塞下一轮；关闭流程另行等待 callback 返回及捕获对象销毁。

## 屏障契约

`core::CallbackLifetime` 是每实例的终止屏障。一个 unsigned 原子状态的最高位表示停止接纳，
其余位记录租约。提交异步工作前用 CAS 取得租约，关闭与接纳共享同一原子状态，防止观察到零后
又登记新请求。RAII 租约在业务捕获对象释放后原子减一，不执行等待、互斥锁或条件变量通知。

关闭线程先 `CloseAdmission()`，取消已接纳任务，再 `Wait()` 或 `WaitUntil(deadline)`。
等待在 callback 外部执行，以 condition_variable 每 2ms 检查原子计数，避免完成端加锁或丢失
通知。检查间隔仅作用于停服，实际唤醒延迟由操作系统调度决定。普通请求取消不关闭整个屏障。

`Track()` 包装可复制的 std::function：只执行第一次完成，移出 callback 与租约，按照
callback 捕获销毁、租约归还的顺序释放。Provider 保留已调用的包装函数不会让关闭继续等待；
拒绝提交时销毁未调用包装函数同样释放租约。callback 异常传播给原来的业务异常处理层，
RAII 仍保证正确归还；包装分配错误转换成 core::Status。

完成调用期间的包装状态需要跨线程共享，并且 Provider 可以保留空壳。共享控制块沿用 core
TaskGroup/coroutine 的 shared_ptr 分配方式；现有 ObjectPool 的借用 owner 生命周期不足以承载
这一空壳，不增加共享内存池 allocator 或裸指针释放逻辑。网络字节缓冲仍使用原来的内存池。

## 接入与关闭顺序

OpenAiAsyncLlmClient 在提交 completion 时登记包装回调。Shutdown 拒绝新提交、取消在途 operation，
等待完成 callback 与其捕获对象释放，再停止 retry runtime。PersonaRuntime 在 SubmitChat admission
时包装最终 callback，覆盖排队、同步完成、异步 LLM 和 memory admission，Shutdown 在原有操作
收口之后额外等待这个屏障。

示例 PersonaGatewayServer 复用既有关闭顺序：停止 HTTP/WS 接入、关闭 SSE session、停止维护任务、
取消并关闭 PersonaRuntime、关闭线程池。Runtime 新屏障会在 HttpServer 成员析构之前等待最终
callback 退出；不更改 Session lane/quota 的释放时机，不新建线程池或 callback 队列。

只在最终系统退出、优雅停服或故障关闭资源时等待。必须从未被该屏障计数的控制线程调用 Shutdown。
callback 内若需要停服，交给已有控制线程执行；不能同步等待自己释放租约。接口没有自动把 callback
内 Shutdown 移到其他线程。停服后不支持复用同一 Runtime/Client 接纳新请求，重启应重新装配实例。

WaitUntil 超时只返回 Timeout，不授权析构仍被借用的 HttpServer。Runtime/Client 析构使用无期限
收口；Provider 不履行最终 callback 契约时可能阻止优雅退出，控制层必须保留资源并处理超时。
屏障保护业务 callback，不意味着任意外部保留的 IServerEventStream 都可比 HttpServer 活得更久；
SDK 借用的 socket/executor 仍必须在所属网络 runtime 析构前释放。

## 下游接入边界

独立 Bootstrap 等回调服务可复用 CallbackLifetime。先在异步提交前登记，再在外部关闭流程停止
接纳、取消请求并等待 callback。仅调用 gRPC TryCancel 不等于 callback 已返回。本次修复在 SDK
和参考 Gateway 完成，Application 自有 Bootstrap 需要后续单独接入该公开工具。

## 回归验证

新增五项 core 测试覆盖捕获释放顺序、异常/未调用包装释放、关闭与 CAS admission 竞争、实例隔离、
4096 callbacks/8 线程突发完成；吞吐耗时写入 GTest XML，不设置依赖机器性能的硬门限。
另有 LLM 真实 SSE 完成 callback 阻塞、Persona continuation callback 在 lane 释放后阻塞、
Gateway 活跃 SSE 时 Stop 和析构三项测试。

相关 core、LLM、流式传输、Persona、Gateway 测试集合共 269 项：267 项通过，2 项 Redis 环境
测试跳过。这些测试不访问真实云 LLM，不消费 API 额度。
