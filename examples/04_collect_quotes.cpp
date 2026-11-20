#include <ioman/ioman.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    using Clock = ioman::ioman::Clock;
    ioman::ioman loop(1, 2);
    const auto local = loop.submit([] {
        ioman::sleep_for(5ms);
        return 45;
    });
    const auto legacy = loop.submit_blocking([] {
        std::this_thread::sleep_for(10ms);
        return 48;
    });
    const auto unavailable = loop.submit([]() -> int {
        throw std::runtime_error("supplier unavailable");
    });
    const std::vector<ioman::Future<int>> quotes{local, legacy, unavailable};

    // 完成流只有一个消费者。通过原下标把完成项映射回供应商。
    auto stream = ioman::as_completed(quotes);
    const auto snapshot = loop.wait(quotes, ioman::WaitMode::FIRST_EXCEPTION, Clock::now() + 1s);
    std::cout << "ready at snapshot: " << snapshot.done.size()
              << ", pending: " << snapshot.pending.size() << '\n';
    // 严格汇总可接受首个错误提前交付。流和其余来源仍可继续使用。
    try {
        (void)ioman::gather(quotes).get();
    } catch (const std::runtime_error& error) {
        std::cout << "strict collection failed: " << error.what() << '\n';
    }
    while (const auto item = stream.next()) {
        try {
            const int quote = item->future.get();
            std::cout << "supplier " << item->index << " quoted " << quote << '\n';
        } catch (const std::exception& error) {
            std::cout << "supplier " << item->index << ": " << error.what() << '\n';
        }
    }
    const auto prices = ioman::gather(std::vector{local, legacy}).get();
    std::cout << "first two prices in input order: " << prices[0] << ", " << prices[1] << '\n';

    // 部分成功也可用时，等全部结束，保留原下标及每项异常。
    const auto settled = ioman::gather_settled(quotes).get();
    for (std::size_t index = 0; index < settled.size(); ++index) {
        if (!settled[index].exception()) {
            std::cout << "usable quote " << index << ": " << settled[index].get() << '\n';
        }
    }
}
