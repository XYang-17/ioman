#pragma once

#include <list>
#include <memory>
#include <mutex>

namespace ioman {

class Coroutine;
class ConditionVariable;
namespace detail {
class WaitEvent;
}

/**
 * @brief 保护调度协程之间的共享数据。同一时刻只有一个协程持锁。
 * @details 以协程作为所有者。锁被占用时，lock() 挂起等待者，让 worker 继续调度。
 *          可用 std::lock_guard 或 std::unique_lock 在离开作用域时自动解锁。
 * @note 此锁不可递归获取。须在持有者释放锁且所有等待调用结束后再销毁锁。
 * @note 下列投递示例使用 ioman.h 中的 ioman::submit。
 */
class Mutex {
public:
    /**
     * @brief 创建一把可用的互斥锁。可在普通线程中创建，再交给调度协程使用。
     * @code
     * ioman::Mutex mutex;
     * @endcode
     */
    Mutex() = default;
    /// 禁止复制构造。所有权及等待队列绑定到原锁对象。
    Mutex(const Mutex&) = delete;
    /// 禁止复制赋值。共享同一把锁时传递该对象的引用。
    Mutex& operator=(const Mutex&) = delete;

    /**
     * @brief 获取锁。锁被占用时挂起当前协程，按排队顺序等待。
     * @throws std::logic_error 调用方不是调度协程，或已经持有该锁。
     * @throws TaskCancelled 当前协程在获取锁时响应取消请求。
     * @note 成功返回后，当前协程负责解锁。等待异常退出时会清理登记并归还可能已经转交的所有权。
     * @code
     * ioman::Mutex mutex;
     * int count = 0;
     * ioman::submit([&] {
     *     const std::lock_guard<ioman::Mutex> lock(mutex);
     *     ++count;
     * }).get();
     * @endcode
     */
    void lock();

    /**
     * @brief 立即尝试加锁。
     * @return 取得锁时返回 true。其他协程或当前协程已经持锁时返回 false。
     * @throws std::logic_error 调用方不是调度协程。
     * @note 此调用不会挂起，也不会检查任务取消。成功时须解锁。
     * @code
     * ioman::Mutex mutex;
     * ioman::submit([&] {
     *     std::unique_lock<ioman::Mutex> lock(mutex, std::try_to_lock);
     *     if (lock.owns_lock()) {
     *         // 访问受保护的数据。
     *     }
     * }).get();
     * @endcode
     */
    bool try_lock();

    /**
     * @brief 释放锁并向首个有效等待者转交所有权。
     * @throws std::logic_error 调用方不是持锁的调度协程。
     * @note 使用 RAII 锁管理所有权时，由包装对象负责解锁。
     * @code
     * ioman::Mutex mutex;
     * ioman::submit([&] {
     *     mutex.lock();
     *     mutex.unlock();
     * }).get();
     * @endcode
     */
    void unlock();

private:
    friend class ConditionVariable;

    /// 一次尚未完成的加锁请求，由队列按登记顺序保管。
    struct Waiter {
        Coroutine* coroutine; ///< 等待协程的非拥有指针，用于转交所有权。
        std::shared_ptr<detail::WaitEvent> event; ///< 本次加锁的共享事件，用于协调所有权转交与取消。
    };

    /**
     * @brief 获取锁，需要时登记并挂起当前协程。
     * @param interruptible 为 true 时检查并响应取消。条件变量传入 false 以保证先拿回锁。
     */
    void _L_lock(bool interruptible);
    /// 验证当前调度协程持有此锁，否则抛出 std::logic_error。条件变量在登记前调用。
    void _L_check_owner() const;
    /// 调用方须持有 _M_mutex。向首个有效等待者转交所有权，队列耗尽时清空所有者。
    void _U_release();

    mutable std::mutex _M_mutex; ///< 内部状态锁，串行化所有者和队列的访问，挂起前释放。
    Coroutine* _M_owner = nullptr; ///< 持锁协程的非拥有指针，空指针表示可直接获取。
    std::list<Waiter> _M_waiters; ///< 加锁等待队列，转交时从队首取出，取消时按事件移除。
};

} // namespace ioman
