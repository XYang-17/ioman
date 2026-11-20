#include <test_support.h>
#include <ioman/coroutine.h>
#include <ioman/context/context.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * @brief 测试检查辅助函数。
 *
 * @param condition 测试条件。
 * @param message 条件不满足时的错误信息。
 *
 * @throws std::runtime_error 测试条件不满足时抛出。
 *
 * 不使用 assert，保证 Release 模式下测试仍然有效。
 */
void check(
    const bool condition,
    const std::string& message
) {
    if (false == condition) {
        throw std::runtime_error(message);
    }
}


/**
 * @brief 检查指定操作是否抛出预期异常。
 *
 * @tparam Exception 预期异常类型。
 * @param operation 待执行的操作。
 * @param test_name 测试名称。
 *
 * @throws std::runtime_error 未抛出预期异常时抛出。
 */
template <typename Exception>
void expect_exception(
    const std::function<void()>& operation,
    const std::string& test_name
) {
    bool exception_caught = false;

    try {
        operation();
    } catch (const Exception&) {
        exception_caught = true;
    }

    check(
        exception_caught,
        test_name + ": expected exception was not thrown"
    );
}


/**
 * @brief 测试协程创建及初始状态。
 *
 * 验证：
 * 1. 协程创建成功。
 * 2. 初始状态为 READY。
 * 3. 协程尚未结束。
 * 4. 普通线程中 current() 返回 nullptr。
 */
void test_coroutine_initial_state() {
    ioman::Coroutine coroutine([]() {});

    check(
        ioman::CoroutineState::READY == coroutine.state(),
        "Initial state should be READY"
    );

    check(
        false == coroutine.finished(),
        "New coroutine should not be finished"
    );

    check(
        nullptr == ioman::Coroutine::current(),
        "Current coroutine should be nullptr outside coroutine"
    );

    std::cout << "[PASS] test_coroutine_initial_state\n";
}


/**
 * @brief 测试协程正常执行和结束。
 *
 * 验证入口函数只执行一次，执行完成后状态变为 DEAD。
 */
void test_coroutine_normal_completion() {
    int execution_count = 0;

    ioman::Coroutine coroutine([&execution_count]() {
        ++execution_count;
    });

    coroutine.resume();

    check(
        1 == execution_count,
        "Coroutine entry should execute exactly once"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should enter DEAD state"
    );

    check(
        true == coroutine.finished(),
        "Coroutine should be finished"
    );

    std::cout << "[PASS] test_coroutine_normal_completion\n";
}


/**
 * @brief 测试协程单次挂起和恢复。
 *
 * 验证：
 * 1. 协程执行到 yield() 后返回调用者。
 * 2. 协程状态正确变为 SUSPENDED。
 * 3. 再次 resume() 后从挂起位置继续执行。
 */
void test_coroutine_single_yield() {
    int execution_step = 0;
    ioman::Coroutine* coroutine_ptr = nullptr;

    ioman::Coroutine coroutine([&]() {
        ++execution_step;

        coroutine_ptr->yield();

        ++execution_step;
    });

    coroutine_ptr = &coroutine;

    coroutine.resume();

    check(
        1 == execution_step,
        "Coroutine should stop at first yield"
    );

    check(
        ioman::CoroutineState::SUSPENDED == coroutine.state(),
        "Coroutine should enter SUSPENDED state"
    );

    check(
        false == coroutine.finished(),
        "Suspended coroutine should not be finished"
    );

    coroutine.resume();

    check(
        2 == execution_step,
        "Coroutine should continue after yield"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should enter DEAD state after completion"
    );

    std::cout << "[PASS] test_coroutine_single_yield\n";
}


/**
 * @brief 测试协程多次挂起和恢复。
 *
 * 验证协程能够保存执行现场，并在多次恢复后
 * 从各自的挂起位置继续执行。
 */
