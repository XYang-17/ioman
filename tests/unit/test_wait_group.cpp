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

void zero_underflow_overflow() {
    ioman::WaitGroup group;
    group.wait();
    group.add(0);
    throws<std::logic_error>([&] { group.done(); });
    group.add(std::numeric_limits<int>::max());
    throws<std::overflow_error>([&] { group.add(1); });
    group.add(-std::numeric_limits<int>::max());
    group.wait();
}

void mixed_waiters_and_reuse() {
    ioman::WaitGroup group;
    ioman::ioman loop(2);
    for (int round = 0; round < 3; ++round) {
        group.add(1);
        std::vector<std::thread> threads;
        std::vector<ioman::Future<void>> tasks;
        std::atomic<int> finished{0};
        for (int index = 0; index < 4; ++index) threads.emplace_back([&] { group.wait(); ++finished; });
        for (int index = 0; index < 8; ++index) tasks.push_back(loop.submit([&] { group.wait(); ++finished; }));
        group.done();
        for (auto& thread : threads) thread.join();
        for (auto& task : tasks) task.get();
        check(finished == 12, "lost round notification");
    }
}

void cancel_preserves_count() {
    ioman::WaitGroup group;
    group.add(1);
    ioman::ioman loop;
    auto waiter = loop.submit_task([&] { group.wait(); });
    loop.submit([] {}).get();
    waiter.cancel();
    throws<ioman::TaskCancelled>([&] { waiter.get(); });
    group.done();
    group.wait();
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"zero_underflow_overflow", zero_underflow_overflow},
        {"mixed_waiters_and_reuse", mixed_waiters_and_reuse},
        {"cancel_preserves_count", cancel_preserves_count},
    });
}
