#include <test_support.h>
#include <ioman/context/context.h>

#include <iostream>
#include <stdexcept>
#include <thread>

using ioman::Context;

/**
 * @brief 检查测试条件。
 *
 * 使用异常报告测试失败，避免 Release 模式下 assert 被禁用。
 */
#define CHECK(condition)                                      \
    do {                                                      \
        if (!(condition)) {                                   \
            throw std::runtime_error(                         \
                "Test failed: " #condition                    \
            );                                                \
        }                                                     \
    } while (false)


/**
 * @brief 测试主上下文的获取。
 */
void test_main_context() {
    Context* main_context = Context::current();

    CHECK(main_context != nullptr);
    CHECK(Context::current() == main_context);
    CHECK(main_context->finished() == false);

    std::cout << "[PASS] test_main_context\n";
}


/**
 * @brief 测试协程的创建、挂起和恢复。
 */
void test_context_switch() {
    Context* main_context = Context::current();

    int step = 0;

    auto coroutine = Context::create([&]() {
        // 第一次进入协程。
        ++step;

        // 主动切回主上下文，模拟协程挂起。
        Context::swap(Context::current(), main_context);

        // 再次恢复后，从挂起位置继续执行。
        ++step;
    });

    CHECK(coroutine != nullptr);
    CHECK(coroutine->finished() == false);
    CHECK(0 == step);

    // 第一次切换：协程执行到挂起位置。
    Context::swap(main_context, coroutine.get());

    CHECK(1 == step);
    CHECK(Context::current() == main_context);
    CHECK(coroutine->finished() == false);

    // 第二次切换：协程继续执行并正常结束。
    Context::swap(main_context, coroutine.get());

    CHECK(2 == step);
    CHECK(Context::current() == main_context);
    CHECK(coroutine->finished() == true);

    std::cout << "[PASS] test_context_switch\n";
}


/**
 * @brief 测试协程入口直接执行结束的情况。
 */
void test_context_finish() {
    Context* main_context = Context::current();

    bool executed = false;

    auto coroutine = Context::create([&]() {
        executed = true;
    });

    CHECK(coroutine->finished() == false);

    // 协程执行完毕后，应该自动返回主上下文。
    Context::swap(main_context, coroutine.get());

    CHECK(executed == true);
    CHECK(coroutine->finished() == true);
    CHECK(Context::current() == main_context);

    std::cout << "[PASS] test_context_finish\n";
}


/**
 * @brief 测试不同线程拥有独立的主上下文。
 */
void test_thread_local_context() {
    Context* main_context = Context::current();

    bool thread_passed = false;

    std::thread worker([&]() {
        Context* thread_main = Context::current();

        CHECK(thread_main != nullptr);
        CHECK(thread_main != main_context);
        CHECK(Context::current() == thread_main);

        // 在工作线程中创建并执行一个协程。
        bool executed = false;

        auto coroutine = Context::create([&]() {
            executed = true;
        });

        Context::swap(thread_main, coroutine.get());

        CHECK(executed == true);
        CHECK(coroutine->finished() == true);
        CHECK(Context::current() == thread_main);

        thread_passed = true;
    });

    worker.join();

    CHECK(thread_passed == true);
    CHECK(Context::current() == main_context);

    std::cout << "[PASS] test_thread_local_context\n";
}


/**
 * @brief 测试非法参数处理。
 */
void test_invalid_arguments() {
    bool empty_entry_caught = false;
    bool zero_stack_caught = false;
    bool null_context_caught = false;

    // 空入口函数。
    try {
        auto coroutine = Context::create(Context::Entry{});
    } catch (const std::invalid_argument&) {
        empty_entry_caught = true;
    }

    CHECK(empty_entry_caught == true);

    // 栈大小为 0。
    try {
        auto coroutine = Context::create([]() {}, 0);
    } catch (const std::invalid_argument&) {
        zero_stack_caught = true;
    }

    CHECK(zero_stack_caught == true);

    // 空上下文指针。
    try {
        Context::swap(Context::current(), nullptr);
    } catch (const std::invalid_argument&) {
        null_context_caught = true;
    }

    CHECK(null_context_caught == true);

    std::cout << "[PASS] test_invalid_arguments\n";
}


/**
 * @brief 测试已经结束的协程不能再次恢复。
 */
void test_resume_finished_context() {
    Context* main_context = Context::current();

    auto coroutine = Context::create([]() {});

    Context::swap(main_context, coroutine.get());

    CHECK(coroutine->finished() == true);

    bool exception_caught = false;

    try {
        Context::swap(main_context, coroutine.get());
    } catch (const std::logic_error&) {
        exception_caught = true;
    }

    CHECK(exception_caught == true);

    std::cout << "[PASS] test_resume_finished_context\n";
}


int main() {
    testing::Report report(__FILE__);
    try {
        test_main_context();
        test_context_switch();
        test_context_finish();
        test_thread_local_context();
        test_invalid_arguments();
        test_resume_finished_context();

        std::cout << "\nAll Context tests passed.\n";
    } catch (const std::exception& exception) {
        std::cerr << "\n[FAIL] " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
