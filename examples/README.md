# ioman 示例

每个 `.cpp` 都有独立的 `main()`，使用公开头文件，可分别链接项目的 `ioman` 库。示例用定时等待和手动发布结果来模拟外部服务，无需网络或外部数据文件。并发任务的完成顺序和状态快照可能随运行变化。

| 示例 | 场景 | 主要接口和观察点 |
| --- | --- | --- |
| [01_promise_future.cpp](01_promise_future.cpp) | 异步供应商报价和归档 | 用 `Promise` 发布普通值、异常和只能移动的资源。通过 `Future` 等待、订阅并读取结果，观察订阅回调与生产线程的关系。 |
| [02_submit_report.cpp](02_submit_report.cpp) | 从旧库存接口生成报告 | 用 `submit` 和 `submit_blocking` 分配不同类型的工作。观察单个 worker 如何等待同一调度器里的子任务，以及如何关闭显式和默认运行时。 |
| [03_cancel_and_deadlines.cpp](03_cancel_and_deadlines.cpp) | 后台导出、查询和轮询 | 对比 `wait_for(shield(task), duration)` 与 `wait_for(task, duration)`，演示 `Task::cancel()`、取消检查点、睡眠、让出及绝对和相对期限。 |
| [04_collect_quotes.cpp](04_collect_quotes.cpp) | 收集多个供应商报价，部分来源允许失败 | 用 `wait` 查看状态快照，用 `CompletionStream` 消费完成项，比较 `gather` 与 `gather_settled` 的错误处理。 |
| [05_task_group.cpp](05_task_group.cpp) | 汇总发票，并处理校验失败或用户放弃 | 用 `TaskGroup` 混合投递协程和线程池任务，观察 `join()` 如何报告错误及 `cancel()` 后如何等任务退出。 |
| [06_sync_dispatch.cpp](06_sync_dispatch.cpp) | 分批派发订单并等待处理结束 | 用 `Mutex` 保护共享状态，用 `ConditionVariable` 等待批次就绪，用 `WaitGroup` 等待本轮计数归零。通过完成订阅结算工作，覆盖任务在开始前被取消的情况。 |
