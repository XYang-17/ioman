#include "sync/condition_variable.h"

#include "cancellation.h"

#include <exception>
#include <stdexcept>

namespace ioman {

void ConditionVariable::wait(Mutex& mutex) {
    mutex._L_check_owner();
    cancellation_point();
    const auto event = std::make_shared<detail::WaitEvent>();
    {
        // 先登记再释放用户锁。生产者取得同一把锁后便可以修改条件并通知此等待者。
        const std::lock_guard<std::mutex> lock(_M_mutex);
        _M_waiters.push_back(event);
    }

    mutex.unlock();
    // 暂存挂起期间的异常，等当前协程恢复互斥锁所有权后再传播。
    std::exception_ptr failure;
    try {
        detail::wait_interruptibly(event);
    } catch (...) {
        failure = std::current_exception();
    }

    {
        const std::lock_guard<std::mutex> lock(_M_mutex);
        // 取消结束的等待可能仍在队列中。通知方已经取出的事件无需再清理。
        _M_waiters.remove(event);
    }
    // 重新加锁期间暂缓检查取消，调用方可在持锁状态下处理异常。
    mutex._L_lock(false);
    if (nullptr != failure) {
        std::rethrow_exception(failure);
    }
}
void ConditionVariable::wait(std::unique_lock<Mutex>& lock) {
    if (!lock.owns_lock()) {
        throw std::logic_error("ConditionVariable wait requires a locked Mutex");
    }
    // 由原等待流程临时释放并重新获取底层锁，RAII 包装保留其持锁记录。
    wait(*lock.mutex());
}

void ConditionVariable::notify_one() {
    while(true) {
        std::shared_ptr<detail::WaitEvent> event;
        {
            const std::lock_guard<std::mutex> lock(_M_mutex);
            if (_M_waiters.empty()) {
                return;
            }
            event = std::move(_M_waiters.front());
            _M_waiters.pop_front();
        }
        // 取消若先到，此次通知应继续交给下一名等待者。
        if (event->complete(detail::WakeReason::READY)) {
            return;
        }
    }
}

void ConditionVariable::notify_all() {
    decltype(_M_waiters) waiters;
    {
        const std::lock_guard<std::mutex> lock(_M_mutex);
        // 取出这一批等待者，释放内部锁后逐个发布通知。
        waiters.swap(_M_waiters);
    }
    for (const auto& event : waiters) {
        event->complete(detail::WakeReason::READY);
    }
}

void ConditionVariable::_L_check_lock(const std::unique_lock<Mutex>& lock) {
    if (!lock.owns_lock()) {
        throw std::logic_error("ConditionVariable wait requires a locked Mutex");
    }
    lock.mutex()->_L_check_owner();
}


} // namespace ioman