void test_coroutine_multiple_yields() {
    std::vector<int> execution_order;
    ioman::Coroutine* coroutine_ptr = nullptr;

    ioman::Coroutine coroutine([&]() {
        execution_order.push_back(1);
        coroutine_ptr->yield();

        execution_order.push_back(2);
        coroutine_ptr->yield();

        execution_order.push_back(3);
        coroutine_ptr->yield();

        execution_order.push_back(4);
    });

    coroutine_ptr = &coroutine;

    const std::array<std::size_t, 3> expected_sizes = {
        1, 2, 3
    };

    const std::array<int, 4> expected_order = {
        1, 2, 3, 4
    };

    for (std::size_t index = 0;
         index < expected_sizes.size();
         ++index) {
        coroutine.resume();

        check(
            ioman::CoroutineState::SUSPENDED == coroutine.state(),
            "Coroutine should be suspended after yield"
        );

        check(
            expected_sizes[index] == execution_order.size(),
            "Unexpected execution count after yield"
        );

        check(
            true == coroutine.finished() ? false : true,
            "Coroutine should not be finished before final resume"
        );
    }

    coroutine.resume();

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should finish after final resume"
    );

    check(
        expected_order.size() == execution_order.size(),
        "Unexpected final execution count"
    );

    for (std::size_t index = 0;
         index < expected_order.size();
         ++index) {
        check(
            expected_order[index] == execution_order[index],
            "Coroutine execution order is incorrect"
        );
    }

    std::cout << "[PASS] test_coroutine_multiple_yields\n";
}


/**
 * @brief 测试 current() 的正确性。
 *
 * 验证：
 * 1. 普通线程中 current() 返回 nullptr。
 * 2. 协程运行期间 current() 返回当前协程。
 * 3. 协程挂起返回后 current() 恢复为 nullptr。
 * 4. 协程再次恢复后 current() 再次指向当前协程。
 * 5. 协程结束后 current() 恢复为 nullptr。
 */
void test_coroutine_current() {
    ioman::Coroutine* coroutine_ptr = nullptr;

    ioman::Coroutine coroutine([&]() {
        check(
            ioman::Coroutine::current() == coroutine_ptr,
            "Current coroutine pointer is incorrect"
        );

        coroutine_ptr->yield();

        check(
            ioman::Coroutine::current() == coroutine_ptr,
            "Current coroutine pointer is incorrect after resume"
        );
    });

    coroutine_ptr = &coroutine;

    check(
        nullptr == ioman::Coroutine::current(),
        "Current coroutine should be nullptr before resume"
    );

    coroutine.resume();

    check(
        nullptr == ioman::Coroutine::current(),
        "Current coroutine should be nullptr after yield"
    );

    coroutine.resume();

    check(
        nullptr == ioman::Coroutine::current(),
        "Current coroutine should be nullptr after completion"
    );

    std::cout << "[PASS] test_coroutine_current\n";
}


/**
 * @brief 测试运行中的协程状态。
 *
 * 验证协程执行入口函数时状态为 RUNNING，
 * 挂起后状态为 SUSPENDED，再次恢复后状态为 RUNNING。
 */
void test_coroutine_running_state() {
    ioman::Coroutine* coroutine_ptr = nullptr;
    bool first_running_check = false;
    bool second_running_check = false;

    ioman::Coroutine coroutine([&]() {
        first_running_check =
            ioman::CoroutineState::RUNNING == coroutine_ptr->state();

        coroutine_ptr->yield();

        second_running_check =
            ioman::CoroutineState::RUNNING == coroutine_ptr->state();
    });

    coroutine_ptr = &coroutine;

    coroutine.resume();

    check(
        true == first_running_check,
        "Coroutine should be RUNNING during first execution"
    );

    check(
        ioman::CoroutineState::SUSPENDED == coroutine.state(),
        "Coroutine should be SUSPENDED after yield"
    );

    coroutine.resume();

    check(
        true == second_running_check,
        "Coroutine should be RUNNING after resume"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should be DEAD after completion"
    );

    std::cout << "[PASS] test_coroutine_running_state\n";
}


