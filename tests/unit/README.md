# 单元测试

测试源码直接放在模块目录下，每个源文件是独立可执行程序。表中列出前置条件、输入与操作、预期结果。异步等待优先使用门信号和任务完成通知确定顺序。

可执行文件按源文件的相对目录输出到 tests/obj，报告按源码的相对路径写入 tests/report，命名为 `test_对象.report.txt`。每次执行覆盖上次报告，包含源文件位置、用例结果及耗时。退出码 0 表示全部通过，非零表示失败。报告即时刷新。新测试有 50 秒进程内看门狗，已有调度器、线程池及顶层测试保留自己的看门狗，CTest 对每个程序另设 60 秒超时。例如 unit/test_task.cpp 对应 tests/obj/unit/test_task，报告为 tests/report/unit/test_task.report.txt。

使用支持 ucontext 的 C++17 工具链配置。例如从项目根目录执行下面的命令。

```sh
cmake -S tests -B build-tests -DIOMAN_TEST_MODULES=unit
cmake --build build-tests -j
ctest --test-dir build-tests --output-on-failure
# 只选一个文件，也可使用相对于 tests/ 的完整路径
cmake -S tests -B build-one -DIOMAN_TESTS=unit/test_mutex.cpp
cmake --build build-one -j
ctest --test-dir build-one --output-on-failure
```

如果当前位于 tests 目录，使用以下命令。`-S .` 指向当前目录中的 CMakeLists.txt。

```sh
cmake -S . -B ../build-tests -DIOMAN_TEST_MODULES=unit
cmake --build ../build-tests -j
ctest --test-dir ../build-tests --output-on-failure
```


模块和文件同时指定时取并集并去重。不存在的模块或文件会在配置阶段报错。各运行实例应依次使用同一源码树，避免同时覆盖二进制或报告。

## test_context.cpp

