#include <test_support.h>
#include <ioman/ioman.h>
#include <ioman/sync/mutex.h>
#include <ioman/sync/condition_variable.h>
#include <ioman/sync/wait_group.h>

#include <atomic>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

namespace {
using testing::check;
using testing::throws;
using namespace std::chrono_literals;

void single_publication() {
    ioman::Promise<int> producer;
    auto future = producer.get_future();
    throws<std::future_error>([&] { producer.get_future(); });
    producer.set_value(7);
    throws<std::future_error>([&] { producer.set_value(8); });
    throws<std::future_error>([&] { producer.set_exception(std::make_exception_ptr(std::runtime_error("x"))); });
    check(future.get() == 7, "original value changed");
}

void invalid_exception() {
    ioman::Promise<void> producer;
    auto future = producer.get_future();
    throws<std::invalid_argument>([&] { producer.set_exception(nullptr); });
    check(!future.is_ready(), "invalid exception completed promise");
    producer.set_value();
    future.get();
}

void broken_promise() {
    const auto future = [] { ioman::Promise<int> producer;
        return producer.get_future(); }();
    check(future.is_ready(), "broken promise not ready");
    try { future.get();
        throw std::runtime_error("missing broken promise"); }
    catch (const std::future_error& error) {
        check(error.code() == std::make_error_code(std::future_errc::broken_promise), "wrong future error");
    }
}

void publish_before_get() {
    ioman::Promise<int> producer;
    producer.set_value(23);
    check(producer.get_future().get() == 23, "early publication");
}

void concurrent_publishers() {
    ioman::Promise<int> producer;
    auto future = producer.get_future();
    std::atomic<int> winners{0}, losers{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) threads.emplace_back([&, index] {
        try { producer.set_value(index);
            ++winners; } catch (const std::future_error&) { ++losers; }
    });
    for (auto& thread : threads) thread.join();
    check(winners == 1 && losers == 7 && future.get() >= 0 && future.get() < 8, "publication race");
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"single_publication", single_publication},
        {"invalid_exception", invalid_exception},
        {"broken_promise", broken_promise},
        {"publish_before_get", publish_before_get},
        {"concurrent_publishers", concurrent_publishers},
    });
}
