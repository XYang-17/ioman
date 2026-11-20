#pragma once

#include "mutex.h"

namespace ioman {

/**
 * @brief 让调度协程在共享条件暂未满足时挂起，收到通知后重新检查条件。
 * @details 调用方用同一把 Mutex 保护同一个共享条件的读写。通知可由任意线程发出。
 *          通知只影响已经登记的等待者。等待前先检查共享条件，醒来后再检查。
 * @note 条件变量和关联互斥锁须存活到所有等待及通知调用结束。
 * @note 下列投递示例使用 ioman.h 中的 ioman::submit。
 */
class ConditionVariable {
public:
    /**
     * @brief 创建条件变量，初始等待队列为空。
     * @code
     * ioman::ConditionVariable condition;
     * @endcode
     */
    ConditionVariable() = default;
    /// 禁止复制构造。等待登记绑定到原条件变量。
    ConditionVariable(const ConditionVariable&) = delete;
    /// 禁止复制赋值。共享同一条件变量时传递对象引用。
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    /**
     * @brief 登记等待、释放锁、挂起协程，恢复时重新加锁。
     * @param mutex 当前协程持有的互斥锁。同一条件应使用同一把互斥锁保护。
     * @details 返回时仍由当前协程持锁。等待过程中发生异常时，先重新加锁，再传播异常。
     * @throws std::logic_error 调用方不在调度协程中，或当前协程未持有 mutex。
     * @throws TaskCancelled 当前等待响应了取消请求。
     * @note 唤醒后应重新检查条件。此重载只操作实参 mutex 的底层所有权。
     * @code
     * ioman::Mutex mutex;
     * ioman::ConditionVariable condition;
     * bool ready = false;
     * auto consumer = ioman::submit([&] {
     *     std::unique_lock<ioman::Mutex> lock(mutex);
     *     while (!ready) {
     *         condition.wait(mutex);
     *     }
     * });
     * ioman::submit([&] {
     *     const std::lock_guard<ioman::Mutex> lock(mutex);
     *     ready = true;
     *     condition.notify_one();
     * }).get();
     * consumer.get();
     * @endcode
     */
    void wait(Mutex& mutex);

    /**
     * @brief 等待一次通知。挂起时释放底层互斥锁，返回或传播等待异常前重新加锁。
     * @param lock 当前协程持有的 RAII 锁，与用于保护条件的 Mutex 关联。
     * @throws std::logic_error lock 未持锁，或当前协程无该 Mutex 的所有权。
     * @throws TaskCancelled 当前等待响应了取消请求。
     * @note 等待期间 lock 保留其关联关系。唤醒后应重新检查条件。
     * @code
     * ioman::Mutex mutex;
     * ioman::ConditionVariable condition;
     * bool ready = false;
     * auto consumer = ioman::submit([&] {
     *     std::unique_lock<ioman::Mutex> lock(mutex);
     *     while (!ready) {
     *         condition.wait(lock);
     *     }
     * });
     * ioman::submit([&] {
     *     const std::lock_guard<ioman::Mutex> lock(mutex);
     *     ready = true;
     *     condition.notify_one();
     * }).get();
     * consumer.get();
     * @endcode
     */
    void wait(std::unique_lock<Mutex>& lock);

    /**
     * @brief 在持锁时检查条件。条件为 false 时挂起，恢复并加锁后再次检查。
     * @param lock 当前协程持有的锁。
     * @param predicate 读取该锁保护的条件，返回 true 时结束等待。
     * @throws std::logic_error lock 未持锁，或当前协程无底层 Mutex 的所有权。
     * @throws TaskCancelled 当前等待响应了取消请求。
     * @note 谓词在持锁时执行，可能执行多次。初次检查为 true 时直接返回。
     *       谓词自身的异常直接传播，调用方仍持锁。
     * @code
     * ioman::Mutex mutex;
     * ioman::ConditionVariable condition;
     * bool ready = false;
     * auto consumer = ioman::submit([&] {
     *     std::unique_lock<ioman::Mutex> lock(mutex);
     *     condition.wait(lock, [&] { return ready; });
     * });
     * ioman::submit([&] {
     *     const std::lock_guard<ioman::Mutex> lock(mutex);
     *     ready = true;
     *     condition.notify_one();
     * }).get();
     * consumer.get();
     * @endcode
     */
    template <typename Predicate>
    void wait(std::unique_lock<Mutex>& lock, Predicate predicate) {
        _L_check_lock(lock);
        while (!predicate()) {
            wait(lock);
        }
    }

    /**
     * @brief 通知最早登记且仍能接收通知的一名等待者。
     * @note 可从任意线程调用。队列空时直接返回。被唤醒的协程重新取得锁后才能继续。
     * @code
     * ioman::ConditionVariable condition;
     * condition.notify_one(); // 当前没有等待者，调用立即返回。
     * @endcode
     */
    void notify_one();

    /**
     * @brief 取出当前等待队列并通知其中所有仍在等待的协程。
     * @note 可从任意线程调用。取出队列后登记的等待者留待后续通知。
     *       多个协程使用同一把锁时，仍须分别重新取得锁并检查条件。
     * @code
     * ioman::Mutex mutex;
     * ioman::ConditionVariable condition;
     * bool ready = false;
     * auto consumer = ioman::submit([&] {
     *     std::unique_lock<ioman::Mutex> lock(mutex);
     *     condition.wait(lock, [&] { return ready; });
     * });
     * ioman::submit([&] {
     *     const std::lock_guard<ioman::Mutex> lock(mutex);
     *     ready = true;
     *     condition.notify_all();
     * }).get();
     * consumer.get();
     * @endcode
     */
    void notify_all();

private:
    /// 检查 lock 的持锁标记和当前协程的实际所有权，任一检查失败时抛出 std::logic_error。
    static void _L_check_lock(const std::unique_lock<Mutex>& lock);

    std::mutex _M_mutex; ///< 队列状态锁，协调等待登记、清理和通知取出，挂起或发布通知前释放。
    std::list<std::shared_ptr<detail::WaitEvent>> _M_waiters; ///< 等待事件队列，按登记顺序保存每次等待的共享事件。
};

} // namespace ioman
