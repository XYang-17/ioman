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

void invalid_lock() {
    ioman::Mutex mutex;
    ioman::ConditionVariable condition;
    std::unique_lock<ioman::Mutex> lock(mutex, std::defer_lock);
    throws<std::logic_error>([&] { condition.wait(lock); });
    throws<std::logic_error>([&] { condition.wait(lock, [] { return true; }); });
    throws<std::logic_error>([&] { condition.wait(mutex); });
    condition.notify_one();
    condition.notify_all();
}

void predicate_notify_all() {
    for (const auto workers : {1U, 3U}) {
        ioman::Mutex mutex;
        ioman::ConditionVariable condition;
        bool ready = false;
        ioman::ioman loop(workers);
        std::vector<ioman::Future<void>> readers;
        for (int index = 0; index < 24; ++index) readers.push_back(loop.submit([&] {
            std::unique_lock<ioman::Mutex> lock(mutex);
            condition.wait(lock, [&] { return ready; });
            check(lock.owns_lock() && ready, "predicate contract");
        }));
        loop.submit([&] { std::lock_guard<ioman::Mutex> lock(mutex);
            ready = true;
            condition.notify_all(); }).get();
        for (auto& reader : readers) reader.get();
    }
}

void raw_wait_notify_one() {
    ioman::Mutex mutex;
    ioman::ConditionVariable condition;
    ioman::ioman loop;
    auto reader = loop.submit([&] {
        mutex.lock();
        condition.wait(mutex);
        check(!mutex.try_lock(), "ownership not restored");
        mutex.unlock();
    });
    loop.submit([] {}).get();
    condition.notify_one();
    reader.get();
}

void cancel_reacquires_lock() {
    ioman::Mutex mutex;
    ioman::ConditionVariable condition;
    ioman::ioman loop;
    std::atomic<bool> held{false};
    auto reader = loop.submit_task([&] {
        std::unique_lock<ioman::Mutex> lock(mutex);
        try { condition.wait(lock); }
        catch (const ioman::TaskCancelled&) {
            held = lock.owns_lock() && !mutex.try_lock();
            throw;
        }
    });
    loop.submit([] {}).get();
    reader.cancel();
    throws<ioman::TaskCancelled>([&] { reader.get(); });
    check(held, "cancellation lost lock");
    condition.notify_all();
    loop.submit([&] { std::lock_guard<ioman::Mutex> lock(mutex); }).get();
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"invalid_lock", invalid_lock},
        {"predicate_notify_all", predicate_notify_all},
        {"raw_wait_notify_one", raw_wait_notify_one},
        {"cancel_reacquires_lock", cancel_reacquires_lock},
    });
}
