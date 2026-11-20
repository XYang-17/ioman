#include <test_support.h>
#include <ioman/ioman.h>
#include <ioman/scheduler.h>

#include <atomic>

namespace {
using testing::check;
using testing::throws;
using namespace std::chrono_literals;

void cancel_versus_completion() {
    for (const auto workers : {1U, 3U}) {
        ioman::ioman loop(workers, 2);
        for (int round = 0; round < 32; ++round) {
            ioman::Promise<int> producer;
            const auto future = producer.get_future();
            const auto task = loop.submit_task([future] { return future.get(); });
            std::promise<void> begin;
            const auto gate = begin.get_future().share();
            std::thread publisher([&] { gate.wait();
                producer.set_value(42); });
            std::thread canceller([&] { gate.wait();
                task.cancel(); });
            begin.set_value();
            publisher.join();
            canceller.join();
            try { check(task.get() == 42, "incorrect race value"); }
            catch (const ioman::TaskCancelled&) {}
            check(task.is_ready(), "race task incomplete");
        }
        check(loop.submit([] { return 9; }).get() == 9, "runtime unusable");
    }
}

void timeout_versus_completion() {
    ioman::ioman loop;
    for (int round = 0; round < 32; ++round) {
        ioman::Promise<int> producer;
        auto future = producer.get_future();
        std::thread publisher([&] { producer.set_value(19); });
        try { check(loop.wait_for(future, 0ms) == 19, "incorrect timed value"); }
        catch (const ioman::TimeoutError&) {}
        publisher.join();
        check(future.get() == 19, "timeout damaged result");
    }
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"cancel_versus_completion", cancel_versus_completion},
        {"timeout_versus_completion", timeout_versus_completion},
    });
}
