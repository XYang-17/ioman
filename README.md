# ioman

ioman 是一个 C++17 协程库。它在 `ioman::ioman` 中统一提供协程调度和独立线程池。两种执行方式都通过 `Future<T>` 返回结果，调用方可以继续等待、设置期限，或通过 `Task<T>` 请求取消。

## 快速开始

```cpp
#include <ioman/ioman.h>

int main() {
    ioman::ioman loop(1, 2);
    const auto input = loop.submit_blocking([] { return 21; });
    const auto result = loop.submit([input] { return input.get() * 2; });
    const int value = result.get();
    loop.shutdown();
    return 42 == value ? 0 : 1;
}
```

这里线程池执行第一个函数。协程在结果尚未就绪时调用 `input.get()` 会挂起，原 worker 可以继续执行其他任务。调用 `result.get()` 的是主线程，因此它会阻塞等待。

### 线程默认运行时与自由函数

包含 `ioman/ioman.h` 后，可以直接用命名空间下的自由函数投递任务和设置期限。普通线程首次调用需要运行时的自由函数时，会按需创建本线程的默认实例。需要指定 worker 数量时，先在该线程显式构造 `ioman::ioman`。两种创建方式共用一个名额，线程已经拥有活跃实例时再次构造会抛出 `std::logic_error`。

```cpp
#include <ioman/ioman.h>

#include <chrono>

int main() {
    const auto result = ioman::submit([] {
        return ioman::timeout(std::chrono::seconds{1}, [] {
            ioman::sleep_for(std::chrono::milliseconds{5});
            return 42;
        });
    });
    const int value = result.get();
    ioman::shutdown();
    return 42 == value ? 0 : 1;
}
```

- 调度器 worker 和线程池 worker 上的自由函数使用该线程所属的实例。受 ioman 管理的线程不能再创建自己的实例。
- 持有其他线程创建的运行时引用时，可以用 `remote.submit(fn)` 显式投递过去。调用期间目标实例须保持有效。被投递的任务执行时，其内部自由函数使用实际执行它的运行时。
- `ioman::shutdown()` 关闭普通线程拥有的实例。线程没有实例时直接返回，受管线程调用时抛出 `std::logic_error`。只有完全关闭后，本线程才可显式构造或按需创建下一代实例。隐式实例也会在线程退出时自动关闭。
- 关闭期间，已开始的任务仍可通过自由函数使用原实例的计时服务，新投递仍被拒绝。
- `yield_now()`、`as_completed()`、`gather()` 和 `gather_settled()` 的自由函数只操作当前执行现场或输入 Future，无需创建运行时。

## 库的构建与使用

需要 CMake 3.16 或更新版本、支持 C++17 的编译器、系统线程库以及可用的 `ucontext` 接口。以下命令使用 Linux 或 WSL 中的工具链，均从项目根目录执行。

### 构建与安装

在项目根目录依次执行配置、构建和安装。默认生成静态库，示例将库及其配套文件安装到项目下的 install 目录。

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j
cmake --install build
```

配置步骤检查编译器和依赖并生成构建文件，构建步骤生成库，安装步骤将库、头文件和 CMake 包配置收集到安装目录。安装完成后，用户项目即可链接使用。

通过标准变量 `CMAKE_INSTALL_PREFIX` 设置安装目录，也可以用 `cmake --install build --prefix /your/install/path` 覆盖本次安装位置。省略安装目录配置时采用 CMake 的默认安装前缀。安装树通常如下，具体库目录由 `GNUInstallDirs` 决定，也可能为 `lib64`。

```text
安装目录/
├── include/
│   └── ioman/
│       ├── ioman.h
│       └── ...
└── lib/
    ├── libioman.a 或 libioman.so
    └── cmake/ioman/
        ├── iomanConfig.cmake
        └── iomanTargets.cmake
