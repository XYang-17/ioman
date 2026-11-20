#include <ioman/ioman.h>

#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    ioman::ioman loop;
    {
        auto group = loop.task_group();
        const auto body = group.spawn([] { return std::string{"invoice"}; });
        const auto tax = group.spawn_blocking([] { return 42; }); // 模拟同步税率查询。
        group.join();
        std::cout << body.get() << ", total: " << tax.get() << '\n';
    }
    {
        // 让两个子任务都登记完成后再发布故障，避免失败先关闭投递入口。
        ioman::Promise<void> begin;
        ioman::Promise<void> supplier;
        const auto started = begin.get_future();
        const auto data = supplier.get_future();

        auto group = ioman::task_group();
        const auto validator = group.spawn([started] {
            started.wait();
            throw std::runtime_error("invoice currency is missing");
        });
        const auto pricing = group.spawn_blocking([data] { data.wait(); });
        begin.set_value();
        try {
            group.join();
        } catch (const std::runtime_error& error) {
            std::cout << "group failed: " << error.what() << '\n';
        }
        // join() 报告分组故障时，其他子任务已经退出。
        try {
            pricing.get();
        } catch (const ioman::TaskCancelled&) {
            std::cout << "pricing cancelled\n";
        }
        std::cout << "validator finished: " << validator.is_ready() << '\n';
    }
    {
        ioman::Promise<void> confirmation;
        const auto confirmed = confirmation.get_future();
        auto group = loop.task_group();
        const auto pending = group.spawn([confirmed] { confirmed.wait(); });
        group.cancel(); // 例如用户主动放弃订单。
        group.join();
        std::cout << "abandoned order finished: " << pending.is_ready() << '\n';
    }
}
