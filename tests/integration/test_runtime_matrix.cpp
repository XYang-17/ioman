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

void execution_matrix() {
    for (const auto scheduler_workers : {1U, 3U}) {
        for (const auto pool_workers : {1U, 3U}) {
            for (const int count : {0, 1, 64}) {
                ioman::ioman loop(scheduler_workers, pool_workers);
                std::vector<ioman::Future<int>> tasks;
                for (int index = 0; index < count; ++index) tasks.push_back(loop.submit([index] {
                    auto blocking = ioman::submit_blocking([index] { return index * 2; });
                    auto child = ioman::submit([index] { ioman::yield_now();
                        return index; });
                    return blocking.get() + child.get();
                }));
                auto values = ioman::gather(tasks).get();
                int sum = 0;
                for (const int value : values) sum += value;
                check(sum == 3 * count * (count - 1) / 2, "mixed result mismatch");
                loop.shutdown();
                throws<std::logic_error>([&] { loop.submit([] {}); });
            }
        }
    }
}

void cross_instance_future() {
    ioman::Promise<int> producer;
    auto future = producer.get_future();
    auto remote = std::async(std::launch::async, [future] {
        ioman::ioman loop(2, 2);
        return loop.submit([future] { return future.get() + 1; }).get();
    });
    ioman::ioman loop;
    loop.submit_blocking([&] { producer.set_value(41); }).get();
    check(remote.get() == 42, "cross runtime wakeup");
}

void concurrent_submitters() {
    ioman::ioman loop(3, 2);
    std::atomic<int> sum{0};
    std::vector<std::thread> submitters;
    for (int submitter_index = 0; submitter_index < 4; ++submitter_index) submitters.emplace_back([&] {
        for (int task_index = 0; task_index < 32; ++task_index) {
            auto task = task_index % 2 == 0 ? loop.submit([] { return 1; }) : loop.submit_blocking([] { return 1; });
            sum += task.get();
        }
    });
    for (auto& thread : submitters) thread.join();
    check(sum == 128, "concurrent submissions");
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"execution_matrix", execution_matrix},
        {"cross_instance_future", cross_instance_future},
        {"concurrent_submitters", concurrent_submitters},
    });
}