```

### 可选构建配置

需要动态库时，在配置阶段增加 `BUILD_SHARED_LIBS=ON`。以下示例使用独立构建目录，安装位置可按需设置。

```sh
cmake -S . -B build-shared \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$PWD/install-shared"
cmake --build build-shared -j
cmake --install build-shared
```

对于 Unix Makefiles 等单配置生成器，可以额外设置 `-DCMAKE_BUILD_TYPE=Release` 来启用面向正式使用的优化配置，或设置 `-DCMAKE_BUILD_TYPE=Debug` 以便调试。基本流程可省略该参数，此时采用工具链默认的编译选项。

### 在用户项目中使用已安装的库

用户程序可包含 `<ioman/ioman.h>`，例如使用上面的快速开始程序。用户项目的 `CMakeLists.txt` 可以写为以下内容，假设入口文件名为 `main.cpp`。

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)

find_package(ioman CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE ioman::ioman)
```

在用户项目根目录配置，通过 `CMAKE_PREFIX_PATH` 指向先前的安装目录。

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/ioman/install
cmake --build build -j
./build/my_app
```

`ioman::ioman` 自动传递头文件目录、C++17 要求和线程依赖。选择动态库时，将 `CMAKE_PREFIX_PATH` 改为动态库安装目录。部署动态链接的程序时，需要让系统动态库加载器能够找到安装的共享库。

### 将源码作为子项目使用

将 ioman 放入用户项目的 `third_party/ioman` 后，可直接链接构建目标。

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)

add_subdirectory(third_party/ioman)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE ioman::ioman)
```

## 测试的构建与运行

测试使用独立的 [tests/CMakeLists.txt](tests/CMakeLists.txt)。以下命令从 ioman 项目根目录执行，需要使用与库相同的 C++17 和 `ucontext` 工具链。

### 全部测试

不指定筛选条件时，自动搜索所有测试源码。

```sh
cmake -S tests -B build-tests
cmake --build build-tests -j
ctest --test-dir build-tests --output-on-failure
```

如果当前已经位于 ioman 的 tests 目录，可使用下面的命令。

```sh
cmake -S . -B ../build-tests
cmake --build ../build-tests -j
ctest --test-dir ../build-tests --output-on-failure
```

`-S` 的相对路径以执行命令时的工作目录为基准。在 tests 目录使用 `-S tests` 会指向 tests/tests。也可用 `-S /absolute/path/to/ioman/tests` 明确指定源码目录。

### 指定模块或测试文件

`IOMAN_TEST_MODULES` 接收 `tests` 下的模块目录名，例如 `unit` 或 `integration`。下面只构建单元测试。

```sh
cmake -S tests -B build/unit -DIOMAN_TEST_MODULES=unit
cmake --build build/unit -j
ctest --test-dir build/unit --output-on-failure
```

`IOMAN_TESTS` 接收测试名、文件名或相对于 `tests` 的路径。下面只编译 `unit/test_task.cpp` 及其库依赖。

```sh
cmake -S tests -B build/task -DIOMAN_TESTS=unit/test_task.cpp
cmake --build build/task -j
ctest --test-dir build/task --output-on-failure
```

选择多个项目时使用带引号的分号列表，例如 `-DIOMAN_TESTS="test_future;test_promise"` 或 `-DIOMAN_TEST_MODULES="unit;integration"`。同时指定模块和文件时取并集，重复匹配的测试只构建一次。不存在的模块或文件会直接报错，同名文件存在歧义时须使用相对路径。

这些选项会保存在构建目录的缓存中。复用同一个构建目录并恢复全部测试时，应显式清空两个选项。

```sh
cmake -S tests -B build-tests -DIOMAN_TEST_MODULES="" -DIOMAN_TESTS=""
```

### 可执行文件与报告

测试程序保留源码相对于 `tests` 的目录层级，输出到 `tests/obj`。运行后，报告写入 `tests/report`，同样保留模块层级。

```text
tests/unit/test_task.cpp
tests/obj/unit/test_task
tests/report/unit/test_task.report.txt
```

