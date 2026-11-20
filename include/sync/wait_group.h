#pragma once

#include <memory>
#include <mutex>

namespace ioman {

/**
 * @brief 用计数跟踪一组尚未完成的工作，计数归零时通知所有本轮等待者。
 * @details add()、done() 和 wait() 可由普通线程或调度协程并发调用。
 * @note 先登记工作，再启动工作或等待。所有引用该对象的调用结束后才可销毁它。
 * @note 下列投递示例使用 ioman.h 中的 ioman::submit。
 */
class WaitGroup {
public:
    /**
     * @brief 创建计数为零的等待组。第一次将计数增至正数时开始新一轮。
     * @code
     * ioman::WaitGroup group;
     * @endcode
     */
    WaitGroup() = default;
    /// 禁止复制构造。计数和等待轮次属于原对象。
    WaitGroup(const WaitGroup&) = delete;
    /// 禁止复制赋值。多个调用方通过引用共享同一个等待组。
    WaitGroup& operator=(const WaitGroup&) = delete;

    /**
     * @brief 将 n 加到当前计数，降至零时通知本轮所有等待者。
     * @param n 计数的改变量。正数登记工作，负数结算工作，零保留当前计数。
     * @throws std::logic_error 结果小于零。
     * @throws std::overflow_error 结果超出可表示范围。
     * @note 计数曾经归零后再次增加会开启新一轮。首次登记须先于任务投递或该轮的等待。
     * @code
     * ioman::WaitGroup group;
     * group.add(2);
     * // 两项工作都完成后，可以一次结算两项。
     * group.add(-2);
     * @endcode
     */
    void add(int n);

    /**
     * @brief 结算一项已登记的工作，等价于 add(-1)。最后一项结算时通知本轮等待者。
     * @throws std::logic_error 当前计数已经为零。
     * @note 每项登记的工作都应结算一次。工作失败或取消时也要结算。
     * @code
     * ioman::WaitGroup group;
     * group.add(1);
     * // 一项已登记的工作完成。
     * group.done();
     * @endcode
     */
    void done();

    /**
     * @brief 等待进入此方法时观察到的那一轮计数归零。
     * @details 普通线程阻塞，调度协程挂起。取消只结束当前调用方的等待。
     *          归零后可用 add() 开始下一轮，上一轮的等待者仍会收到通知。
     * @throws TaskCancelled 当前调用方的取消作用域触发了取消。
     * @note 方法先检查取消。观察到计数为零时立即返回。
     *       调用者可通过各个任务的结果对象读取返回值或异常。
     * @code
     * ioman::WaitGroup group;
     * group.add(1);
     * auto task = ioman::submit([&group] { group.done(); });
     * group.wait();
     * task.get();
     * @endcode
     */
    void wait() const;

private:
    struct Round; ///< 一轮从首次登记到计数归零的共享完成状态。

    mutable std::mutex _M_mutex; ///< 内部状态锁，使计数与当前轮次的读取和更新保持一致。
    int _M_count = 0; ///< 未完成的工作项数，零表示当前没有进行中的轮次。
    std::shared_ptr<Round> _M_round; ///< 当前轮次的共享引用。归零时移出，已有等待者保留各自的引用。
};

} // namespace ioman
