#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace ioman {

/**
 * @brief 保存执行现场，并在同一线程内切换执行权。
 * @details create() 创建的上下文拥有独立的栈。current() 按线程创建的主上下文
 *          使用该线程原有的栈。两者都通过平台后端保存和恢复执行现场。
 *          调度和任务状态管理由上层负责。当前平台后端使用 ucontext。
 * @note 同一个上下文不能同时在多个线程执行。Context 对象禁止复制和移动，
 *       持有它的 unique_ptr 可以转移所有权。
 */
class Context {
public:
    /// 首次切入新上下文时执行的无参入口。异常需在入口内部处理。
    using Entry = std::function<void()>;

    /// create() 分配的默认栈大小，单位为字节，共 64 KiB。
    static constexpr std::size_t DEFAULT_STACK_SIZE = 64 * 1024;

    /**
     * @brief 分配独立的执行栈，并为首次切入配置入口。
     * @param entry 首次切入时执行的非空函数。调用方应确保异常不会逃出入口。
     * @param stack_size 执行栈大小，单位为字节，必须大于零。
     * @return 拥有新上下文的 unique_ptr。返回时入口尚未执行。
     * @details 入口正常返回时，把控制权交回最近一次切入该上下文的调用者。
     * @throws std::invalid_argument 入口为空或栈大小为零。
     * @throws std::bad_alloc 分配上下文或栈失败。
     * @throws std::system_error 平台后端获取初始执行现场失败。
     */
    static std::unique_ptr<Context> create(
        Entry entry,
        std::size_t stack_size = DEFAULT_STACK_SIZE
    );

    /**
     * @brief 保存 from 的执行现场，并在当前线程上切入 to。
     * @param from 当前线程正在执行的上下文，必须与 current() 一致。
     * @param to 本线程可以切入的目标上下文，必须有效、尚未结束且与 from 不同。
     * @details 新上下文首次切入时从入口开始，已保存的上下文从上次切出的位置继续。
     *          from 再次获得执行权时，这次 swap() 调用才会返回。
     *          每次切入都会更新 to 的调用者，供入口正常返回时恢复。
     * @throws std::invalid_argument 任一指针为空，或两个指针指向同一上下文。
     * @throws std::logic_error from 不是当前上下文，或 to 已经结束。
     * @throws std::system_error 平台切换失败，此时恢复原有的上下文记录。
     */
    static void swap(Context* from, Context* to);

    /**
     * @brief 获取当前线程正在执行的上下文。
     * @return 非拥有指针。从线程原有的栈上调用时返回主上下文，
     *         从协程栈上调用时返回正在执行的协程上下文。
     * @details 主上下文在该线程首次查询时创建，由线程局部存储管理，
     *          并在线程退出时销毁。调用方不得通过返回指针释放上下文。
     * @throws std::bad_alloc 首次创建主上下文时分配失败。
     */
    static Context* current();

    /**
     * @brief 查询入口函数是否已经正常返回。
     * @return 入口返回后为 true。尚未进入、正在执行、已挂起或为主上下文时为 false。
     * @note 若需跨线程读取状态，由调用方提供同步。
     */
    bool finished() const noexcept;

    /**
     * @brief 释放后端资源及 create() 分配的独立栈。
     * @note 调用方必须确保上下文已不再执行，也不会被恢复。
     *       析构只释放持有的资源，挂起现场中的函数不会因此继续执行或展开栈。
     */
    ~Context();

    /// 禁止复制上下文的执行现场。
    Context(const Context&) = delete;
    /// 禁止通过复制赋值覆盖执行现场。
    Context& operator=(const Context&) = delete;

    /// 禁止移动对象，保证入口和调用者记录引用的地址稳定。
    Context(Context&&) = delete;
    /// 禁止通过移动赋值替换上下文。
    Context& operator=(Context&&) = delete;

private:
    /**
     * @brief 封装执行栈、平台后端、入口函数、调用者和结束状态。
     */
    struct Impl;

    /**
     * @brief 接管内部状态，供 create() 和主上下文的按需创建使用。
     * @param impl 要接管的非空内部状态。
     */
    explicit Context(std::unique_ptr<Impl> impl);

    /**
     * @brief 执行入口函数，完成后恢复最近一次切入此上下文的调用者。
     * @param argument 由后端传回的当前 Context 指针。
     * @details 入口返回后标记完成，更新线程当前上下文并恢复调用者的现场。
     *          入口抛出异常、调用者缺失或恢复失败时，终止进程。
     */
    [[noreturn]] static void _M_entry_trampoline(void* argument) noexcept;

private:
    /// 独占内部状态，平台执行现场及栈资源随它释放。
    std::unique_ptr<Impl> _M_impl;
};

} // namespace ioman
