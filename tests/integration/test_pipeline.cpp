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

void bounded_pipeline() {
    for (const auto workers : {1U, 3U}) {
        ioman::Mutex mutex;
        ioman::ConditionVariable readable, writable;
        std::deque<int> queue;
        bool closed = false;
        ioman::WaitGroup produced;
        produced.add(2);
        std::atomic<int> sum{0}, count{0};
        ioman::ioman loop(workers, 2);
        std::vector<ioman::Future<void>> consumers, producers;
        for (int consumer_index = 0; consumer_index < 3; ++consumer_index) consumers.push_back(loop.submit([&] {
            for (;;) {
                std::unique_lock<ioman::Mutex> lock(mutex);
                readable.wait(lock, [&] { return closed || !queue.empty(); });
                if (queue.empty()) break;
                const int value = queue.front();
                queue.pop_front();
                writable.notify_one();
                lock.unlock();
                sum += value;
                ++count;
                ioman::yield_now();
            }
        }));
        for (int producer_index = 0; producer_index < 2; ++producer_index) producers.push_back(loop.submit([&, producer_index] {
            for (int value = producer_index + 1; value <= 100; value += 2) {
                std::unique_lock<ioman::Mutex> lock(mutex);
                writable.wait(lock, [&] { return queue.size() < 3; });
                queue.push_back(value);
                readable.notify_one();
            }
            produced.done();
        }));
        produced.wait();
        for (auto& producer : producers) producer.get();
        loop.submit([&] { std::lock_guard<ioman::Mutex> lock(mutex);
            closed = true;
            readable.notify_all(); }).get();
        for (auto& consumer : consumers) consumer.get();
        check(sum == 5050 && count == 100 && queue.empty(), "pipeline lost data");
    }
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"bounded_pipeline", bounded_pipeline},
    });
}
