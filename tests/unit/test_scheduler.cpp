#include <test_support.h>
#include <ioman/scheduler.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using testing::check;
using testing::throws;

using ioman::Coroutine;
using ioman::CoroutineState;
using ioman::Scheduler;
using ioman::TaskHandle;

constexpr std::chrono::seconds test_timeout{5};

// void check(const bool condition, const char* const message) {
//     if (!condition) {
//         throw std::runtime_error(message);
//     }
// }
// 
// template <typename Exception, typename Operation>
// void throws(const Operation& operation, const char* const message) {
//     try {
//         operation();
//     } catch (const Exception&) {
//         return;
//     }
//     throw std::runtime_error(message);
// }

template <typename Future>
decltype(auto) await(Future& future, const char* const message) {
    check(std::future_status::ready == future.wait_for(test_timeout), message);
    return future.get();
}

// 使用公开状态确认已进入指定分支，无需猜测 worker 的调度时机。
void await_suspended(const TaskHandle& task) {
    const auto deadline = std::chrono::steady_clock::now() + test_timeout;
    while (CoroutineState::SUSPENDED != task.state()) {
        check(!task.done(), "Task finished instead of suspending");
        check(std::chrono::steady_clock::now() < deadline, "Task did not suspend");
        std::this_thread::yield();
    }
}

void test_lifecycle_and_invalid_calls() {
    struct CopyFails {
        CopyFails() = default;
        CopyFails(const CopyFails&) {
            throw std::runtime_error("Closed scheduler tried to copy the callable");
        }
        void operator()() const {}
    };

    throws<std::invalid_argument>([] { Scheduler scheduler(0); }, "Accepted zero workers");
    check(nullptr == Scheduler::current(), "Caller has a scheduler");

    Scheduler scheduler;
    check(scheduler.is_closed(), "Scheduler reported open before start");
    scheduler.stop();
    throws<std::logic_error>(
        [&] { scheduler.post([] {}); }, "Accepted a task before start"
    );
    throws<std::logic_error>(
        [&] { scheduler.suspend_current(); }, "Allowed suspension from an external thread"
    );

    scheduler.start();
    scheduler.start();
    check(!scheduler.is_closed(), "Started scheduler reported closed");
    throws<std::invalid_argument>(
        [&] { scheduler.post({}); }, "Accepted an empty task"
    );
    throws<std::invalid_argument>(
        [&] { scheduler.post_coroutine(nullptr); }, "Accepted a null coroutine"
    );

    const TaskHandle task = scheduler.post([&] {
        check(&scheduler == Scheduler::current(), "Incorrect current scheduler");
        check(nullptr != Coroutine::current(), "Worker did not enter the coroutine");
        throws<std::logic_error>(
            [&] { scheduler.stop(); }, "Allowed a worker to join itself"
        );
    });
    task.wait();
    check(task.done() && CoroutineState::DEAD == task.state(), "Task did not complete");
    scheduler.stop();
    scheduler.stop();
    check(scheduler.is_closed(), "Stopped scheduler reported open");
    throws<std::logic_error>(
        [&] { scheduler.post([] {}); }, "Accepted a task after stop"
    );
    const CopyFails callable;
    throws<std::logic_error>(
        [&] { scheduler.post(callable); }, "Closed scheduler copied a callable before rejecting it"
    );

    scheduler.start();
    check(!scheduler.is_closed(), "Restart did not reopen the scheduler");
    scheduler.post([] {}).wait();
    check(0 == scheduler.pending_count(), "Restart left tasks in the ready queue");
}

void test_post_rechecks_after_concurrent_stop() {
    std::promise<void> copied;
    auto copy_started = copied.get_future();
    std::promise<void> release;
    const auto gate = release.get_future().share();
    struct BlockingCopy {
        std::promise<void>& copied;
        const std::shared_future<void>& gate;

        BlockingCopy(std::promise<void>& started, const std::shared_future<void>& ready)
            : copied(started), gate(ready) {}

        BlockingCopy(const BlockingCopy& other) : copied(other.copied), gate(other.gate) {
            copied.set_value();
            await(gate, "Caller did not release the callable copy");
        }

        void operator()() const {}
    };

    const BlockingCopy callable(copied, gate);
    Scheduler scheduler;
    scheduler.start();
    auto posting = std::async(std::launch::async, [&] {
        throws<std::logic_error>(
            [&] { scheduler.post(callable); }, "Post did not recheck after concurrent stop"
        );
    });
    await(copy_started, "Scheduler did not start packaging the callable");
    scheduler.stop();
    release.set_value();
    await(posting, "Posting did not finish after scheduler stop");
    check(0 == scheduler.pending_count(), "A concurrently rejected task remained queued");
}