也可从项目根目录直接运行 `./tests/obj/unit/test_task`。报告每次运行覆盖上一次内容，包含用例结果和耗时。退出码 0 表示全部通过，非零表示失败。多个构建目录会共享这些可执行文件和报告路径，使用同一源码树时应依次构建和运行。

完整用例说明见 [单元测试](tests/unit/README.md) 和 [集成测试](tests/integration/README.md)。

## 用户接口目录

[示例目录](examples/README.md)提供独立程序，演示结果交付、任务投递、取消、超时、组合等待与协程同步。

```text
include/
├── ioman.h                运行时、任务分组和自由函数
├── future.h               共享的异步结果 Future<T>
├── task.h                 可取消的任务 Task<T> 和 shield()
├── promise.h              手动发布结果的 Promise<T>
├── cancellation.h         取消检查函数及 TaskCancelled、TimeoutError
├── completion_stream.h    完成流、完成项和集合等待的结果类型
└── sync/
    ├── mutex.h                协程互斥锁
    ├── condition_variable.h   协程条件变量
    └── wait_group.h           可供普通线程与协程等待的工作计数
```

使用顶层运行时通常只需包含 `ioman/ioman.h`。同步组件按需包含各自的头文件。

## 投递、读取结果和取消

构造 `ioman::ioman(scheduler_workers, pool_workers)` 时会启动调度器 worker 和计时线程。`pool_workers` 是线程池的 worker 上限，线程池从零个线程开始。两个参数默认均为 1，且都须大于零。四种投递都接收可无参调用的函数对象，参数可通过 lambda 捕获传入。设函数的返回类型为 `T`，支持值、引用和 `void`。用户函数抛出的异常由对应结果句柄交付。运行时关闭后，投递调用直接抛出 `std::logic_error`。

| 调用 | 执行位置 | 返回类型 |
| --- | --- | --- |
| `loop.submit(fn)` | 协程调度器 | `Future<T>` |
| `loop.submit_blocking(fn)` | 独立线程池 | `Future<T>` |
| `loop.submit_task(fn)` | 协程调度器 | `Task<T>` |
| `loop.submit_blocking_task(fn)` | 独立线程池 | `Task<T>` |

在自由函数形式中，将 `loop.submit(fn)` 写成 `ioman::submit(fn)` 即可，其余三种投递也使用相同的名称和签名。计时、集合等待和任务分组同样提供去掉 `loop.` 的自由函数形式。

线程池每次投递时先计算已有 worker 可接手多少待取任务。容量不足且尚未达到上限时，尝试启动一个 worker。达到上限后，新任务留在 FIFO 队列中。已启动的 worker 留在池中复用。首个 worker 启动失败时，此次投递抛出异常。后续扩容失败时，已启动的 worker 继续处理队列。

### Future 与 Task

| 接口 | 行为 |
| --- | --- |
| `is_ready()` | 查询值、异常或取消结果是否已经发布。 |
| `wait()` | 等待目标完成，保留值供后续读取。目标本身的异常通过 `get()` 传播。 |
| `get()` | 等待并取得值或引用。若任务返回 `void`，只等待结束。任务异常在调用处重新抛出。 |
| `future.exception()` | 查询已发布的异常指针。成功或尚未完成时返回空指针。 |
| `future.on_ready(fn)` | 订阅一次完成通知。保留返回的订阅句柄，直到不再需要通知。 |
| `task.future()` / `ioman::shield(task)` | 取得共享同一结果的 Future。等待方取消时，取消请求不会转发给目标。 |
| `task.cancel()` | 请求取消。返回 `true` 表示本次发布了新请求，可再用 `wait()` 确认目标已结束。 |

`Future<T>` 和 `Task<T>` 都从投递或结果生产者取得，没有公开的默认构造函数。复制构造或赋值后，新句柄与来源共享结果。复制 Task 也共享取消能力。给句柄赋予另一个任务的结果或销毁句柄，都只影响句柄本身持有的引用。

