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

void copy_cancel_and_shield() {
    ioman::Promise<void> producer;
    auto gate = producer.get_future();
    ioman::ioman loop;
    auto task = loop.submit_task([gate] { gate.wait();
        return 3; });
    auto copy = task;
    throws<ioman::TimeoutError>([&] { loop.wait_for(ioman::shield(copy), 0ms); });
    check(task.cancel(), "first cancellation");
    throws<ioman::TaskCancelled>([&] { copy.get(); });
    check(!task.cancel() && task.is_ready(), "completed cancellation");
}

void success_and_exception() {
    ioman::ioman loop;
    auto task = loop.submit_task([] { return 31; });
    task.wait();
    check(task.get() == 31 && task.future().get() == 31 && !task.cancel(), "successful task");
    loop.submit_blocking_task([] {}).get();
    auto failed = loop.submit_blocking_task([]() -> int { throw std::domain_error("bad"); });
    failed.wait();
    throws<std::domain_error>([&] { failed.get(); });
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"copy_cancel_and_shield", copy_cancel_and_shield},
        {"success_and_exception", success_and_exception},
    });
}