/**
 * @brief 测试协程嵌套执行。
 *
 * 外层协程 A 恢复内层协程 B。
 * B 执行后主动挂起，返回 A。
 * A 继续执行并挂起，返回主上下文。
 * 随后分别恢复 A 和 B，验证上下文切换关系。
 *
 * 注意：这里测试的是嵌套 resume/yield，
 * 不涉及 Scheduler 的任务调度。
 */
void test_nested_coroutines() {
    std::vector<int> execution_order;

    ioman::Coroutine* outer_ptr = nullptr;
    ioman::Coroutine* inner_ptr = nullptr;

    ioman::Coroutine inner([&]() {
        check(
            ioman::Coroutine::current() == inner_ptr,
            "Inner coroutine current pointer is incorrect"
        );

        execution_order.push_back(1);

        inner_ptr->yield();

        execution_order.push_back(4);
    });

    inner_ptr = &inner;

    ioman::Coroutine outer([&]() {
        check(
            ioman::Coroutine::current() == outer_ptr,
            "Outer coroutine current pointer is incorrect"
        );

        execution_order.push_back(0);

        inner_ptr->resume();

        check(
            ioman::Coroutine::current() == outer_ptr,
            "Outer coroutine should become current after inner yield"
        );

        execution_order.push_back(2);

        outer_ptr->yield();

        execution_order.push_back(5);

        inner_ptr->resume();

        execution_order.push_back(6);
    });

    outer_ptr = &outer;

    // 第一次恢复外层协程。
    outer.resume();

    check(
        ioman::CoroutineState::SUSPENDED == outer.state(),
        "Outer coroutine should be suspended"
    );

    check(
        ioman::CoroutineState::SUSPENDED == inner.state(),
        "Inner coroutine should be suspended"
    );

    // 恢复外层协程，继续执行并完成内层协程。
    outer.resume();

    check(
        ioman::CoroutineState::DEAD == outer.state(),
        "Outer coroutine should be DEAD"
    );

    check(
        ioman::CoroutineState::DEAD == inner.state(),
        "Inner coroutine should be DEAD"
    );

    const std::vector<int> expected_order = {
        0, 1, 2, 5, 4, 6
    };

    check(
        expected_order == execution_order,
        "Nested coroutine execution order is incorrect"
    );

    std::cout << "[PASS] test_nested_coroutines\n";
}


/**
 * @brief 测试空入口函数。
 *
 * 创建协程时传入空入口函数，应抛出 std::invalid_argument。
 */
void test_empty_entry() {
    expect_exception<std::invalid_argument>(
        []() {
            ioman::Coroutine coroutine(
                ioman::Coroutine::Entry{}
            );
        },
        "test_empty_entry"
    );

    std::cout << "[PASS] test_empty_entry\n";
}


/**
 * @brief 测试非法栈大小。
 *
 * 当前 Context 不允许创建栈大小为 0 的协程。
 */
void test_invalid_stack_size() {
    expect_exception<std::invalid_argument>(
        []() {
            ioman::Coroutine coroutine(
                []() {},
                0
            );
        },
        "test_invalid_stack_size"
    );

    std::cout << "[PASS] test_invalid_stack_size\n";
}


/**
 * @brief 测试恢复已经结束的协程。
 *
 * DEAD 状态的协程不能再次恢复。
 */
void test_resume_dead_coroutine() {
    ioman::Coroutine coroutine([]() {});

    coroutine.resume();

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should be DEAD before second resume"
    );

    expect_exception<std::logic_error>(
        [&coroutine]() {
            coroutine.resume();
        },
        "test_resume_dead_coroutine"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine state should remain DEAD"
    );

    std::cout << "[PASS] test_resume_dead_coroutine\n";
}


/**
 * @brief 测试运行中的协程不能重入恢复。
 *
 * 在协程入口函数内部再次调用自己的 resume()，
 * 应当抛出 std::logic_error。
 *
 * 验证失败的重入操作不会破坏当前协程的执行状态。
 */
