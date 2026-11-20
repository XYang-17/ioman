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

void shared_value() {
    ioman::Promise<int> producer;
    auto future = producer.get_future();
    auto copy = future;
    check(!future.is_ready(), "premature readiness");
    producer.set_value(42);
    copy = future;
    check(copy.get() == 42 && future.get() == 42 && future.is_ready(), "shared result");
}

void exception_and_void() {
    ioman::Promise<int> producer;
    auto future = producer.get_future();
    producer.set_exception(std::make_exception_ptr(std::runtime_error("failure")));
    future.wait();
    check(future.exception() != nullptr, "missing error");
    throws<std::runtime_error>([&] { future.get(); });
    ioman::Promise<void> done;
    auto signal = done.get_future();
    done.set_value();
    signal.get();
}

void reference_and_move_only() {
    int value = 3;
    ioman::Promise<int&> ref;
    auto future = ref.get_future();
    ref.set_value(value);
    check(&future.get() == &value, "reference identity");
    ioman::Promise<std::unique_ptr<int>> producer;
    auto result = producer.get_future();
    auto copy = result;
    producer.set_value(std::make_unique<int>(9));
    check(*result.get() == 9, "move result");
    throws<std::future_error>([&] { copy.get(); });
}

void subscription_lifetime() {
    ioman::Promise<int> producer;
    auto future = producer.get_future();
    int calls = 0;
    const auto retained = future.on_ready([&] { ++calls; });
    { const auto dropped = future.on_ready([&] { calls += 100; }); }
    producer.set_value(1);
    check(calls == 1, "expired callback ran");
    const auto late = future.on_ready([&] { ++calls; });
    check(calls == 2, "late subscription");
    throws<std::invalid_argument>([&] { future.on_ready({}); });
}

void multiple_thread_waiters() {
    ioman::Promise<int> producer;
    const auto future = producer.get_future();
    std::atomic<int> sum{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) threads.emplace_back([&] { sum += future.get(); });
    producer.set_value(17);
    for (auto& thread : threads) thread.join();
    check(sum == 136, "thread waiters lost wakeup");
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"shared_value", shared_value},
        {"exception_and_void", exception_and_void},
        {"reference_and_move_only", reference_and_move_only},
        {"subscription_lifetime", subscription_lifetime},
        {"multiple_thread_waiters", multiple_thread_waiters},
    });
}
