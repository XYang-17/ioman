#include <test_support.h>
#include <ioman/scheduler.h>
#include <ioman/thread_pool.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ioman::CoroutineState;
using ioman::Scheduler;
using ioman::TaskHandle;
using ioman::ThreadPool;

constexpr std::chrono::seconds test_timeout{5};

void check(const bool condition, const char* const message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Operation>
void check_throws(const Operation& operation, const char* const message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

template <typename Future>
decltype(auto) await(Future& future, const char* const message) {
    check(std::future_status::ready == future.wait_for(test_timeout), message);
    return future.get();
}

void await_suspended(const TaskHandle& task) {
    const auto deadline = std::chrono::steady_clock::now() + test_timeout;
    while (CoroutineState::SUSPENDED != task.state()) {
        check(!task.done(), "Scheduler task finished instead of waiting");
        check(std::chrono::steady_clock::now() < deadline, "Scheduler task did not suspend");
        std::this_thread::yield();
    }
}

void test_submit_results() {
    int number = 7;
    bool executed = false;
    ThreadPool pool(2);

    const auto text = pool.submit([] { return std::string("result"); });
    const auto copy = text;
    check("result" == text.get() && "result" == copy.get(), "Copied future lost the result");
    check(text.is_ready(), "Read result was not marked ready");

    const auto signal = pool.submit([&] { executed = true; });
    signal.get();
    check(executed, "Future<void> returned before the job was done");

    const auto reference = pool.submit([&]() -> int& { return number; });
    check(&number == &reference.get(), "Reference result did not preserve identity");
    reference.get() = 42;
    check(42 == number, "Reference result did not refer to the original value");
}

void test_move_only_callable_and_result() {
    ThreadPool pool(1);
    const auto result = pool.submit([value = std::make_unique<int>(23)]() mutable {
        return std::move(value);
    });
    const auto copy = result;
    const auto value = copy.get();
    check(nullptr != value && 23 == *value, "Move-only result was not delivered");
}

void test_task_exception_and_invalid_calls() {
    struct JobError {};

    check_throws<std::invalid_argument>([] { ThreadPool pool(0); }, "Accepted zero workers");
    ThreadPool pool(1);
    const auto failed = pool.submit([]() -> int { throw JobError{}; });
    check_throws<JobError>([&] { failed.get(); }, "Future did not rethrow the task exception");

    const auto self_shutdown = pool.submit([&] { pool.shutdown(); });
    check_throws<std::logic_error>(
        [&] { self_shutdown.get(); }, "A worker was allowed to shut down its own pool"
    );
    const auto next = pool.submit([] { return 19; });
    check(19 == next.get(), "The worker did not continue after a task exception");
}

void test_parallel_workers() {
    std::promise<std::thread::id> first_started;
    std::promise<std::thread::id> second_started;
    auto first_thread = first_started.get_future();
    auto second_thread = second_started.get_future();
    std::promise<void> release;
    const auto gate = release.get_future().share();
    const auto operation = [gate](std::promise<std::thread::id>& started) {
        started.set_value(std::this_thread::get_id());
        await(gate, "Caller did not release the pool workers");
    };

    ThreadPool pool(2);
    const auto first = pool.submit([&] { operation(first_started); });
    const auto second = pool.submit([&] { operation(second_started); });
    // 等两个任务都进入后才放行，确认它们可以同时执行。
    const auto first_id = await(first_thread, "First worker did not start");
    const auto second_id = await(second_thread, "Second worker did not start concurrently");
    check(first_id != second_id, "Two simultaneous jobs used the same thread");
    release.set_value();
    first.get();
    second.get();
}

void test_concurrent_submissions_respect_worker_limit() {
    constexpr std::size_t worker_limit = 3;
    constexpr std::size_t submitter_count = 8;
    std::promise<void> submit;
    const auto submit_gate = submit.get_future().share();
    std::promise<void> release;
    const auto task_gate = release.get_future().share();
    std::promise<void> reached_limit;
    auto limit_reached = reached_limit.get_future();
    std::atomic<std::size_t> running{0};
    ThreadPool pool(worker_limit);

    using Result = ioman::Future<std::thread::id>;
    std::vector<std::future<Result>> submitters;
    for (std::size_t index = 0; index < submitter_count; ++index) {
        submitters.push_back(std::async(std::launch::async, [&] {
            await(submit_gate, "Caller did not release the submitters");
            return pool.submit([&] {
                if (worker_limit == ++running) {
                    reached_limit.set_value();
                }
                await(task_gate, "Caller did not release the tasks");
                return std::this_thread::get_id();
            });
        }));
    }
    submit.set_value();

    std::vector<Result> results;
    for (auto& submitter : submitters) {
        results.push_back(await(submitter, "Concurrent submission did not finish"));
    }
    await(limit_reached, "Pool did not expand to cover queued work");
    release.set_value();

    std::set<std::thread::id> workers;
    for (const auto& result : results) {
        workers.insert(result.get());
    }
    check(worker_limit == workers.size(), "Concurrent submissions violated the worker limit");
}

void test_shutdown_before_first_submission() {
    struct CopyFails {
        CopyFails() = default;
        CopyFails(const CopyFails&) {
            throw std::runtime_error("Closed pool tried to copy the callable");
        }
        void operator()() const {}
    };

    ThreadPool pool(4);
    check(!pool.is_closed(), "Pool without workers reported closed");
    pool.shutdown();
    pool.shutdown();
    check(pool.is_closed(), "Shut down pool reported open");
    check_throws<std::logic_error>([&] { pool.submit([] {}); }, "Closed idle pool accepted a job");
    const CopyFails callable;
    check_throws<std::logic_error>(
        [&] { pool.submit(callable); }, "Closed pool did not reject before packaging the task"
    );
}

void test_first_submission_races_shutdown() {
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<bool> ran{false};
    ThreadPool pool(4);
    auto submitter = std::async(std::launch::async, [&] {
        await(gate, "Caller did not release the submitter");
        try {
            pool.submit([&] { ran.store(true); }).get();
            return true;
        } catch (const std::logic_error&) {
            return false;
        }
    });
    auto closer = std::async(std::launch::async, [&] {
        await(gate, "Caller did not release the closer");
        pool.shutdown();
    });
    release.set_value();
    const bool accepted = await(submitter, "Initial submission did not settle during shutdown");
    await(closer, "Shutdown did not settle during initial submission");
    check(accepted == ran.load(), "Shutdown lost an accepted job or ran a rejected job");
}

void test_shutdown_drains_ready_queue() {
    std::vector<int> order;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    const auto gate = release.get_future().share();
    ThreadPool pool(1);

    const auto first = pool.submit([&] {
        started.set_value();
        await(gate, "Caller did not release the running job");
        order.push_back(1);
    });
    await(started_future, "First job did not start");
    const auto second = pool.submit([&] { order.push_back(2); });
    const auto third = pool.submit([&] { order.push_back(3); });
    check(!first.is_ready() && !second.is_ready() && !third.is_ready(), "Jobs completed too early");

    auto closing = std::async(std::launch::async, [&] { pool.shutdown(); });
    auto also_closing = std::async(std::launch::async, [&] { pool.shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + test_timeout;
    bool rejected = false;
    // 首个任务仍占用唯一 worker，拒绝投递表示关闭已经开始。
    while (!rejected && std::chrono::steady_clock::now() < deadline) {
        try {
            pool.submit([] {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } catch (const std::logic_error&) {
            check(pool.is_closed(), "Pool reported open after rejecting a submission");
            rejected = true;
        }
    }

    constexpr std::chrono::seconds no_wait{0};
    const bool still_closing = std::future_status::timeout == closing.wait_for(no_wait) &&
        std::future_status::timeout == also_closing.wait_for(no_wait);
    release.set_value();
    await(closing, "Shutdown did not finish");
    await(also_closing, "Concurrent shutdown did not finish");
    check(rejected, "Pool did not reject submissions during shutdown");
    check(still_closing, "Shutdown returned while a job was still running");
    check(first.is_ready() && second.is_ready() && third.is_ready(), "Shutdown left pending jobs");
    first.get();
    second.get();
    third.get();
    const std::vector<int> expected{1, 2, 3};
    check(expected == order, "Shutdown did not execute all accepted jobs in FIFO order");
    pool.shutdown();
    check_throws<std::logic_error>([&] { pool.submit([] {}); }, "Accepted a job after shutdown");
}

void test_destructor_drains_queue() {
    const auto result = [] {
        ThreadPool pool(1);
        return pool.submit([] { return 31; });
    }();
    check(result.is_ready(), "Pool destruction returned before its job finished");
    check(31 == result.get(), "Job result was lost after pool destruction");
}

void test_scheduler_waits_for_pool_result() {
    std::promise<void> release;
    const auto gate = release.get_future().share();
    ThreadPool pool(1);
    Scheduler scheduler;
    scheduler.start();

    const auto result = pool.submit([gate] {
        check(nullptr == Scheduler::current(), "Pool task ran on a scheduler worker");
        await(gate, "Caller did not release the pool task");
        return 42;
    });
    const auto wait_result = [result] {
        check(42 == result.get(), "Scheduler waiter received an incorrect result");
    };
    const auto first = scheduler.post(wait_result);
    const auto second = scheduler.post(wait_result);
    await_suspended(first);
    await_suspended(second);

    // 两个协程都在等同一个结果时，唯一的 scheduler worker 仍可执行其他任务。
    scheduler.post([] {}).wait();
    release.set_value();
    first.wait();
    second.wait();
    check(42 == result.get(), "Pool result could not be read from the caller thread");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    testing::Report report(__FILE__);
    // 给潜在的死锁设置总超时，直接运行测试时也能报告失败。
    std::promise<void> finished;
    auto finished_future = finished.get_future();
    std::thread watchdog([&] {
        constexpr std::chrono::seconds suite_timeout{45};
        if (std::future_status::timeout == finished_future.wait_for(suite_timeout)) {
            std::cerr << "[FAIL] test_thread_pool timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
    });

    const TestCase tests[] = {
        {"submit_results", test_submit_results},
        {"move_only_callable_and_result", test_move_only_callable_and_result},
        {"task_exception_and_invalid_calls", test_task_exception_and_invalid_calls},
        {"parallel_workers", test_parallel_workers},
        {"concurrent_submissions_respect_worker_limit", test_concurrent_submissions_respect_worker_limit},
        {"shutdown_before_first_submission", test_shutdown_before_first_submission},
        {"first_submission_races_shutdown", test_first_submission_races_shutdown},
        {"shutdown_drains_ready_queue", test_shutdown_drains_ready_queue},
        {"destructor_drains_queue", test_destructor_drains_queue},
        {"scheduler_waits_for_pool_result", test_scheduler_waits_for_pool_result},
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
