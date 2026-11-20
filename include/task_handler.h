#pragma once

#include "coroutine.h"

#include <functional>
#include <memory>

namespace ioman {

class Scheduler;
namespace detail {
class WaitEvent;
class CancellationState;
}
template <typename T>
class Promise;

/**
 * @brief 可复制的完成句柄，供调度任务、Future 和内部等待信号使用。
 * @details 句柄副本共享调度状态、异常和完成通知。Future 的返回值保存在其结果状态中。
 */
class TaskHandle {
public:
    /// 复制句柄，共享原任务的状态和完成通知。
    TaskHandle(const TaskHandle&) noexcept = default;

    /// 改为共享右侧句柄的状态，返回当前句柄的引用。
    TaskHandle& operator=(const TaskHandle&) noexcept = default;

    /** @return 已发布 DEAD 状态时返回 true，包括正常结束和异常退出。 */
    bool done() const noexcept;

    /**
     * @brief 原子读取状态，无需持有状态锁。
     * @return 调度任务的当前状态。仅用于完成通知时，状态从 READY 变为 DEAD。
     */
    CoroutineState state() const noexcept;

    /**
     * @brief 等待任务完成，并在失败时重新抛出任务异常。
     * @details 在普通线程中阻塞，在调度任务中挂起当前协程。
     *          目标已完成时直接检查异常。返回时，完成回调可能仍在执行。
     * @throws std::logic_error 目标尚未完成，且等待方无法协作式挂起或试图等待自身。
     * @throws std::runtime_error 所属调度器停止，导致协程挂起失败或提前结束。
     */
    void wait() const;

    /**
     * @brief 等待目标结束，目标异常可通过 error() 单独读取。
     * @details 普通线程会阻塞，调度协程会挂起。等待过程本身产生的异常仍会传播。
     */
    void wait_ready() const;

    /**
     * @brief 读取当前保存的异常，无需先等待目标完成。
     * @return 已发布的异常。目标尚未完成或正常结束时返回空指针。
     */
    std::exception_ptr error() const;

    /**
     * @brief 注册由共享状态持有的一次性完成回调。
     * @param callback 目标完成时执行的无参函数。
     * @details 目标已完成时在调用线程中立即执行，回调异常会传播给调用方。
     *          否则由发布完成状态的线程在锁外执行，回调异常会被隔离。
     * @throws std::invalid_argument callback 为空。
     */
    void on_done(std::function<void()> callback) const;

    /**
     * @brief 注册由调用方持有的一次性完成订阅。
     * @param callback 目标完成时执行的无参函数。
     * @return 订阅句柄。释放最后一个副本后，发布方尚未取得的回调会被跳过，
     *         已经取得的回调仍可能执行。
     * @details 目标已完成时在调用线程中立即执行，回调异常会传播给调用方。
     *          否则由发布线程在锁外执行，回调异常会被隔离。
     * @throws std::invalid_argument callback 为空。
     */
    std::shared_ptr<const std::function<void()>>
    subscribe(std::function<void()> callback) const;

private:
    friend class Scheduler;
    friend class detail::WaitEvent;
    friend class detail::CancellationState;
    template <typename T>
    friend class Promise;
    
    /** @brief 句柄副本共享的状态，包含异常、等待者、回调和订阅。 */
    struct State;

    /**
     * @brief 创建处于 READY 状态的完成句柄。
     * @param coroutine 用于检测协程是否等待自身的非拥有指针。
     *                  仅用于完成通知时传入空指针。
     */
    explicit TaskHandle(const Coroutine* coroutine);

    /**
     * @brief 原子更新任务的调度状态。
     * @param state 由调度器或任务完成流程确定的新状态。
     * @details 此方法只写入状态字段，调用时无需为这次写入持有任务状态锁。
     */
    void _M_set_state(CoroutineState state) const noexcept;

    /**
     * @brief 实现 wait() 和 wait_ready() 共用的等待流程。
     * @param rethrow 是否在等待结束后重新抛出目标异常。
     * @details 方法内部获取状态锁，在阻塞线程或挂起协程时释放该锁。
     */
    void _L_wait(bool rethrow) const;

    /**
     * @brief 保存异常，将状态设为 DEAD，并通知等待者、订阅者和完成回调。
     * @param error 任务退出时的异常，正常结束时为空。
     * @details 每个共享状态只发布一次。调用时须释放调度器队列锁和任务状态锁。
     *          协程等待者在状态锁内唤醒。普通线程在锁释放后收到通知，
     *          随后在锁外执行存活的订阅和完成回调。
     */
    void _L_finish(std::exception_ptr error) const;

private:
    /// 任务回收后，句柄的副本仍可持有并查询共享状态。
    std::shared_ptr<State> _M_state;
};

} // namespace ioman
