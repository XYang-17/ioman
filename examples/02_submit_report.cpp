#include <ioman/ioman.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    ioman::ioman loop(1, 2);

    // 模拟只能同步调用的旧库存接口。阻塞操作由线程池承担。
    const auto stock = loop.submit_blocking([] {
        std::this_thread::sleep_for(10ms);
        return std::vector<int>{3, 4, 5};
    });
    const auto report = loop.submit([stock] {
        // 成员形式创建的运行时也会绑定到 worker，内部自由函数使用同一实例。
        const auto header = ioman::submit([] { return std::string{"units in stock: "}; });
        int total = 0;
        for (const int units : stock.get()) {
            total += units;
        }
        // 即使只有一个调度 worker，等待 header 也会挂起当前协程。
        return header.get() + std::to_string(total);
    });

    std::cout << report.get() << '\n';
    loop.shutdown(); // 在运行时所管理的 worker 之外关闭，并先等必要任务完成。

    // 当前线程的旧实例已关闭，自由函数可以按需创建新的默认运行时。
    ioman::submit_blocking_task([] { std::cout << "report audit saved\n"; }).get();
    ioman::shutdown();
}