`wait()` 返回 `void`，可以响应调用方的取消。目标失败、取消或成功都算完成，因此确认完成后通常还要用 `get()` 检查结果。查询 `is_ready()` 只能知道结果是否发布。可以读 `get()` 取得结果，也可以用 Future 的 `exception()` 单独查看失败原因。

Future 副本共享结果。可复制值可以多次读取，引用结果要求所引用的对象在使用期间有效。只可移动的结果由所有共享句柄共同提取一次，再次提取会抛出 `std::future_error`。通过 `Task::wait()` 或 `Task::get()` 等待时，等待方自身的取消会向目标任务传播。

`on_ready()` 注册时若结果已就绪，回调会在调用线程立即执行，否则由发布结果的线程执行。释放最后一个订阅句柄后，发布方尚未取得的回调会被跳过。已经取得的回调仍可能执行，回调引用的对象须保持有效。

下面用一个订阅记录结果已经发布。回调可以在发布线程运行，应保持简短。

```cpp
#include <ioman/promise.h>

bool observe_completion() {
    ioman::Promise<int> producer;
    const auto result = producer.get_future();
    bool notified = false;
    const auto subscription = result.on_ready([&] { notified = true; });
    producer.set_value(42);
    return notified && result.is_ready() && nullptr != result.exception() && 42 == result.get();
}
```

### 主动请求取消

排队中的任务被取消时会跳过用户函数，Future 交付 `TaskCancelled`。已开始的任务采用协作式取消，在 `get()`、`sleep_for()`、`yield_now()` 或显式 `cancellation_point()` 等取消检查点响应请求。计算循环可用 `cancellation_requested()` 查询状态，或在合适的位置执行检查点。同步 I/O 和外部库调用需要自己支持中断。

| 接口 | 输入、输出与注意事项 |
| --- | --- |
| `cancellation_requested()` | 无参数，立即返回当前取消作用域是否已收到请求。没有作用域时返回 `false`。 |
| `cancellation_point()` | 无参数，未收到请求时正常返回。收到请求时抛出该作用域记录的异常。 |
| `TaskCancelled` / `TimeoutError` | 可按 `const&` 捕获，也可通过 `what()` 获取诊断文字。前者用于任务取消，后者用于到期的等待或期限作用域。 |

```cpp
#include <ioman/ioman.h>

#include <chrono>

void cancel_task(ioman::ioman& loop) {
    const auto task = loop.submit_task([&loop] {
        loop.sleep_for(std::chrono::seconds{10});
    });
    task.cancel();
    task.wait();
    try {
        task.get();
    } catch (const ioman::TaskCancelled&) {
        // 此任务以取消结束。
    }
}
```

### 手动交付异步结果

`Promise<T>::get_future()` 每个生产者可调用一次，取得的 Future 可以复制。`set_value(value)`、`set_value()`（`void`）和 `set_exception(error)` 中只有一次发布能成功。异常指针须非空。Promise 在发布前析构时，Future 以 `std::future_error(broken_promise)` 结束。Promise 默认构造后拥有一个可发布的状态，禁止复制。各发布函数返回 `void`，重复发布或重复取 Future 抛出 `std::future_error`。对引用结果，应让被引用对象活到结果使用结束。传入空异常指针会抛出 `std::invalid_argument`。

```cpp
#include <ioman/promise.h>

int promise_example() {
    ioman::Promise<int> producer;
    const auto result = producer.get_future();
    producer.set_value(42);
    return result.get();
}
```

## 计时、期限和保护

`sleep_for`、`sleep_until` 和 `yield_now` 返回 `void`，均可响应当前任务的取消。相对时长用 `std::chrono` 表达，例如 `std::chrono::milliseconds{50}`。绝对期限可用 `ioman::ioman::Clock::now() + duration` 计算。两个 `wait_for` 重载返回 `T`，`T` 为 `void` 时只等待。结果在期限内就绪时，按 `get()` 的规则交付值或异常。

