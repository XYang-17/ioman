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

void invalid_calls_and_ownership() {
    ioman::Mutex mutex;
    throws<std::logic_error>([&] { mutex.lock(); });
    throws<std::logic_error>([&] { mutex.try_lock(); });
    throws<std::logic_error>([&] { mutex.unlock(); });
    ioman::ioman loop;
    loop.submit([&] {
        mutex.lock();
        throws<std::logic_error>([&] { mutex.lock(); });
        check(!mutex.try_lock(), "recursive try lock");
        auto other = ioman::submit([&] { throws<std::logic_error>([&] { mutex.unlock(); }); });
        other.get();
        mutex.unlock();
    }).get();
}

void contention_matrix() {
    for (const auto workers : {1U, 3U}) {
        for (const auto count : {1, 48}) {
            ioman::Mutex mutex;
            int value = 0;
            ioman::ioman loop(workers);
            std::vector<ioman::Future<void>> tasks;
            for (int task_index = 0; task_index < count; ++task_index) tasks.push_back(loop.submit([&] {
                for (int step = 0; step < 20; ++step) {
                    std::unique_lock<ioman::Mutex> lock(mutex);
                    const int old = value;
                    ioman::yield_now();
                    value = old + 1;
                }
            }));
            for (auto& task : tasks) task.get();
            check(value == count * 20, "mutual exclusion");
        }
    }
}

void cancel_waiter_then_reuse() {
    ioman::Mutex mutex;
    ioman::ioman loop;
    loop.submit([&] {
        mutex.lock();
        auto waiter = ioman::submit_task([&] { std::lock_guard<ioman::Mutex> lock(mutex); });
        ioman::yield_now();
        waiter.cancel();
        throws<ioman::TaskCancelled>([&] { waiter.get(); });
        mutex.unlock();
        check(mutex.try_lock(), "lock leaked after cancellation");
        mutex.unlock();
    }).get();
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"invalid_calls_and_ownership", invalid_calls_and_ownership},
        {"contention_matrix", contention_matrix},
        {"cancel_waiter_then_reuse", cancel_waiter_then_reuse},
    });
}
