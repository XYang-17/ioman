#include <test_support.h>
#include <ioman/ioman.h>
#include <ioman/scheduler.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using testing::check;
using testing::throws;

using ioman::Future;
using ioman::Promise;
using ioman::TaskCancelled;
using ioman::TimeoutError;
using ioman::WaitMode;

constexpr std::chrono::seconds test_timeout{5};
constexpr std::chrono::milliseconds immediate{0};
constexpr std::chrono::hours long_wait{1};

void test_cancel_queued_and_suspended() {
    Promise<void> release;
    const auto gate = release.get_future();
    std::atomic<bool> ran{false};
    std::promise<void> entered;
    auto started = entered.get_future();
    ioman::ioman loop;
    const auto blocker = loop.submit_blocking([&] {
        entered.set_value();
        gate.get();
    });
    check(std::future_status::ready == started.wait_for(test_timeout), "Pool did not start");
    const auto queued = loop.submit_blocking_task([&] { ran.store(true); });
    check(queued.cancel(), "Queued task rejected cancellation");
    check(!queued.cancel(), "Completed cancellation could be requested again");
    throws<TaskCancelled>([&] { queued.get(); }, "Queued cancellation was not published");
    release.set_value();
    blocker.get();

    Promise<void> never;
    const auto pending = never.get_future();
    const auto suspended = loop.submit_task([pending] { pending.get(); });
    loop.submit([] {}).get();
    check(suspended.cancel(), "Suspended task rejected cancellation");
    throws<TaskCancelled>([&] { suspended.get(); }, "Cancellation did not wake the coroutine");
    check(!ran.load(), "Cancelled queued task executed");
}

void test_wait_for_and_shield() {
    Promise<int> release;
    const auto gate = release.get_future();
    ioman::ioman loop;
    const auto protected_task = loop.submit_task([gate] { return gate.get(); });
    loop.submit([] {}).get();

    throws<TimeoutError>(
        [&] { loop.wait_for(ioman::shield(protected_task), immediate); },
        "Protected wait did not time out"
    );
    check(!protected_task.is_ready(), "Protected task was cancelled by the timeout");
    release.set_value(21);
    check(21 == protected_task.get(), "Protected task could not finish");

    const auto cancellable = loop.submit_task([&] { loop.sleep_for(long_wait); });
    loop.submit([] {}).get();
    throws<TimeoutError>(
        [&] { loop.wait_for(cancellable, immediate); }, "Task wait did not report its timeout"
    );
    check(cancellable.is_ready(), "Timed out task did not finish its cancellation");
    throws<TaskCancelled>([&] { cancellable.get(); }, "Timed out task was not cancelled");
}

void test_nested_timeout_and_caller_cancellation() {
    Promise<void> release;
    const auto gate = release.get_future();
    ioman::ioman loop;
    const auto target = loop.submit_task([gate] { gate.get(); });
    loop.submit([] {}).get();

    const auto waiter = loop.submit_task([target] { ioman::shield(target).get(); });
    loop.submit([] {}).get();
    waiter.cancel();
    throws<TaskCancelled>([&] { waiter.get(); }, "Waiting task did not receive cancellation");
    check(!target.is_ready(), "Shield failed to protect the target");
    release.set_value();
    target.get();

    const auto nested = loop.submit([&] {
        throws<TimeoutError>([&] {
            loop.timeout(std::chrono::milliseconds(10), [&] {
                loop.timeout(long_wait, [&] { loop.sleep_for(long_wait); });
            });
        }, "Outer timeout was not translated by its own scope");
        ioman::cancellation_point();
        return 12;
    });
    check(12 == nested.get(), "Expired scope leaked cancellation into its caller");

    Promise<void> dependency;
    const auto unavailable = dependency.get_future();
    const auto child = loop.submit_task([unavailable] { unavailable.get(); });
    const auto parent = loop.submit_task([child] { child.get(); });
    loop.submit([] {}).get();
    parent.cancel();
    throws<TaskCancelled>([&] { parent.get(); }, "Parent did not observe cancellation");
    throws<TaskCancelled>([&] { child.get(); }, "Unshielded task did not receive cancellation");
}