void test_resume_running_coroutine() {
    ioman::Coroutine* coroutine_ptr = nullptr;
    bool exception_caught = false;

    ioman::Coroutine coroutine([&]() {
        expect_exception<std::logic_error>(
            [coroutine_ptr]() {
                coroutine_ptr->resume();
            },
            "test_resume_running_coroutine"
        );

        exception_caught = true;
    });

    coroutine_ptr = &coroutine;

    coroutine.resume();

    check(
        true == exception_caught,
        "Reentrant resume should throw logic_error"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should finish normally after rejected reentrant resume"
    );

    std::cout << "[PASS] test_resume_running_coroutine\n";
}


/**
 * @brief 测试在 READY 状态下非法调用 yield()。
 *
 * 尚未执行的协程不能主动挂起。
 */
void test_yield_ready_coroutine() {
    ioman::Coroutine coroutine([]() {});

    expect_exception<std::logic_error>(
        [&coroutine]() {
            coroutine.yield();
        },
        "test_yield_ready_coroutine"
    );

    check(
        ioman::CoroutineState::READY == coroutine.state(),
        "Coroutine state should remain READY"
    );

    std::cout << "[PASS] test_yield_ready_coroutine\n";
}


/**
 * @brief 测试在 SUSPENDED 状态下非法调用 yield()。
 *
 * 挂起的协程不能由外部直接调用 yield()。
 */
void test_yield_suspended_coroutine() {
    ioman::Coroutine* coroutine_ptr = nullptr;

    ioman::Coroutine coroutine([&]() {
        coroutine_ptr->yield();
    });

    coroutine_ptr = &coroutine;

    coroutine.resume();

    check(
        ioman::CoroutineState::SUSPENDED == coroutine.state(),
        "Coroutine should be SUSPENDED"
    );

    expect_exception<std::logic_error>(
        [&coroutine]() {
            coroutine.yield();
        },
        "test_yield_suspended_coroutine"
    );

    check(
        ioman::CoroutineState::SUSPENDED == coroutine.state(),
        "Coroutine state should remain SUSPENDED"
    );

    std::cout << "[PASS] test_yield_suspended_coroutine\n";
}


/**
 * @brief 测试在 DEAD 状态下非法调用 yield()。
 *
 * 已经结束的协程不能再次主动挂起。
 */
void test_yield_dead_coroutine() {
    ioman::Coroutine coroutine([]() {});

    coroutine.resume();

    expect_exception<std::logic_error>(
        [&coroutine]() {
            coroutine.yield();
        },
        "test_yield_dead_coroutine"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine state should remain DEAD"
    );

    std::cout << "[PASS] test_yield_dead_coroutine\n";
}


/**
 * @brief 测试协程入口函数中的异常传播。
 *
 * 验证：
 * 1. 入口函数抛出的异常不会穿过 Context 入口边界。
 * 2. 异常能够保存并在 resume() 返回后重新抛出。
 * 3. 异常发生后协程状态为 DEAD。
 */
void test_coroutine_exception() {
    ioman::Coroutine coroutine([]() {
        throw std::runtime_error("coroutine runtime error");
    });

    bool exception_caught = false;

    try {
        coroutine.resume();
    } catch (const std::runtime_error& exception) {
        exception_caught = true;

        check(
            std::string("coroutine runtime error") == exception.what(),
            "Exception message is incorrect"
        );
    }

    check(
        true == exception_caught,
        "Coroutine exception should propagate to caller"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should be DEAD after exception"
    );

    check(
        true == coroutine.finished(),
        "Coroutine should be finished after exception"
    );

    check(
        nullptr == ioman::Coroutine::current(),
        "Current coroutine should be nullptr after exception"
    );

    std::cout << "[PASS] test_coroutine_exception\n";
}


/**
 * @brief 测试协程挂起后再发生异常。
 *
 * 验证异常不会在第一次 resume() 时提前传播，
 * 而是在协程恢复并真正结束后传播。
 */
