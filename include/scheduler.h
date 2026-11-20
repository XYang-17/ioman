#pragma once

#include "task_handler.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ioman {

class Coroutine;
class ioman;

/** @brief 在固定数量的 worker 上协作式执行协程。 */
class Scheduler {
public:
    /** @param worker_count worker 数量，必须大于零。 */
    explicit Scheduler(std::size_t worker_count = 1);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /** @brief 启动 worker，使调度器可以接受任务。 */
    void start();

    /**
     * @brief 停止接受任务，唤醒挂起的协程，并等待 worker 退出。
     * @details 已就绪的任务继续执行。显式挂起会以异常恢复。
     *          必须从调度器外部调用，正在执行的用户代码需要主动结束。
     */
    void stop();

    /**
     * @brief 查询调度器当前是否拒绝新任务。
     * @return 尚未启动、正在停止或已经停止时返回 true。
     * @note 此查询可跨线程调用。投递时仍需在队列锁内确认状态。
     */
    bool is_closed() const noexcept;

    /**
     * @brief 投递函数并返回任务句柄。
     * @throws std::logic_error 调度器尚未启动或已经开始停止。
     */
    TaskHandle post(std::function<void()> fn);

    /** @brief 在将调用对象包装成 std::function 之前检查投递入口。 */
    template <typename F, std::enable_if_t<
        !std::is_same_v<std::decay_t<F>, std::function<void()>> &&
        std::is_constructible_v<std::function<void()>, F&&>, int> = 0>
    TaskHandle post(F&& fn) {
        _M_check_open();
        return _L_post(std::function<void()>(std::forward<F>(fn)));
    }

    /**
     * @brief 接管并投递尚未执行的协程。
     * @param co 由调用者独占、可用 delete 释放的协程。
     *           调用后由调度器负责释放，包括投递失败的情况。
     */
    void post_coroutine(Coroutine* co);

    /** @return 当前 worker 所属调度器，普通线程返回 nullptr。 */
    static Scheduler* current() noexcept;

    /**
     * @brief 挂起当前调度任务，直到被 resume_later 唤醒。
     * @details 调用时不得持有用户互斥锁。调度器停止时抛出异常。
     */
    void suspend_current();

    /**
     * @brief 从任意线程唤醒调度器拥有的协程。
     * @details 提前到达的唤醒可供下一次挂起消费。同一次挂起的
     *          重复唤醒会合并。调用者应保证 co 在调用期间有效。
     */
    void resume_later(Coroutine* co);

    /** @return 所有 worker 的就绪队列中等待执行的任务数。 */
    std::size_t pending_count() const;

private:
    friend class ioman;
    
    struct Impl;

    /// 启动 worker，并在进入调度循环前调用内部线程绑定函数。
    void _L_start(const std::function<void()>& bind_worker);

    /// 快速检查投递入口，关闭时抛出 std::logic_error。
    void _M_check_open() const;

    /// 创建任务并在队列锁内确认投递是否仍可接受。
    TaskHandle _L_post(std::function<void()> fn);

    std::unique_ptr<Impl> _M_impl;
};

} // namespace ioman