| 接口 | 行为 |
| --- | --- |
| `loop.sleep_for(duration)` | 等一段时长。在协程内挂起，在普通线程中阻塞。 |
| `loop.sleep_until(deadline)` | 等到绝对时刻。绝对期限用 `ioman::ioman::Clock`，即 `std::chrono::steady_clock`。 |
| `ioman::ioman::yield_now()` | 在调度协程中主动让出，回到原 worker 的队尾。在普通线程中调用 `std::this_thread::yield()`。 |
| `loop.wait_for(future, duration)` | 超时抛出 `TimeoutError`，只结束本次等待。 |
| `loop.wait_for(task, duration)` | 超时向目标发出取消请求，等目标退出后再抛出 `TimeoutError`。为清理资源，实际等待可能超过指定时长。 |
| `loop.timeout(duration, fn)` | 在当前执行现场调用 `fn`，其中多次等待共享一个相对期限。 |
| `loop.timeout_at(deadline, fn)` | 使用绝对期限。作用域可以嵌套，由触发期限的作用域将其取消转换为 `TimeoutError`。 |

```cpp
#include <ioman/ioman.h>

#include <chrono>

void protected_wait(ioman::ioman& loop) {
    const auto task = loop.submit_task([&loop] {
        loop.sleep_for(std::chrono::milliseconds{20});
        return 42;
    });

    try {
        loop.wait_for(ioman::shield(task), std::chrono::milliseconds{5});
    } catch (const ioman::TimeoutError&) {
        // 这次等待已结束，原任务仍可继续。
    }
    task.get();
}
```

`shield()` 只隔离通过等待方传来的取消。直接对原 Task 调用 `cancel()`，或由运行时关闭时取消尚未开始的任务，仍然有效。期限也依赖协作式检查点。`timeout` 和 `timeout_at` 直接返回无参函数的结果（包括引用和 `void`），并传播它的异常。放入不经过检查点的长计算时，可在循环中调用 `cancellation_point()`，使期限到达后能退出。

下面用绝对期限限制同一段协程代码。该代码仍在投递它的调度协程中执行，期限会覆盖其中的睡眠和让出。

```cpp
#include <ioman/ioman.h>

int use_deadline(ioman::ioman& loop) {
    using namespace std::chrono;
    return loop.submit([&] {
        const auto deadline = ioman::ioman::Clock::now() + seconds{1};
        return loop.timeout_at(deadline, [&] {
            loop.sleep_until(ioman::ioman::Clock::now() + milliseconds{5});
            ioman::yield_now();
            return 42;
        });
    }).get();
}
```

## 组合等待与子任务分组

组合接口接收同一结果类型的 `std::vector<Future<T>>`。可混合协程、线程池和手动 Promise 的结果。手头有 `Task<T>` 时，用 `task.future()` 加入列表。

| 接口 | 行为 |
| --- | --- |
| `loop.as_completed(futures)` | 返回单消费者完成流。每次 `next()` 给出原下标和已就绪的 Future，取完返回 `std::nullopt`。多个完成通知同时到达时以实际入队顺序为准。 |
| `loop.wait(futures, mode, deadline)` | 支持 `FIRST_COMPLETED`、`FIRST_EXCEPTION` 或 `ALL_COMPLETED`。返回 `done` 和 `pending` 的原输入下标。期限可省略，超时仍返回下标。 |
| `loop.gather(futures)` | 成功时按输入顺序返回结果。首次观察到异常即完成聚合 Future，其余任务继续执行。支持同类型的可复制值或 `void`。 |
| `loop.gather_settled(futures)` | 等全部完成并按原顺序返回 Future 列表，可逐项读取异常、引用和只可移动的值。 |