[源文件](test_context.cpp) · [最近运行报告](../report/unit/test_context.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `test_main_context` | 主线程 | 重复 current() | 非空且地址稳定，未结束 |
| `test_context_switch` | 新建入口分两段的上下文 | 切入、切回、再次切入 | 步骤依次为 0、1、2，主上下文恢复 |
| `test_context_finish` | 入口直接返回 | 切入一次 | 入口执行且标记结束 |
| `test_thread_local_context` | 主线程和一个普通线程 | 分别获取上下文并执行协程 | 主上下文各自独立 |
| `test_invalid_arguments` | 有效主上下文 | 空入口、零栈、空切换参数 | 非法参数抛指定异常 |
| `test_resume_finished_context` | 上下文已完成 | 再次切入 | 抛 logic_error |

## test_coroutine.cpp

[源文件](test_coroutine.cpp) · [最近运行报告](../report/unit/test_coroutine.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `test_coroutine_initial_state` | 新协程 | 读取 state 和 finished | READY，未完成，入口未执行 |
| `test_coroutine_normal_completion` | 新协程 | resume 一次 | 入口运行，状态 DEAD |
| `test_coroutine_single_yield` | 含一次 yield 的入口 | resume 两次 | 先 SUSPENDED 后 DEAD，局部状态保留 |
| `test_coroutine_multiple_yields` | 含多次 yield 的入口 | 多次 resume | 每次仅执行下一段 |
| `test_coroutine_current` | 主线程及协程入口 | 挂起前后查询 current | 入口内指向自身，退出恢复调用者 |
| `test_coroutine_running_state` | 入口执行期间 | 查询 state | RUNNING |
| `test_nested_coroutines` | 外层协程调用内层 | 嵌套 resume 和 yield | 分别返回最近调用者，现场恢复 |
| `test_empty_entry` | 构造阶段 | 空入口 | 抛 invalid_argument |
| `test_invalid_stack_size` | 构造阶段 | 零栈大小 | 抛 invalid_argument |
| `test_resume_dead_coroutine` | 已结束的协程 | 再次 resume | 抛 logic_error |
| `test_resume_running_coroutine` | 入口正在运行 | 对自身 resume | 抛 logic_error |
| `test_yield_ready_coroutine` | 尚未运行的协程 | 调用 yield | 抛 logic_error |
| `test_yield_suspended_coroutine` | 已挂起的协程 | 在外部调用 yield | 抛 logic_error |
| `test_yield_dead_coroutine` | 已结束的协程 | 调用 yield | 抛 logic_error |
| `test_coroutine_exception` | 入口抛异常 | resume | 异常向调用方传播，状态结束 |
| `test_coroutine_exception_after_yield` | 入口先挂起再抛异常 | 恢复两次 | 第二次恢复传播异常 |
| `test_resume_exception_coroutine` | 已因异常结束 | 再次 resume | 拒绝恢复 |
| `test_thread_context_isolation` | 主线程和工作线程 | 分别运行挂起与恢复 | current 和 Context 互不污染 |
| `test_multiple_coroutines` | 两个独立协程 | 交错恢复 | 顺序为 1、2、3、4，均完成 |

## test_scheduler.cpp

[源文件](test_scheduler.cpp) · [最近运行报告](../report/unit/test_scheduler.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `lifecycle_and_invalid_calls` | 未启动的 Scheduler | 零 worker、空任务、空协程、重复启动停止、重启、自线程 stop | 错误被拒绝，重启后可投递，关闭时不复制函数对象 |
| `post_rechecks_after_concurrent_stop` | 投递与关闭并发 | 函数构造期间停止 | 最终登记再次检查入口，拒绝无效投递 |
| `yield_returns_to_queue_tail` | 单 worker 的就绪队列 | 多个任务交错 yield | 让出者排至队尾 |
| `cross_thread_wake_and_early_wake` | 协程显式挂起 | 外线程唤醒与提前唤醒 | 任务恢复且无丢失唤醒 |
| `multiple_workers_and_affinity` | 多个 worker | 任务多次让出 | 任务恢复线程保持一致 |
| `post_coroutine_releases_ownership` | 独立协程 | 交给调度器并执行 | 正确接管并释放协程 |
| `same_worker_cooperative_wait` | 单 worker | 任务等待同 worker 的任务 | 挂起等待方，目标仍能执行 |
| `stop_resumes_suspended_tasks` | 任务已显式挂起 | 停止调度器 | 恢复并交付停止异常 |
| `exception_callbacks_and_cross_scheduler_wait` | 两个调度器和多个回调 | 异常任务、抛异常回调和跨调度器等待 | 异常可读，后续回调仍执行，跨实例唤醒正常 |

## test_thread_pool.cpp

[源文件](test_thread_pool.cpp) · [最近运行报告](../report/unit/test_thread_pool.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `submit_results` | 2 个 worker 上限 | string、void、int& 结果 | 副本共享值，引用保持身份 |
| `move_only_callable_and_result` | 1 个 worker | 捕获并返回 unique_ptr | 只可移动函数与结果可用 |
| `task_exception_and_invalid_calls` | 线程池 | 零 worker、任务抛异常、worker 内关闭 | 错误被拒绝，worker 继续处理下一任务 |
| `parallel_workers` | 2 个 worker 上限和阻塞门 | 同时投递两个任务 | 两个不同线程均能进入 |
| `concurrent_submissions_respect_worker_limit` | 8 个投递线程，3 个 worker 上限 | 并发投递并等待 | 实际使用线程数为 3 |
| `shutdown_before_first_submission` | 尚未投递的池 | shutdown 后投递 | 入口关闭并短路拒绝 |
| `first_submission_races_shutdown` | 新池 | 首次投递与关闭竞争 | 结果符合接受或拒绝的合法路径 |
| `shutdown_drains_ready_queue` | 有运行和排队任务 | 关闭并尝试再投递 | 接受的任务被执行，新投递被拒绝 |
| `destructor_drains_queue` | 有未完成任务 | 离开池的作用域 | 析构等待任务执行结束 |
| `scheduler_waits_for_pool_result` | 调度器与线程池 | 协程等待池任务的 Future | 协程可挂起，结果正确 |

## test_ioman.cpp

[源文件](test_ioman.cpp) · [最近运行报告](../report/unit/test_ioman.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `cancel_queued_and_suspended` | 池 worker 被占用，另有挂起协程 | 取消排队及挂起任务 | 排队函数不执行，等待方收到 TaskCancelled |
| `wait_for_and_shield` | 任务等待门信号 | 保护 Future 与直接 Task 超时 | 保护结果继续，Task 超时取消并等退出 |
| `nested_timeout_and_caller_cancellation` | 父子任务及嵌套期限 | 取消父等待者、触发外层期限 | 未保护子任务被取消，期限归属正确且不泄漏 |
| `wait_and_combinators` | 3 个 Promise 输入 | 按 2、1、3 顺序发布，集合等待含零预算 | 流保完成顺序，聚合保输入顺序，超时保留 pending |
| `gather_failure_and_task_group` | 失败及未完成输入，含两个子任务 | 聚合失败，分组子任务失败 | 快速报告聚合异常，分组取消兄弟并等待退出 |
| `shutdown_preserves_active_timers` | 已开始的协程任务 | 关闭期间继续计时等待 | 活动任务完成后才回收服务 |
| `thread_default_runtime` | 普通线程的运行时绑定 | 显式和默认创建、重复创建、关闭重建 | 每线程唯一，正确路由并可重建 |
| `worker_routing_and_explicit_cross_thread_submit` | 受管线程及远端实例 | 自由函数投递、受管线程创建实例、跨实例投递 | 使用所属实例，非法创建被拒绝，显式目标生效 |
| `free_functions_while_runtime_is_closing` | 有活动任务且运行时正在关闭 | 自由函数计时、再投递及有副作用的函数对象 | 计时继续，新投递提前拒绝并避免复制 |

## test_future.cpp

[源文件](test_future.cpp) · [最近运行报告](../report/unit/test_future.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `shared_value` | 已取得 Future 及其副本 | 发布 42，复制及赋值句柄 | 发布前未就绪，发布后所有副本重复读取 42 |
| `exception_and_void` | 各一个 int 和 void 生产者 | 发布 runtime_error 和 void 完成 | wait 不抛目标异常，get 重抛，void 正常完成 |
| `reference_and_move_only` | 引用对象仍存活 | 发布 int& 和 unique_ptr<int> | 引用地址一致，可移动结果全局只能提取一次 |
| `subscription_lifetime` | 结果尚未发布 | 保留一个订阅，释放另一个，发布后再注册 | 只调用存活订阅，晚注册同步调用，空回调被拒绝 |
| `multiple_thread_waiters` | 一个共享结果 | 8 个普通线程等待同一 Future | 所有线程读取 17，和为 136 |

## test_promise.cpp

[源文件](test_promise.cpp) · [最近运行报告](../report/unit/test_promise.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `single_publication` | 空 Promise<int> | 重复 get_future、set_value 和 set_exception | 重复操作抛 future_error，首次值保持为 7 |
| `invalid_exception` | 未完成的 Promise<void> | 先发布空异常，再正常完成 | 空异常抛 invalid_argument，之后仍可成功发布 |
| `broken_promise` | 已取 Future，生产者离开作用域 | 不发布结果就销毁生产者 | Future 就绪，get 抛 broken_promise |
| `publish_before_get` | 尚未取 Future | 先 set_value(23)，再取 Future | 结果立即可读 |
| `concurrent_publishers` | 一个 Promise<int> | 8 个线程分别发布不同值 | 一个发布成功，其余均收到 future_error |

## test_task.cpp

[源文件](test_task.cpp) · [最近运行报告](../report/unit/test_task.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `copy_cancel_and_shield` | 任务等待未发布的 Future | 复制 Task，经 shield 零期限等待，再取消 | 保护等待超时，首次取消成功，副本 get 抛取消 |
| `success_and_exception` | 运行时已创建 | 值任务、void 任务及异常线程任务 | wait 保留结果，异常经 get 传播 |

## test_completion_stream.cpp

[源文件](test_completion_stream.cpp) · [最近运行报告](../report/unit/test_completion_stream.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `empty_and_duplicates` | 空输入及同一个 Future 的重复位置 | 空流 next，重复输入发布后读取 | 空流结束，重复下标各交付一次 |
| `completion_order_and_failure` | 两个未完成的输入 | 逆序发布，后一输入发布异常 | 流按发布顺序交付，异常由对应 get 抛出 |
| `combinator_edges` | 空、失败和混合完成的 Future 列表 | gather、gather_settled 和三种 wait 模式 | 空列表立即返回，聚合保序，失败不吞异常 |

## test_mutex.cpp

[源文件](test_mutex.cpp) · [最近运行报告](../report/unit/test_mutex.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `invalid_calls_and_ownership` | 普通线程及两个调度协程 | 普通线程加锁、递归 lock、非所有者 unlock | 错误使用抛 logic_error，try_lock 自持时返回 false |
| `contention_matrix` | 1 或 3 个 worker，1 或 48 个协程 | 每个协程持锁更新 20 次并让出 | 最终计数恰为任务数乘 20，无重复占用 |
| `cancel_waiter_then_reuse` | 单 worker，主任务持锁 | 等待者排队后取消，再释放并重新加锁 | 取消不遗留所有权，后续任务可获取锁 |

## test_condition_variable.cpp

[源文件](test_condition_variable.cpp) · [最近运行报告](../report/unit/test_condition_variable.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `invalid_lock` | 未持锁的 unique_lock 和普通线程 | 调用所有 wait 形式 | 抛 logic_error，不登记等待 |
| `predicate_notify_all` | 1 或 3 worker，24 个等待者 | 谓词由同一把锁保护，发布后 notify_all | 每个等待者恰好观察一次条件并退出 |
| `raw_wait_notify_one` | 单 worker，先确认消费者已登记 | 原始 Mutex 重载等待一次，通知一个 | 返回后消费者仍拥有锁 |
| `cancel_reacquires_lock` | 单 worker，消费者已挂起 | 取消 condition.wait | 异常捕获时仍持锁，退出后锁可复用 |

## test_wait_group.cpp

[源文件](test_wait_group.cpp) · [最近运行报告](../report/unit/test_wait_group.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `zero_underflow_overflow` | 空等待组 | wait、add(0)、done、add(INT_MAX) 再 add(1) | 零等待立即返回，下溢和上溢抛异常，原计数保持 |
| `mixed_waiters_and_reuse` | 4 个普通线程及 8 个协程等待 | 连续 3 轮 add(1) 后 done | 所有等待者完成，各轮互不污染 |
| `cancel_preserves_count` | 计数为 1，协程等待 | 取消等待者，随后 done | 调用方取消不改计数，done 后可再次等待 |

## test_cancellation.cpp

[源文件](test_cancellation.cpp) · [最近运行报告](../report/unit/test_cancellation.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `outside_scope` | 普通线程没有取消作用域 | 查询并检查取消 | 返回 false，检查正常返回 |
| `nested_deadline_and_recovery` | 单 worker 运行时 | 内层立即到期，捕获后正常运行下一任务 | 内层转 TimeoutError，取消状态不会泄漏 |

## test_task_handle.cpp

[源文件](test_task_handle.cpp) · [最近运行报告](../report/unit/test_task_handle.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `completion_and_callbacks` | 已启动的单 worker Scheduler，任务等待外部信号 | 复制句柄，登记回调与订阅后发布完成 | 状态 DEAD，存活回调各一次，过期订阅跳过 |
| `failure_wait_modes` | 已启动的 Scheduler | 任务抛 domain_error | wait_ready 只等就绪，error 非空，wait 重抛异常 |
