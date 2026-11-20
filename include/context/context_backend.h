#pragma once

#include <cstddef>
#include <memory>

namespace ioman::detail {

/**
 * @brief 为 Context 封装平台执行现场和切换操作。
 * @details 当前实现使用 ucontext。Context 管理栈的生命周期、当前上下文记录
 *          和调用者关系。后端持有原生现场，并按调用方指定的目标进行切换。
 */
class ContextBackend {
public:
    /// 首次切入时执行的无异常入口。入口结束时应切回调用者，直接返回会终止进程。
    using Entry = void (*)(void*) noexcept;

    /**
     * @brief 分配原生状态所需的存储。
     * @details 新上下文通过 initialize() 配置。线程主上下文可以直接作为
     *          swap_to() 的源，在首次切出时保存现场。
     * @throws std::bad_alloc 状态存储分配失败。
     */
    ContextBackend();

    /** @brief 释放后端状态。调用方须保证该现场已停止执行且不会再被恢复。 */
    ~ContextBackend();

    /// 禁止复制原生执行现场。
    ContextBackend(const ContextBackend&) = delete;

    /// 禁止通过复制赋值覆盖原生执行现场。
    ContextBackend& operator=(const ContextBackend&) = delete;

    /**
     * @brief 在调用方提供的栈上配置新上下文的入口。
     * @param stack 栈缓冲区起始地址。缓冲区由调用方持有，须在后端使用期间保持有效。
     * @param stack_size 缓冲区容量，单位为字节，必须大于零。
     * @param entry 非空的入口函数，在目标上下文首次获得执行权时调用。
     * @param argument 原样传给入口的参数。若指向对象，调用方须保证该对象
     *                 在入口使用它期间有效。
     * @details 此方法只配置初始现场，实际执行从首次切入开始。
     * @throws std::system_error 平台调用获取初始执行现场失败。
     */
    void initialize(std::byte* stack, std::size_t stack_size, Entry entry, void* argument);

    /**
     * @brief 将当前执行现场保存到此后端，并切入目标现场。
     * @param target 同一线程中可切入的目标。其现场须已初始化或由先前的切换保存。
     * @details 当前现场被其他上下文恢复后，本次调用才会返回。
     * @throws std::system_error 平台切换失败。
     */
    void swap_to(const ContextBackend& target);

    /**
     * @brief 直接恢复此后端所持有的有效现场。
     * @details 当前执行现场不会被保存。Context 用此方法在入口结束时恢复调用者。
     *          成功时在目标现场继续执行。平台调用若返回，则终止进程。
     */
    [[noreturn]] void resume() const noexcept;

private:
    /// 封装原生上下文、首次执行的入口及入口参数。
    struct State;

    /// 独占后端状态，保证传给平台入口的状态地址在使用期间稳定。
    std::unique_ptr<State> _M_state;
};

} // namespace ioman::detail