```cpp
#include <ioman/ioman.h>

#include <vector>

void collect_results(ioman::ioman& loop) {
    const auto a = loop.submit([] { return 1; });
    const auto b = loop.submit_blocking([] { return 2; });
    const std::vector inputs{a, b};
    auto stream = loop.as_completed(inputs);
    while (auto item = stream.next()) {
        const auto index = item->index;
        const int value = item->future.get();
        (void)index;
        (void)value;
    }
    const auto values = loop.gather(inputs).get(); // {1, 2}
    (void)values;
}
```

也可直接用 `CompletionStream<T>(futures)` 创建流。流不可复制，同一时间只能有一个消费者调用 `next()`。它返回 `std::optional<Completed<T>>`，完成项含零基下标 `index` 和已经就绪的 `future`。没有可读项时，协程挂起，普通线程阻塞。读完全部输入后返回空值。销毁流释放其订阅，各输入任务继续按原计划执行。

`wait()` 的默认模式是 `WaitMode::ALL_COMPLETED`。启用 `FIRST_COMPLETED` 时任何终态都能触发返回。启用 `FIRST_EXCEPTION` 时，失败或取消会触发返回，全部成功时等到最后一项。返回的 `WaitResult::done` 和 `pending` 各自按输入下标排列，是返回前逐项检查得到的分类。任务可能随后完成。超时后可以继续等待或读取原句柄。

```cpp
#include <ioman/ioman.h>

void inspect_results(ioman::ioman& loop) {
    const auto a = loop.submit([] { return 1; });
    const auto b = loop.submit_blocking([] { return 2; });
    const std::vector inputs{a, b};
    const auto state = loop.wait(inputs, ioman::WaitMode::FIRST_COMPLETED,
        ioman::ioman::Clock::now() + std::chrono::milliseconds{50});
    for (const auto index : state.done) {
        (void)inputs[index].get();
    }

    const auto settled = loop.gather_settled(inputs).get();
    for (const auto& result : settled) {
        if (!result.exception()) {
            (void)result.get();
        }
    }
}
```

如果需要按实际通知顺序边完成边处理，应在等待全部结果之前创建完成流。创建时已经完成的输入按注册顺序交付。输入取消也算就绪。对 `FIRST_EXCEPTION`，输入的取消异常同样满足停止条件。使用只可移动结果时，所有观察方应约定由谁提取。

### TaskGroup

`loop.task_group()` 或自由函数 `ioman::task_group()` 返回一个空的 TaskGroup，无需传入参数。分组不可复制。`spawn(fn)` 投递协程，`spawn_blocking(fn)` 投递线程池任务，两者都返回 `Task<T>`。任一子任务以非取消异常结束时，分组请求取消其余子任务。`cancel()` 主动关闭分组并发出取消请求，`join()` 等待全部子任务退出，再报告首个观察到的非取消异常。`cancel()` 和 `join()` 都返回 `void`，并关闭分组的投递入口。关闭后再次 `spawn` 或 `spawn_blocking` 抛出 `std::logic_error`。`join()` 的调用方被取消时，分组先请求子任务取消，等它们退出后再传播调用方的异常。分组由创建方操作，且须早于运行时销毁。分组析构会请求取消，需要等待实际退出时应显式调用 `join()`。

```cpp
#include <ioman/ioman.h>

int run_group(ioman::ioman& loop) {
    auto group = loop.task_group();
    const auto a = group.spawn([] { return 20; });
    const auto b = group.spawn_blocking([] { return 22; });
    group.join();
    return a.get() + b.get();
}
```

## 协程同步组件

| 组件 | 方法与用法 |
| --- | --- |
| `Mutex`（`ioman/sync/mutex.h`） | `lock()` 竞争时挂起并按排队顺序等待，`try_lock()` 立即尝试，`unlock()` 由持锁协程调用。支持 `std::lock_guard` 和 `std::unique_lock`，锁不可递归获取。 |
| `ConditionVariable`（`ioman/sync/condition_variable.h`） | `wait(mutex)` 或 `wait(lock)` 登记等待并释放关联锁，恢复或取消后先拿回锁。优先用 `wait(lock, predicate)` 在持锁时检查条件。任意线程均可调用 `notify_one()` 或 `notify_all()`。 |
| `WaitGroup`（`ioman/sync/wait_group.h`） | 投递工作前用 `add(n)` 登记，完成时调用 `done()`，用 `wait()` 等本轮计数归零。支持普通线程和调度协程等待，归零后可开始新一轮。 |