void test_coroutine_exception_after_yield() {
    ioman::Coroutine* coroutine_ptr = nullptr;
    bool first_stage_executed = false;

    ioman::Coroutine coroutine([&]() {
        first_stage_executed = true;

        coroutine_ptr->yield();

        throw std::runtime_error("exception after yield");
    });

    coroutine_ptr = &coroutine;

    // 第一次执行只会挂起，不应产生异常。
    coroutine.resume();

    check(
        true == first_stage_executed,
        "Coroutine first stage should execute"
    );

    check(
        ioman::CoroutineState::SUSPENDED == coroutine.state(),
        "Coroutine should be SUSPENDED before exception"
    );

    // 第二次恢复后才会发生异常。
    expect_exception<std::runtime_error>(
        [&coroutine]() {
            coroutine.resume();
        },
        "test_coroutine_exception_after_yield"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should be DEAD after exception"
    );

    std::cout << "[PASS] test_coroutine_exception_after_yield\n";
}


/**
 * @brief 测试协程异常后不能再次恢复。
 *
 * 验证异常传播后，协程已经结束，不能重复执行。
 */
void test_resume_exception_coroutine() {
    ioman::Coroutine coroutine([]() {
        throw std::runtime_error("test exception");
    });

    expect_exception<std::runtime_error>(
        [&coroutine]() {
            coroutine.resume();
        },
        "test_resume_exception_coroutine"
    );

    check(
        ioman::CoroutineState::DEAD == coroutine.state(),
        "Coroutine should be DEAD after exception"
    );

    expect_exception<std::logic_error>(
        [&coroutine]() {
            coroutine.resume();
        },
        "test_resume_exception_coroutine_second_resume"
    );

    std::cout << "[PASS] test_resume_exception_coroutine\n";
}


/**
 * @brief 测试不同线程的主上下文隔离。
 *
 * 验证：
 * 1. 不同线程拥有独立的 Context 主上下文。
 * 2. 不同线程的 Coroutine::current() 相互独立。
 * 3. 工作线程中的协程执行不会改变主线程的当前协程。
 */
void test_thread_context_isolation() {
    ioman::Context* main_context = ioman::Context::current();

    check(
        nullptr == ioman::Coroutine::current(),
        "Main thread should not have a current coroutine"
    );

    std::exception_ptr worker_exception;
    std::atomic<bool> worker_passed = false;

    std::thread worker([&]() {
        try {
            ioman::Context* worker_context =
                ioman::Context::current();

            check(
                worker_context != main_context,
                "Different threads should have different main contexts"
            );

            check(
                ioman::Context::current() == worker_context,
                "Worker main context should remain unchanged"
            );

            ioman::Coroutine* coroutine_ptr = nullptr;

            ioman::Coroutine coroutine([&]() {
                check(
                    ioman::Coroutine::current() == coroutine_ptr,
                    "Worker current coroutine is incorrect"
                );

                coroutine_ptr->yield();

                check(
                    ioman::Coroutine::current() == coroutine_ptr,
                    "Worker current coroutine is incorrect after resume"
                );
            });

            coroutine_ptr = &coroutine;

            coroutine.resume();

            check(
                nullptr == ioman::Coroutine::current(),
                "Worker current coroutine should be nullptr after yield"
            );

            coroutine.resume();

            check(
                nullptr == ioman::Coroutine::current(),
                "Worker current coroutine should be nullptr after completion"
            );

            check(
                ioman::Context::current() == worker_context,
                "Worker context should be restored after coroutine completion"
            );

            worker_passed.store(true);
        } catch (...) {
            worker_exception = std::current_exception();
        }
    });

    worker.join();

    if (worker_exception != nullptr) {
        std::rethrow_exception(worker_exception);
    }

    check(
        true == worker_passed.load(),
        "Worker thread test failed"
    );

    check(
        ioman::Context::current() == main_context,
        "Main thread context should remain unchanged"
    );

    check(
        nullptr == ioman::Coroutine::current(),
        "Main thread current coroutine should remain nullptr"
    );

    std::cout << "[PASS] test_thread_context_isolation\n";
}


/**
 * @brief 测试多个独立协程的执行。
 *
 * 验证多个协程分别拥有独立的执行现场，
 * 其挂起与恢复互不影响。
 */