void test_yield_returns_to_queue_tail() {
    std::vector<int> order;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    Scheduler scheduler;
    scheduler.start();

    const TaskHandle first = scheduler.post([&] {
        order.push_back(1);
        entered.set_value();
        await(release_future, "Caller did not release the first task");
        Coroutine::current()->yield();
        order.push_back(4);
    });

    await(entered_future, "First task did not start");
    const TaskHandle second = scheduler.post([&] { order.push_back(2); });
    const TaskHandle third = scheduler.post([&] { order.push_back(3); });
    check(CoroutineState::RUNNING == first.state(), "First task was not running");
    check(CoroutineState::READY == second.state(), "Second task was not ready");
    check(2 == scheduler.pending_count(), "Pending count did not match the ready queue");

    release.set_value();
    first.wait();
    second.wait();
    third.wait();
    const std::vector<int> expected{1, 2, 3, 4};
    check(expected == order, "Yield did not put the task behind existing ready tasks");
}

void test_cross_thread_wake_and_early_wake() {
    std::promise<Coroutine*> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    std::atomic<int> resumes{0};
    Scheduler scheduler;
    scheduler.start();

    const TaskHandle task = scheduler.post([&] {
        entered.set_value(Coroutine::current());
        await(release_future, "Caller did not release the task");
        scheduler.suspend_current();
        ++resumes;
        scheduler.suspend_current();
        ++resumes;
    });

    Coroutine* const coroutine = await(entered_future, "Task did not publish its coroutine");
    scheduler.resume_later(coroutine);
    scheduler.resume_later(coroutine);
    release.set_value();

    await_suspended(task);
    check(1 == resumes.load(), "Early wake was lost or reused for the second suspension");
    check(0 == scheduler.pending_count(), "Suspended task remained in the ready queue");
    scheduler.post([] {}).wait();
    check(1 == resumes.load(), "Suspended task ran without being woken");

    scheduler.resume_later(coroutine);
    task.wait();
    check(2 == resumes.load(), "Cross-thread wake did not resume the second suspension");
}

void test_multiple_workers_and_affinity() {
    std::promise<std::thread::id> first_entered;
    auto first_future = first_entered.get_future();
    std::promise<std::thread::id> second_entered;
    auto second_future = second_entered.get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    Scheduler scheduler(2);
    scheduler.start();

    const auto run = [&](std::promise<std::thread::id>& entered) {
        const auto initial_thread = std::this_thread::get_id();
        entered.set_value(initial_thread);
        await(release_future, "Caller did not release the workers");
        Coroutine::current()->yield();
        check(initial_thread == std::this_thread::get_id(), "Yield migrated the coroutine");
        check(&scheduler == Scheduler::current(), "Scheduler TLS changed after yield");
    };

    const TaskHandle first = scheduler.post([run, &first_entered] { run(first_entered); });
    const TaskHandle second = scheduler.post([run, &second_entered] { run(second_entered); });
    const auto first_thread = await(first_future, "First worker did not run");
    const auto second_thread = await(second_future, "Second worker did not run concurrently");
    check(first_thread != second_thread, "The two workers ran on the same thread");
    release.set_value();
    first.wait();
    second.wait();
}

void test_post_coroutine_releases_ownership() {
    struct Lifetime {
        explicit Lifetime(std::promise<void>& notification) : destroyed(notification) {}

        std::promise<void>& destroyed;

        ~Lifetime() {
            destroyed.set_value();
        }
    };

    std::promise<void> executed;
    auto executed_future = executed.get_future();
    std::promise<void> destroyed;
    auto destroyed_future = destroyed.get_future();
    Scheduler scheduler;
    scheduler.start();

    auto lifetime = std::make_shared<Lifetime>(destroyed);
    auto coroutine = std::make_unique<Coroutine>([lifetime, &executed] { executed.set_value(); });
    lifetime.reset();
    scheduler.post_coroutine(coroutine.release());
    await(executed_future, "Posted coroutine did not execute");
    await(destroyed_future, "Scheduler did not release the completed coroutine");
}