void test_wait_and_combinators() {
    Promise<int> first;
    Promise<int> second;
    Promise<int> third;
    ioman::ioman loop;
    const std::vector<Future<int>> inputs{
        first.get_future(), second.get_future(), third.get_future()
    };
    auto stream = loop.as_completed(inputs);
    const auto gathered = loop.gather(inputs);
    const auto settled = loop.gather_settled(inputs);

    second.set_value(20);
    const auto item = stream.next();
    check(item.has_value() && 1 == item->index, "Stream did not deliver the first completion");
    check(20 == item->future.get() && !gathered.is_ready(), "Aggregation completed too early");
    const auto partition = loop.wait(inputs, WaitMode::FIRST_COMPLETED);
    const std::vector<std::size_t> done{1};
    const std::vector<std::size_t> pending{0, 2};
    check(done == partition.done && pending == partition.pending, "wait returned the wrong partition");

    const auto deadline = ioman::ioman::Clock::now();
    const auto timed = loop.wait(inputs, WaitMode::ALL_COMPLETED, deadline);
    check(done == timed.done && pending == timed.pending, "Timeout did not preserve pending tasks");
    first.set_value(10);
    third.set_value(30);
    const std::vector<int> expected{10, 20, 30};
    check(expected == gathered.get(), "gather did not preserve input order");
    check(3 == settled.get().size(), "gather_settled lost an input");
    const auto next = stream.next();
    const auto last = stream.next();
    check(next.has_value() && 0 == next->index, "First remaining completion was missing");
    check(last.has_value() && 2 == last->index, "Last completion was missing");
    check(!stream.next().has_value(), "Stream did not terminate");
}

void test_gather_failure_and_task_group() {
    struct JobError {};
    Promise<void> failed;
    Promise<void> unrelated;
    const auto failure = failed.get_future();
    const auto pending = unrelated.get_future();
    ioman::ioman loop;
    const std::vector<Future<void>> inputs{failure, pending};
    const auto gathered = loop.gather(inputs);
    const auto settled = loop.gather_settled(inputs);

    failed.set_exception(std::make_exception_ptr(JobError{}));
    throws<JobError>([&] { gathered.get(); }, "gather did not propagate the first failure");
    check(!settled.is_ready() && !pending.is_ready(), "gather affected an unrelated input");
    unrelated.set_value();
    const auto results = settled.get();
    throws<JobError>([&] { results.front().get(); }, "Settled results lost the failure");
    results.back().get();

    Promise<void> start_failure;
    const auto gate = start_failure.get_future();
    auto group = loop.task_group();
    const auto sibling = group.spawn([&] { loop.sleep_for(long_wait); });
    group.spawn_blocking([gate] { gate.get(); throw JobError{}; });
    loop.submit([] {}).get();
    start_failure.set_value();
    throws<JobError>([&] { group.join(); }, "Task group did not report the failure");
    throws<TaskCancelled>([&] { sibling.get(); }, "Task group did not cancel its sibling");
}

void test_shutdown_preserves_active_timers() {
    ioman::ioman loop;
    const auto sleeper = loop.submit([] {
        ioman::sleep_for(std::chrono::milliseconds{5});
        return 42;
    });
    loop.submit([] {}).get();
    loop.shutdown();
    check(42 == sleeper.get(), "Shutdown interrupted an already started timer");
    throws<std::logic_error>([&] { loop.submit([] {}); }, "Shutdown allowed a new task");
}

void test_thread_default_runtime() {
    ioman::shutdown();
    check(42 == ioman::submit([] { return 42; }).get(), "Default runtime did not execute a task");
    throws<std::logic_error>(
        [] { ioman::ioman duplicate; }, "Explicit runtime coexisted with the thread default"
    );
    {
        auto group = ioman::task_group();
        const auto task = group.spawn([] { return 17; });
        group.join();
        check(17 == task.get(), "Free task_group() did not retain its runtime");
    }
    ioman::shutdown();

    ioman::ioman runtime;
    const auto worker = runtime.submit([] { return ioman::Scheduler::current(); }).get();
    check(worker == ioman::submit([] { return ioman::Scheduler::current(); }).get(),
          "Free submit() did not use the explicit runtime");
    throws<std::logic_error>(
        [] { ioman::ioman duplicate; }, "A thread created a second explicit runtime"
    );

    // A foreign thread may close the explicit instance. The owner can then start another runtime.
    auto close = std::async(std::launch::async, [&] { runtime.shutdown(); });
    close.get();
    check(43 == ioman::submit([] { return 43; }).get(), "Closed binding prevented a new default");
    ioman::shutdown();
}

