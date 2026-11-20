#include "sync/mutex.h"

#include "cancellation.h"
#include "scheduler.h"

#include <stdexcept>

namespace ioman {
namespace {

// 返回当前调度协程的身份，用于校验所有权。普通线程调用时抛出异常。
Coroutine* current_coroutine() {
    auto* const coroutine = Coroutine::current();
    if (nullptr == coroutine || nullptr == Scheduler::current()) {
        throw std::logic_error("ioman Mutex requires a scheduled coroutine");
    }
    return coroutine;
}

} // namespace

void Mutex::lock() {
    _L_lock(true);
}

bool Mutex::try_lock() {
    auto* const coroutine = current_coroutine();
    const std::lock_guard<std::mutex> lock(_M_mutex);
    if (nullptr != _M_owner) {
        return false;
    }
    _M_owner = coroutine;
    return true;
}

void Mutex::_L_lock(const bool interruptible) {
    auto* const coroutine = current_coroutine();
    if (interruptible) {
        cancellation_point();
    }

    std::shared_ptr<detail::WaitEvent> event;
    try {
        {
            const std::lock_guard<std::mutex> lock(_M_mutex);
            if (nullptr == _M_owner) {
                _M_owner = coroutine;
                return;
            }
            if (coroutine == _M_owner) {
                throw std::logic_error("ioman Mutex is not recursive");
            }
            // 在检查所有者的同一临界区登记，解锁方随后即可看到这次请求。
            event = std::make_shared<detail::WaitEvent>();
            _M_waiters.push_back({coroutine, event});
        }

        // 释放内部状态锁后再挂起，其他协程此时仍可解锁或登记。
        if (interruptible) {
            detail::wait_interruptibly(event);
        } else {
            event->wait();
        }
    } catch (...) {
        if (nullptr != event) {
            const std::lock_guard<std::mutex> lock(_M_mutex);
            // 等待失败时移除登记。若解锁方已经转交所有权，则将锁继续交给下一位。
            _M_waiters.remove_if([&event](const Waiter& entry) { return event == entry.event; });
            if (coroutine == _M_owner) {
                _U_release();
            }
        }
        if (interruptible) {
            throw;
        }

        // 无法登记或挂起时，仍通过协作式让出拿回条件变量的锁。
        while (!try_lock()) {
            coroutine->yield();
        }
    }
}

void Mutex::unlock() {
    auto* const coroutine = current_coroutine();
    const std::lock_guard<std::mutex> lock(_M_mutex);
    if (coroutine != _M_owner) {
        throw std::logic_error("Only the owner coroutine can unlock ioman Mutex");
    }
    _U_release();
}

void Mutex::_L_check_owner() const {
    auto* const coroutine = current_coroutine();
    const std::lock_guard<std::mutex> lock(_M_mutex);
    if (coroutine != _M_owner) {
        throw std::logic_error("ConditionVariable wait requires the current coroutine to own the Mutex");
    }
}

void Mutex::_U_release() {
    while (!_M_waiters.empty()) {
        const auto waiter = _M_waiters.front();
        _M_waiters.pop_front();
        _M_owner = waiter.coroutine;
        // 先转交所有权再通知。取消若已经发布，就尝试下一位。
        if (waiter.event->complete(detail::WakeReason::READY)) {
            return;
        }
    }
    _M_owner = nullptr;
}

} // namespace ioman
