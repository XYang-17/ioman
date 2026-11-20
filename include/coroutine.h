#pragma once

#include "context/context.h"

#include <exception>
#include <functional>
#include <memory>

namespace ioman {

namespace detail {
class CancellationScope;
class CancellationState;
}

/**
 * @brief Coroutine 对象自身的执行状态。
 * @details 调度器另外记录任务的就绪或等待状态，用来决定协程让出后是否入队。
 */
enum class CoroutineState {
    READY,      ///< 已构造，入口尚未执行。
    RUNNING,    ///< 已开始执行，也可能正在等待嵌套 resume() 返回。
    SUSPENDED,  ///< 已主动让出，等待下一次 resume()。
    DEAD        ///< 入口已返回或抛出异常，无法再次恢复。
};

/**
 * @brief 具有独立执行栈的协程执行载体。
 * @details 直接创建时，由调用方在同一执行线程调用 resume() 并管理生命周期。
 *          交给 Scheduler 后由所属 worker 恢复。需要调度、取结果或取消时，
 *          通常通过 ioman::submit() 或 ioman::submit_task() 使用协程。
 * @note 只有当前协程能调用自己的 yield()，同一个对象只能由一个线程执行。
 *       以下示例包含 <ioman/coroutine.h>。
 */
class Coroutine {
public:
    /// 用户入口的函数签名。入口返回 void，异常由 resume() 交付给调用者。
    using Entry = std::function<void()>;

    /**
     * @brief 创建协程，初始状态为 READY。
     * @param entry 非空的用户入口，首次 resume() 时调用。
     * @param stack_size 独立栈大小，单位为字节，默认 64 KiB。
     * @throws std::invalid_argument 入口为空或栈大小为零。
     * @throws std::bad_alloc 分配上下文或栈失败。
     * @throws std::system_error 底层上下文创建失败。
     * @code
     * ioman::Coroutine coroutine([] {}, 128 * 1024);
     * coroutine.resume();
     * @endcode
     */
    explicit Coroutine(
        Entry entry,
        std::size_t stack_size = Context::DEFAULT_STACK_SIZE
    );

    /**
     * @brief 销毁协程并释放执行上下文和独立栈。
     * @note 销毁前须确保协程不再运行，也不会恢复。若已挂起，应先让入口退出，
     *       使其栈上的局部对象能够析构。
     * @code
     * {
     *     ioman::Coroutine coroutine([] {});
     *     coroutine.resume();
     * } // 入口已结束，此处释放上下文。
     * @endcode
     */
    ~Coroutine();

    /// 禁止复制协程及其执行现场。
    Coroutine(const Coroutine&) = delete;
    /// 禁止通过复制赋值覆盖现有协程。
    Coroutine& operator=(const Coroutine&) = delete;

    /// 禁止移动，确保入口捕获的对象地址稳定。
    Coroutine(Coroutine&&) = delete;
    /// 禁止通过移动赋值覆盖协程及其执行现场。
    Coroutine& operator=(Coroutine&&) = delete;

    /**
     * @brief 恢复协程执行。
     * @details 首次调用进入入口，后续调用从上次 yield() 处继续。
     *          协程再次让出或结束后，本次调用才返回。入口异常在协程结束后原样抛出。
     * @note 只可恢复 READY 或 SUSPENDED 的对象。直接管理时，在同一执行线程上恢复。
     * @throws std::logic_error 协程已经运行或已经结束。
     * @throws std::system_error 底层上下文切换失败。
     * @code
     * ioman::Coroutine coroutine([] { ioman::Coroutine::current()->yield(); });
     * coroutine.resume(); // 运行到 yield()。
     * coroutine.resume(); // 从 yield() 继续，直到入口结束。
     * @endcode
     */
    void resume();

    /**
     * @brief 挂起当前协程，切回最近一次恢复它的调用方。
     * @details 让出时状态为 SUSPENDED。再次 resume() 后从本次调用继续执行。
     *          由 Scheduler 管理时，直接 yield() 会让任务重新排队。
     * @throws std::logic_error 当前对象不是正在执行的协程，或缺少可返回的调用者上下文。
     * @throws std::system_error 底层上下文切换失败。
     * @code
     * int phase = 0;
     * ioman::Coroutine coroutine([&phase] {
     *     phase = 1;
     *     ioman::Coroutine::current()->yield();
     *     phase = 2;
     * });
     * coroutine.resume(); // phase == 1
     * coroutine.resume(); // phase == 2
     * @endcode
     */
    void yield();

    /**
     * @brief 获取当前 OS 线程正在执行的协程。
     * @return 非拥有指针。当前线程在主上下文或调度循环中执行时返回 nullptr。
     * @details 嵌套 resume() 会返回当前正在执行的内层协程，切回后恢复外层指针。
     * @code
     * ioman::Coroutine coroutine([] {
     *     const auto* self = ioman::Coroutine::current(); // 指向当前协程。
     * });
     * coroutine.resume();
     * @endcode
     */
    static Coroutine* current() noexcept {
        return _M_tls_current_coroutine;
    }

    /**
     * @brief 获取协程对象的执行状态。
     * @return 当前 CoroutineState。调度器的排队状态可通过对应的 TaskHandle 查询。
     * @note 跨线程读取时，调用方须确保与状态更新同步。
     * @code
     * ioman::Coroutine coroutine([] {});
     * const auto before = coroutine.state(); // READY
     * coroutine.resume();
     * const auto after = coroutine.state(); // DEAD
     * @endcode
     */
    CoroutineState state() const noexcept {
        return _M_state;
    }

    /**
     * @brief 判断协程入口是否已结束。
     * @return 入口已返回或以异常退出时为 true，挂起期间为 false。
     * @note 跨线程读取时，调用方须确保与状态更新同步。
     * @code
     * ioman::Coroutine coroutine([] {});
     * coroutine.resume();
     * const bool done = coroutine.finished(); // true
     * @endcode
     */
    bool finished() const noexcept {
        return CoroutineState::DEAD == _M_state;
    }

private:
    friend class detail::CancellationScope;
    /**
     * @brief 协程入口包装函数。
     *
     * 捕获用户入口函数抛出的异常，避免异常穿过
     * 底层 Context 的入口边界。
     */
    void run();

private:
    /// 用户提供的协程入口函数。
    Entry _M_entry;

    /// 协程执行上下文。
    std::unique_ptr<Context> _M_context;

    /// 协程当前状态。
    CoroutineState _M_state = CoroutineState::READY;

    /// 恢复方的非拥有上下文指针，yield() 用它切回调用者。
    /// 通常指向 worker 的主上下文。嵌套 resume() 时也可指向调用方协程的上下文。
    Context* _M_resumer_context = nullptr;

    /// 协程执行过程中产生的异常。
    std::exception_ptr _M_exception;

    /// 顶层任务绑定的取消状态，随协程跨挂起点保留。
    detail::CancellationState* _M_cancellation = nullptr;

    /// 当前线程正在执行的协程。
    static thread_local Coroutine* _M_tls_current_coroutine;
};

} // namespace ioman