void test_worker_routing_and_explicit_cross_thread_submit() {
    std::promise<ioman::ioman*> published;
    auto other = published.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    auto other_owner = std::async(std::launch::async, [&] {
        ioman::ioman remote;
        const auto worker = remote.submit([] { return ioman::Scheduler::current(); }).get();
        check(worker == ioman::submit([] { return ioman::Scheduler::current(); }).get(),
              "Remote owner did not use its own runtime");
        published.set_value(&remote);
        check(std::future_status::ready == released.wait_for(test_timeout),
              "Remote owner's release timed out");
    });
    ioman::ioman local;
    check(std::future_status::ready == other.wait_for(test_timeout), "Remote owner did not start");
    auto* const remote = other.get();
    const auto remote_worker = remote->submit([] { return ioman::Scheduler::current(); }).get();
    const auto local_worker = local.submit([] { return ioman::Scheduler::current(); }).get();
    check(local_worker != remote_worker, "Two owner threads shared a single scheduler");

    const auto from_worker = local.submit([=] {
        throws<std::logic_error>(
            [] { ioman::ioman duplicate; }, "Scheduler worker created another runtime"
        );
        throws<std::logic_error>(
            [] { ioman::shutdown(); }, "Scheduler worker shut down its own runtime"
        );
        const auto implicit = ioman::submit([] { return ioman::Scheduler::current(); });
        const auto explicit_remote = remote->submit([] { return ioman::Scheduler::current(); });
        check(local_worker == implicit.get(), "Worker's implicit submission left its runtime");
        check(remote_worker == explicit_remote.get(), "Worker's explicit remote submission was rerouted");
    });

    const auto from_pool = local.submit_blocking([=] {
        throws<std::logic_error>(
            [] { ioman::ioman duplicate; }, "Thread-pool worker created another runtime"
        );
        const auto implicit = ioman::submit_task([] { return ioman::Scheduler::current(); });
        check(local_worker == implicit.get(), "Thread-pool worker submitted to another runtime");
        ioman::timeout(test_timeout, [] { ioman::sleep_for(immediate); });
    });
    from_worker.get();
    from_pool.get();
    release.set_value();
    other_owner.get();
}

void test_free_functions_while_runtime_is_closing() {
    struct CopyFails {
        CopyFails() = default;
        CopyFails(const CopyFails&) {
            throw std::runtime_error("Closing ioman tried to copy the callable");
        }
        void operator()() const {}
    };

    Promise<void> release;
    const auto gate = release.get_future();
    ioman::ioman loop;
    const auto blocker = loop.submit_blocking([gate] { gate.get(); });
    const auto running = loop.submit([gate] {
        gate.get();
        throws<std::logic_error>(
            [] { ioman::submit([] {}); }, "Managed thread replaced its closing runtime"
        );
        ioman::sleep_for(std::chrono::milliseconds{1});
    });
    loop.submit([] {}).get();
    const auto pending = loop.submit_blocking([] { return 1; });
    auto close = std::async(std::launch::async, [&] { loop.shutdown(); });
    throws<TaskCancelled>([&] { pending.get(); }, "Shutdown did not cancel the pending task");
    const CopyFails callable;
    throws<std::logic_error>(
        [&] { loop.submit(callable); }, "Closing ioman copied a coroutine job before rejecting it"
    );
    throws<std::logic_error>(
        [&] { loop.submit_blocking(callable); }, "Closing ioman copied a pool job before rejecting it"
    );
    release.set_value();
    running.get();
    blocker.get();
    close.get();
}

} // namespace

int main() {
    testing::Report report(__FILE__);
    std::promise<void> finished;
    auto completion = finished.get_future();
    std::thread watchdog([&] {
        constexpr std::chrono::seconds suite_timeout{40};
        if (std::future_status::timeout == completion.wait_for(suite_timeout)) {
            std::cerr << "[FAIL] test_ioman timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
    });

    struct TestCase {
        const char* name;
        void (*run)();
    };
    const TestCase tests[] = {
        {"cancel_queued_and_suspended", test_cancel_queued_and_suspended},
        {"wait_for_and_shield", test_wait_for_and_shield},
        {"nested_timeout_and_caller_cancellation", test_nested_timeout_and_caller_cancellation},
        {"wait_and_combinators", test_wait_and_combinators},
        {"gather_failure_and_task_group", test_gather_failure_and_task_group},
        {"shutdown_preserves_active_timers", test_shutdown_preserves_active_timers},
        {"thread_default_runtime", test_thread_default_runtime},
        {"worker_routing_and_explicit_cross_thread_submit", test_worker_routing_and_explicit_cross_thread_submit},
        {"free_functions_while_runtime_is_closing", test_free_functions_while_runtime_is_closing},
    };
    bool passed = true;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            passed = false;
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unexpected exception\n";
            passed = false;
        }
    }
    finished.set_value();
    watchdog.join();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
