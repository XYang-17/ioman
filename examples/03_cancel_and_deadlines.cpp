#include <ioman/ioman.h>

#include <chrono>
#include <iostream>

int main() {
    using namespace std::chrono_literals;
    using Clock = ioman::ioman::Clock;
    ioman::ioman loop;

    // 模拟后台导出等待上游提供数据。主线程决定何时发布。
    ioman::Promise<void> upstream;
    const auto available = upstream.get_future();
    const auto export_job = loop.submit_task([available] {
        available.get();
        return 120;
    });
    try {
        // 客户端当前没有剩余等待预算，后台导出仍须继续。
        (void)loop.wait_for(ioman::shield(export_job), 0ms);
    } catch (const ioman::TimeoutError&) {
        std::cout << "client stopped waiting for the export\n";
    }
    upstream.set_value();
    std::cout << "exported rows: " << export_job.get() << '\n';

    // 直接传 Task 时，到期会请求取消并等待任务退出。
    ioman::Promise<void> provider;
    const auto response = provider.get_future();
    const auto lookup = loop.submit_task([response] { response.wait(); });
    try {
        loop.wait_for(lookup, 0ms);
    } catch (const ioman::TimeoutError&) {
        std::cout << "lookup timed out, finished: " << lookup.is_ready() << '\n';
    }

    const auto polling = loop.submit_task([] {
        while(true) {
            if (ioman::cancellation_requested()) {
                ioman::cancellation_point();
            }
            ioman::sleep_for(5ms); // 等待下一次轮询，期间也可响应取消。
        }
    });
    (void)polling.cancel();
    try {
        polling.get();
    } catch (const ioman::TaskCancelled&) {
        std::cout << "polling cancelled\n";
    }

    const auto request = loop.submit([] {
        // 一个客户端预算覆盖整个流程，内部步骤使用同一个绝对期限。
        return ioman::timeout_at(Clock::now() + 1s, [] {
            ioman::sleep_until(Clock::now() + 2ms);
            ioman::yield_now();
            return 200;
        });
    });
    std::cout << "response status: " << request.get() << '\n';
    try {
        ioman::timeout(0ms, [] { ioman::cancellation_point(); });
    } catch (const ioman::TimeoutError&) {
        std::cout << "request budget exhausted\n";
    }
}