void test_same_worker_cooperative_wait() {
    std::promise<Coroutine*> entered;
    auto entered_future = entered.get_future();
    Scheduler scheduler;
    scheduler.start();

    const TaskHandle target = scheduler.post([&] {
        entered.set_value(Coroutine::current());
        scheduler.suspend_current();
    });
    Coroutine* const coroutine = await(entered_future, "Target did not start");
    await_suspended(target);

    const TaskHandle waiter = scheduler.post([target] { target.wait(); });
    await_suspended(waiter);
    // 第三个任务能在同一个 worker 上执行，说明 wait() 没有阻塞线程。
    scheduler.post([] {}).wait();
    scheduler.resume_later(coroutine);
    waiter.wait();
    check(target.done(), "Waiter returned before the target finished");
}

void test_stop_resumes_suspended_tasks() {
    std::atomic<bool> ran{false};
    Scheduler scheduler;
    scheduler.start();

    const TaskHandle suspended = scheduler.post([&] { scheduler.suspend_current(); });
    await_suspended(suspended);
    const TaskHandle ready = scheduler.post([&] { ran.store(true); });
    scheduler.stop();

    ready.wait();
    check(ran.load(), "Stop discarded a submitted task");
    check(suspended.done(), "Stop did not finish the suspended task");
    throws<std::runtime_error>(
        [&] { suspended.wait(); }, "Stop did not propagate the suspension error"
    );
}

void test_exception_callbacks_and_cross_scheduler_wait() {
    struct TaskError {};

    std::promise<Coroutine*> entered;
    auto entered_future = entered.get_future();
    std::promise<void> notified;
    auto notified_future = notified.get_future();
    Scheduler scheduler;
    Scheduler other;
    scheduler.start();
    other.start();

    const TaskHandle target = scheduler.post([&] {
        entered.set_value(Coroutine::current());
        scheduler.suspend_current();
        throw TaskError{};
    });
    Coroutine* const coroutine = await(entered_future, "Failing task did not start");
    await_suspended(target);

    target.on_done([] { throw std::runtime_error("Callback error"); });
    target.on_done([&] {
        check(&scheduler == Scheduler::current(), "Callback used the wrong scheduler");
        notified.set_value();
    });
    const TaskHandle waiter = other.post([target] {
        throws<TaskError>([&] { target.wait(); }, "Coroutine wait lost the task exception");
    });
    await_suspended(waiter);
    scheduler.resume_later(coroutine);

    throws<TaskError>([&] { target.wait(); }, "Thread wait lost the task exception");
    waiter.wait();
    await(notified_future, "An exception prevented a later callback from running");
    bool immediate = false;
    target.on_done([&] { immediate = true; });
    check(immediate, "Late callback was not run immediately");
    scheduler.post([] {}).wait();
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    testing::Report report(__FILE__);
    // 把调度回归造成的死锁变成有界失败，直接运行测试时也有效。
    std::promise<void> finished;
    auto finished_future = finished.get_future();
    std::thread watchdog([&] {
        constexpr std::chrono::seconds suite_timeout{45};
        if (std::future_status::timeout == finished_future.wait_for(suite_timeout)) {
            std::cerr << "[FAIL] test_scheduler timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
    });

    const TestCase tests[] = {
        {"lifecycle_and_invalid_calls", test_lifecycle_and_invalid_calls},
        {"post_rechecks_after_concurrent_stop", test_post_rechecks_after_concurrent_stop},
        {"yield_returns_to_queue_tail", test_yield_returns_to_queue_tail},
        {"cross_thread_wake_and_early_wake", test_cross_thread_wake_and_early_wake},
        {"multiple_workers_and_affinity", test_multiple_workers_and_affinity},
        {"post_coroutine_releases_ownership", test_post_coroutine_releases_ownership},
        {"same_worker_cooperative_wait", test_same_worker_cooperative_wait},
        {"stop_resumes_suspended_tasks", test_stop_resumes_suspended_tasks},
        {"exception_callbacks_and_cross_scheduler_wait", test_exception_callbacks_and_cross_scheduler_wait},
    };
    bool passed = true;
    for (const TestCase& test : tests) {
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
