#include <ioman/ioman.h>
#include <ioman/sync/condition_variable.h>
#include <ioman/sync/mutex.h>
#include <ioman/sync/wait_group.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <vector>

int main() {
    ioman::Mutex mutex;
    ioman::ConditionVariable changed;
    ioman::WaitGroup processed;

    // 一个发货批次就绪后，两个协程分别处理自己的订单。
    std::array<int, 2> orders{};
    bool ready = false;
    // 运行时先析构，确保任务和回调引用的数据仍然有效。
    ioman::ioman loop(2);
    const auto process_batch = [&](const std::array<int, 2>& batch) {
        loop.submit([&] {
            const std::lock_guard<ioman::Mutex> lock(mutex);
            ready = false;
        }).get();
        processed.add(static_cast<int>(batch.size())); // 先登记，再投递等待者。
        std::vector<ioman::Future<int>> results;
        for (std::size_t index = 0; index < batch.size(); ++index) {
            results.push_back(loop.submit([&, index] {
                std::unique_lock<ioman::Mutex> lock(mutex);
                changed.wait(lock, [&] { return ready; });
                return orders[index] * 2;
            }));
        }

        // 完成回调负责结算，任务失败或开始前取消也能归还计数。
        // 保留订阅句柄，直到这一轮结束。
        const auto settle = [&] { processed.done(); };
        using Subscription = decltype(results.front().on_ready(settle));
        std::vector<Subscription> subscriptions;
        for (const auto& result : results) {
            subscriptions.push_back(result.on_ready(settle));
        }
        const auto publisher = loop.submit([&, batch] {
            {
                const std::lock_guard<ioman::Mutex> lock(mutex);
                orders = batch;
                ready = true;
            }
            changed.notify_all(); // 谓词的读写使用同一把锁。
        });
        publisher.get();
        processed.wait(); // 主线程阻塞等待本轮。下一次 add() 会开启新一轮。
        for (const auto& result : results) {
            std::cout << "packed units: " << result.get() << '\n';
        }
    };

    try {
        process_batch({3, 5});
        process_batch({2, 4});
    } catch (...) {
        // 投递或登记失败时也要唤醒已开始等待的消费者。
        loop.submit([&] {
            {
                const std::lock_guard<ioman::Mutex> lock(mutex);
                ready = true;
            }
            changed.notify_all();
        }).get();
        throw;
    }
}