```cpp
#include <ioman/ioman.h>
#include <ioman/sync/condition_variable.h>
#include <ioman/sync/wait_group.h>

#include <mutex>

void sync_example(ioman::ioman& loop) {
    ioman::Mutex mutex;
    ioman::ConditionVariable condition;
    ioman::WaitGroup done;
    bool ready = false;

    done.add(1);
    const auto consumer = loop.submit([&] {
        std::unique_lock<ioman::Mutex> lock(mutex);
        condition.wait(lock, [&] { return ready; });
        done.done();
    });
    const auto producer = loop.submit([&] {
        {
            const std::lock_guard<ioman::Mutex> lock(mutex);
            ready = true;
        }
        condition.notify_one();
    });

    done.wait();
    consumer.get();
    producer.get();
}
```

同一条件应使用同一把 Mutex 保护。通知发出时若无人等待，本次通知立即结束。同步对象必须活到所有使用者结束。`WaitGroup` 的取消只结束调用方自己的等待，工作计数仍由工作方结算。如果任务有失败或取消路径，需要确保这些路径也会结算已登记的工作。

### 同步接口约定

- `Mutex` 默认构造为可获取状态。`lock()` 和 `unlock()` 返回 `void`。`try_lock()` 返回是否取得所有权，立即返回且跳过取消检查。持有者重复 `lock()`、从错误的协程解锁，或在普通线程调用这些操作，会抛出 `std::logic_error`。等待加锁可以响应取消。
- `ConditionVariable` 默认构造为空队列。三个 `wait` 重载都返回 `void`，调用时须由当前调度协程持有传入的 `Mutex`。无谓词重载应放在检查条件的循环中。谓词以无参方式在持锁时运行，返回真值时继续执行。等待发生异常时会先恢复持锁状态。两种通知均无参数和返回值，`notify_one()` 唤醒一名有效等待者，`notify_all()` 通知当前取出的整批等待者。
- `WaitGroup` 默认构造时计数为零。`add(int n)` 支持批量增减，`done()` 等于 `add(-1)`，两者都返回 `void`。结果小于零抛出 `std::logic_error`，超出上限抛出 `std::overflow_error`。`wait() const` 返回 `void`，先检查取消，再等待所观察到的那一轮归零。开始下一轮时，上一轮已登记的等待者仍能收到上一轮通知。
- 三种同步对象均不可复制。共享时传递引用，并确保所有调用结束后再销毁对象。

以下示例用 `try_lock()` 的 RAII 形式更新共享计数。拿不到锁时直接返回，调用方可以通过 Future 读取是否执行了更新。

```cpp
#include <ioman/ioman.h>
#include <ioman/sync/mutex.h>

bool try_update(ioman::ioman& loop, ioman::Mutex& mutex, int& count) {
    return loop.submit([&] {
        const std::unique_lock<ioman::Mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        ++count;
        return true;
    }).get();
}
```

## 关闭运行时

从本运行时管理的线程之外调用 `loop.shutdown()`。由它自己的 worker 调用时会抛出 `std::logic_error`。它无参数、返回 `void`，可重复调用。运行时析构时也会关闭：

1. 拒绝新投递。
2. 让尚未开始的任务以 `TaskCancelled` 结束，并跳过其用户函数。
3. 等已开始的任务退出。期间两套执行器和计时线程仍能处理唤醒。
4. 回收 worker 和计时线程。

需要确保某项任务实际执行时，应在关闭前等待它完成。已经开始的任务须能自行收尾，循环等待或长期阻塞可能使关闭一直等待。应将阻塞操作投递给线程池，避免独占调度 worker。一个线程池任务同步等待同池的另一个排队任务时，也要保证有 worker 能运行被等待的任务。引用捕获须活到任务和相关回调都结束。