void test_multiple_coroutines() {
    std::vector<int> execution_order;

    ioman::Coroutine* first_ptr = nullptr;
    ioman::Coroutine* second_ptr = nullptr;

    ioman::Coroutine first([&]() {
        execution_order.push_back(1);

        first_ptr->yield();

        execution_order.push_back(3);
    });

    first_ptr = &first;

    ioman::Coroutine second([&]() {
        execution_order.push_back(2);

        second_ptr->yield();

        execution_order.push_back(4);
    });

    second_ptr = &second;

    first.resume();
    second.resume();

    check(
        ioman::CoroutineState::SUSPENDED == first.state(),
        "First coroutine should be suspended"
    );

    check(
        ioman::CoroutineState::SUSPENDED == second.state(),
        "Second coroutine should be suspended"
    );

    first.resume();
    second.resume();

    const std::vector<int> expected_order = {
        1, 2, 3, 4
    };

    check(
        expected_order == execution_order,
        "Multiple coroutine execution order is incorrect"
    );

    check(
        true == first.finished(),
        "First coroutine should be finished"
    );

    check(
        true == second.finished(),
        "Second coroutine should be finished"
    );

    std::cout << "[PASS] test_multiple_coroutines\n";
}


/**
 * @brief 单项测试信息。
 */
struct TestCase {
    const char* name;
    void (*function)();
};


/**
 * @brief 执行单项测试。
 *
 * @param test 测试项。
 * @return true 表示测试通过，false 表示测试失败。
 */
bool run_test(const TestCase& test) {
    try {
        test.function();
        return true;
    } catch (const std::exception& exception) {
        std::cerr
            << "[FAIL] "
            << test.name
            << ": "
            << exception.what()
            << '\n';
    } catch (...) {
        std::cerr
            << "[FAIL] "
            << test.name
            << ": unknown exception\n";
    }

    return false;
}

} // namespace


int main() {
    testing::Report report(__FILE__);
    const std::array<TestCase, 19> tests = {{
        {
            "test_coroutine_initial_state",
            test_coroutine_initial_state
        },
        {
            "test_coroutine_normal_completion",
            test_coroutine_normal_completion
        },
        {
            "test_coroutine_single_yield",
            test_coroutine_single_yield
        },
        {
            "test_coroutine_multiple_yields",
            test_coroutine_multiple_yields
        },
        {
            "test_coroutine_current",
            test_coroutine_current
        },
        {
            "test_coroutine_running_state",
            test_coroutine_running_state
        },
        {
            "test_nested_coroutines",
            test_nested_coroutines
        },
        {
            "test_empty_entry",
            test_empty_entry
        },
        {
            "test_invalid_stack_size",
            test_invalid_stack_size
        },
        {
            "test_resume_dead_coroutine",
            test_resume_dead_coroutine
        },
        {
            "test_resume_running_coroutine",
            test_resume_running_coroutine
        },
        {
            "test_yield_ready_coroutine",
            test_yield_ready_coroutine
        },
        {
            "test_yield_suspended_coroutine",
            test_yield_suspended_coroutine
        },
        {
            "test_yield_dead_coroutine",
            test_yield_dead_coroutine
        },
        {
            "test_coroutine_exception",
            test_coroutine_exception
        },
        {
            "test_coroutine_exception_after_yield",
            test_coroutine_exception_after_yield
        },
        {
            "test_resume_exception_coroutine",
            test_resume_exception_coroutine
        },
        {
            "test_thread_context_isolation",
            test_thread_context_isolation
        },
        {
            "test_multiple_coroutines",
            test_multiple_coroutines
        }
    }};

    std::size_t passed_count = 0;
    std::size_t failed_count = 0;

    std::cout << "========== Coroutine Tests ==========\n";

    for (const TestCase& test : tests) {
        if (true == run_test(test)) {
            ++passed_count;
        } else {
            ++failed_count;
        }
    }

    std::cout << "\n========== Test Summary ==========\n";
    std::cout << "Total : " << tests.size() << '\n';
    std::cout << "Passed: " << passed_count << '\n';
    std::cout << "Failed: " << failed_count << '\n';

    if (0 != failed_count) {
        return 1;
    }

    std::cout << "All coroutine tests passed.\n";

    return 0;
}
