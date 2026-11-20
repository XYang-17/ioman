#include <test_support.h>
#include <ioman/ioman.h>
#include <ioman/scheduler.h>

#include <atomic>

namespace {
using testing::check;
using testing::throws;
using namespace std::chrono_literals;

void completion_and_callbacks() {
    ioman::Scheduler scheduler;
    scheduler.start();
    ioman::Promise<void> producer;
    auto gate = producer.get_future();
    std::atomic<int> calls{0};
    auto handle = scheduler.post([gate] { gate.get(); });
    auto copy = handle;
    handle.on_done([&] { ++calls; });
    const auto subscription = handle.subscribe([&] { ++calls; });
    { const auto dropped = handle.subscribe([&] { calls += 100; }); }
    producer.set_value();
    copy.wait();
    scheduler.stop();
    check(handle.done() && handle.state() == ioman::CoroutineState::DEAD && !handle.error(), "completion state");
    check(calls == 2, "callback lifetime");
    handle.on_done([&] { ++calls; });
    check(calls == 3, "late callback");
    throws<std::invalid_argument>([&] { handle.subscribe({}); });
    throws<std::invalid_argument>([&] { handle.on_done({}); });
}

void failure_wait_modes() {
    ioman::Scheduler scheduler;
    scheduler.start();
    auto failed = scheduler.post([] { throw std::domain_error("bad"); });
    failed.wait_ready();
    check(failed.error() != nullptr, "lost exception");
    throws<std::domain_error>([&] { failed.wait(); });
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"completion_and_callbacks", completion_and_callbacks},
        {"failure_wait_modes", failure_wait_modes},
    });
}