## 内部机制

下列组件供运行时及执行器使用。应用通常通过前面的投递、等待和取消接口完成同样的操作。

| 接口 | 直接使用时的约定 |
| --- | --- |
| `Scheduler` | `start()` 后通过 `post(fn)` 投递函数并取得 `TaskHandle`； <br>`post_coroutine(co)` 接管新协程的所有权；<br>`suspend_current()` 等待显式唤醒；<br>`resume_later(co)` 将目标交回原 worker；<br>`current()` 查询当前线程的调度器；<br>`pending_count()` 查询就绪队列长度；<br>`is_closed()` 查询是否已停止接收新任务；<br>`stop()` 会使显式挂起的协程恢复并收到停止异常。 |
| `ThreadPool` | 按投递量启动 worker；<br>`submit(fn)` 返回 Future；<br>`shutdown()` 拒绝新投递，并等待已接收的任务执行完毕；<br>`is_closed()` 查询投递入口是否已关闭。 |
| `TaskHandle` | `done()` 和 `state()` 查询状态；<br>`wait()` 传播目标异常；<br>`wait_ready()` 只等待就绪；<br>`error()` 读取异常；<br>`on_done(fn)` 由任务状态持有回调；<br>`subscribe(fn)` 由调用方持有订阅句柄。 |
| `Coroutine` | `resume()` 切入协程；<br>`yield()` 切回最近的调用者；<br>`current()`、`state()`、`finished()` 用于观察现场和状态；<br>调度器里的直接 `yield()` 会重新排队。 |
| 取消作用域、等待事件和计时器 | 输入为完成、取消或到期信号，向等待者交付首先发布的唤醒原因；<br>作用域与当前执行现场关联，内部订阅随等待清理。 |
| `Context` | `create(entry, stack_size)` 创建独立栈，默认独立栈为 64 KB；<br>`current()` 取得本线程当前上下文；<br>`swap(from, to)` 切换现场；<br>`finished()` 查询入口是否已返回；<br>调用方需保证上下文和栈在可能恢复期间有效。 |

## 未来工作

- **任务调度优先级**。可以先为各 worker 的就绪队列增加高、普通、低三档优先级，同级按入队顺序运行。明确协程唤醒后是否沿用原优先级，并为低优先级任务保留执行机会。正在运行的协程会继续执行，直到主动让出、挂起或退出。
- **更多协程同步组件**。增加用于限制并发的 `Semaphore`、用于等待状态变化的 `Event`，以及用于任务间传递数据的有界 `Channel<T>`。明确取消与唤醒同时发生时许可或消息如何交接，以及通道关闭后如何处理缓冲数据和等待者。
- **非阻塞 I/O**。在 Linux 上探索基于 `epoll` 的文件描述符等待层，再提供连接、接收和发送等套接字操作。让就绪、超时、取消和描述符关闭都能结束同一次等待，并处理部分读写及迟到的通知。
- **背压与容量控制**。考虑为调度器和线程池设置待处理任务上限。队列满时可先提供立即报告接收结果的 `try_submit`，再探索能响应取消、超时和关闭的容量等待接口。
- **任务分组与异常诊断**。考虑为 `TaskGroup` 增加并发上限、整体期限和可选的多异常收集。汇总时区分子任务自身失败和因分组取消而退出。
- **运行时可观测性**。为任务增加名称或 ID，记录入队、开始、挂起、唤醒和结束等状态变化。统计各 worker 的队列深度和等待时长，辅助定位调度饥饿和关闭时仍未退出的任务。
- **并发验证与性能基线**。增加可复现的竞态测试、压力测试和基准测试，覆盖完成与超时、取消与锁交接、投递与关闭同时发生的情况。记录投递到开始、挂起到恢复的延迟，以及任务积压时的内存占用。
