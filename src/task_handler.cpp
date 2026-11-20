#include "task_handler.h"

#include "scheduler.h"

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ioman {

struct TaskHandle::State {
    /// 等待此任务完成的协程及其所属调度器。
    struct Waiter {
        /// 负责将等待协程重新入队的调度器。
        Scheduler* scheduler;

        /// 需要在任务完成时唤醒的协程。
        Coroutine* coroutine;
    };

    /// 保存用于自等待检查的协程身份。内部完成信号可以没有关联协程。
    explicit State(const Coroutine* const task) : coroutine(task) {}

    /// 非拥有指针，仅用于判断当前协程是否在等待自身。
    const Coroutine* const coroutine;

    /// 原子调度状态，state() 和 done() 读取时无需持有 mutex。
    std::atomic<CoroutineState> phase{CoroutineState::READY};

    /// 保护任务异常、协程等待者、完成回调和订阅，并协调线程等待与完成通知。
    std::mutex mutex;

    /// 任务完成时唤醒阻塞在 wait() 或 wait_ready() 中的普通线程。
    std::condition_variable completed;

    /// 保存任务异常，由 wait() 重新抛出。正常完成时为空。
    std::exception_ptr error;

    /// 已登记的协程等待者，由完成方持锁唤醒，由等待者自行注销。
    std::list<Waiter> waiters;

    /// 已注册的完成回调，任务完成时移出并在锁外执行。
    std::vector<std::function<void()>> callbacks;

    /// 弱引用订阅回调，调用方可通过释放订阅句柄放弃后续通知。
    std::list<std::weak_ptr<const std::function<void()>>> subscriptions;
};

TaskHandle::TaskHandle(const Coroutine* const coroutine)
    : _M_state(std::make_shared<State>(coroutine)) {}

bool TaskHandle::done() const noexcept {
    return CoroutineState::DEAD == state();
}

CoroutineState TaskHandle::state() const noexcept {
    return _M_state->phase.load();
}

void TaskHandle::wait() const {
    _L_wait(true);
}

void TaskHandle::wait_ready() const {
    _L_wait(false);
}

std::exception_ptr TaskHandle::error() const {
    const std::lock_guard<std::mutex> lock(_M_state->mutex);
    return _M_state->error;
}

void TaskHandle::_L_wait(const bool rethrow) const {
    const auto state = _M_state;
    Scheduler* const scheduler = Scheduler::current();
    Coroutine* const coroutine = Coroutine::current();
    std::unique_lock<std::mutex> lock(state->mutex);

    if (!done()) {
        if (nullptr == scheduler && nullptr == coroutine) {
            // 等待期间释放互斥锁，唤醒后重新检查任务是否完成。
            state->completed.wait(lock, [this]() { return done(); });
        } else {
            // 协作式等待要求有调度器和当前协程，且任务不能等待自身。
            if (nullptr == scheduler || nullptr == coroutine || coroutine == state->coroutine) {
                throw std::logic_error("Cannot wait for this task from the current context");
            }

            // 在同一把锁下检查完成状态并登记，避免错过完成通知。
            const auto waiter = state->waiters.insert(
                state->waiters.end(), {scheduler, coroutine}
            );
            while (!done()) {
                // 发布方需要取得此锁。若唤醒早于挂起，调度器会保留该通知。
                lock.unlock();
                try {
                    scheduler->suspend_current();
                } catch (...) {
                    // 挂起失败或调度器停止时，先注销等待者再传播异常。
                    lock.lock();
                    state->waiters.erase(waiter);
                    throw;
                }
                // 恢复执行后重新检查目标状态，确认是否还需要等待。
                lock.lock();
            }
            state->waiters.erase(waiter);
        }
    }

    // 在锁内取得异常快照，释放锁后再按调用方要求重新抛出。
    const std::exception_ptr error = state->error;
    lock.unlock();
    if (rethrow && nullptr != error) {
        std::rethrow_exception(error);
    }
}

void TaskHandle::on_done(std::function<void()> callback) const {
    if (nullptr == callback) {
        throw std::invalid_argument("Task completion callback cannot be empty");
    }

    {
        // 在锁内决定登记还是立即执行，避免与任务完成交错而漏掉回调。
        const std::lock_guard<std::mutex> lock(_M_state->mutex);
        if (!done()) {
            _M_state->callbacks.push_back(std::move(callback));
            return;
        }
    }
    // 用户回调可以再次访问任务句柄，因此在锁外调用。
    callback();
}

void TaskHandle::_M_set_state(const CoroutineState state) const noexcept {
    _M_state->phase.store(state);
}

std::shared_ptr<const std::function<void()>> TaskHandle::subscribe(
    std::function<void()> callback
) const {
    if (nullptr == callback) {
        throw std::invalid_argument("Task completion callback cannot be empty");
    }
    // 调用方持有强引用，状态只保存弱引用。
    const auto subscription =
        std::make_shared<const std::function<void()>>(std::move(callback));
    {
        const std::lock_guard<std::mutex> lock(_M_state->mutex);
        if (!done()) {
            auto& subscriptions = _M_state->subscriptions;
            // 登记时顺便清理已经释放的订阅。
            subscriptions.remove_if([](const auto& entry) { return entry.expired(); });
            subscriptions.push_back(subscription);
            return subscription;
        }
    }
    // 任务已完成，退出锁的作用域后立即通知，允许回调再访问句柄。
    (*subscription)();
    return subscription;
}

void TaskHandle::_L_finish(std::exception_ptr error) const {
    std::vector<std::function<void()>> callbacks;
    std::list<std::weak_ptr<const std::function<void()>>> subscriptions;
    {
        const std::lock_guard<std::mutex> lock(_M_state->mutex);
        // 先保存异常，再发布完成状态，确保等待者能取得任务结果。
        _M_state->error = std::move(error);
        _M_set_state(CoroutineState::DEAD);
        // 将回调移出共享状态，统一在锁外执行。
        callbacks.swap(_M_state->callbacks);
        subscriptions.swap(_M_state->subscriptions);

        // 持锁唤醒，保证等待者注销后不会再访问其调度器和协程。
        for (const auto& waiter : _M_state->waiters) {
            waiter.scheduler->resume_later(waiter.coroutine);
        }
    }
    _M_state->completed.notify_all();

    // 先通知仍存活的订阅，再执行共享状态持有的完成回调。
    for (const auto& entry : subscriptions) {
        const auto subscription = entry.lock();
        if (nullptr != subscription) {
            try {
                (*subscription)();
            } catch (...) {
                // 完成通知与用户回调独立，单个回调失败不能打断其他等待者。
            }
        }
    }

    // 等待者已收到通知，在锁外执行登记的用户回调。
    for (const auto& callback : callbacks) {
        try {
            callback();
        } catch (...) {
            // 用户回调的异常不能中断 worker 或跳过其它回调。
        }
    }
}

} // namespace ioman
