# 集成测试

验证公开组件组合后的行为，包括线程和协程之间的等待、多个运行时之间的结果交付，以及取消和完成竞争。每个文件可独立运行。可执行文件保留模块层级，输出到 tests/obj/integration 下。报告按源码相对路径写入 tests/report/integration，命名为 `test_名称.report.txt`，记录每个用例结果和总耗时，每次运行覆盖。退出码非零代表失败。程序内看门狗为 50 秒，CTest 超时为 60 秒。

从 ioman 项目根目录执行。

```sh
cmake -S tests -B build-integration -DIOMAN_TEST_MODULES=integration
cmake --build build-integration -j
ctest --test-dir build-integration --output-on-failure
```

如果当前位于 tests 目录，使用以下命令。`-S .` 指向当前目录中的 CMakeLists.txt。

```sh
cmake -S . -B ../build-integration -DIOMAN_TEST_MODULES=integration
cmake --build ../build-integration -j
ctest --test-dir ../build-integration --output-on-failure
```


运行时矩阵覆盖 scheduler worker 数 1/3、pool worker 数 1/3，以及父协程数 0/1/64 的 12 种组合。另以 4 个普通提交线程和多 worker 验证并发投递。流水线固定小容量 3，放大满队列与空队列的交替。取消与完成竞争各运行 32 轮，以允许的终态集合为判定条件。

这些是有限的边界与并发组合测试。操作系统调度的所有交错无法穷举，报告中的通过表示本轮满足表内断言。

## test_runtime_matrix.cpp

[源文件](test_runtime_matrix.cpp) · [最近运行报告](../report/integration/test_runtime_matrix.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `execution_matrix` | worker 数 (1,1)、(1,3)、(3,1)、(3,3) | 各组合 0、1、64 个父协程，每个等待线程任务及子协程 | 无单 worker 死锁，返回值和符合公式 |
| `cross_instance_future` | 两个普通线程各拥有独立实例 | 另一个实例的任务等待共享结果 | 跨实例 Future 唤醒并返回正确值 |
| `concurrent_submitters` | 一个实例 3 scheduler 和 2 pool worker | 4 个普通线程各混合投递 32 项 | 全部 128 项结束且结果正确 |

## test_pipeline.cpp

[源文件](test_pipeline.cpp) · [最近运行报告](../report/integration/test_pipeline.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `bounded_pipeline` | 容量为 3 的队列，1 或 3 worker，2 个生产者和 3 个消费者 | 传递 1..100，每个生产者处理一半，消费者等待条件 | 每项处理一次，总和 5050，关闭唤醒所有消费者 |

## test_cancel_completion_race.cpp

[源文件](test_cancel_completion_race.cpp) · [最近运行报告](../report/integration/test_cancel_completion_race.report.txt)

| 用例 | 前置条件 | 输入与操作 | 预期结果 |
| --- | --- | --- | --- |
| `cancel_versus_completion` | 1 或 3 worker，各重复 32 轮 | 线程发布结果与线程请求取消同时发生 | 结果只能为 42 或 TaskCancelled，任务最终就绪且可继续投递 |
| `timeout_versus_completion` | 单 worker，各重复 32 轮 | 线程发布与零期限 wait_for 竞争 | 只允许成功或 TimeoutError，原 Future 最终可读 |
