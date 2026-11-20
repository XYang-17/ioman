#include <ioman/promise.h>

#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

int main() {
    ioman::Promise<int> producer;
    const auto price = producer.get_future();
    const auto invoice_price = price; // 两个句柄共享同一次报价。
    std::atomic<bool> notified{false};
    // 保存订阅。报价发布后，回调可能在报价线程上执行。
    const auto subscription = price.on_ready([&] { notified.store(true); });

    std::thread supplier([&] {
        try {
            const std::string reply = "4200"; // 模拟供应商返回的分币报价。
            producer.set_value(std::stoi(reply));
        } catch (...) {
            producer.set_exception(std::current_exception());
        }
    });

    price.wait(); // 普通线程阻塞。wait() 只等待发布，get() 才读取结果。
    supplier.join(); // 确保回调已经退出，才能检查它留下的标记。
    try {
        std::cout << "price in cents: " << invoice_price.get()
                  << ", notified: " << notified.load() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "quote failed: " << error.what() << '\n';
        return 1;
    }

    ioman::Promise<void> archive;
    const auto archived = archive.get_future();
    archive.set_exception(std::make_exception_ptr(std::runtime_error("archive is read-only")));
    if (archived.is_ready() && archived.exception()) {
        try {
            archived.get();
        } catch (const std::runtime_error& error) {
            std::cout << "expected archive error: " << error.what() << '\n';
        }
    }

    // 可移动资源也能交付。所有 Future 副本合计只能提取一次这个资源。
    ioman::Promise<std::unique_ptr<std::string>> download;
    const auto document = download.get_future();
    download.set_value(std::make_unique<std::string>("invoice.csv"));
    const auto filename = document.get();
    std::cout << "downloaded: " << *filename << '\n';
}
