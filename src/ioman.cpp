#include "ioman.h"

#include "scheduler.h"
#include "thread_pool.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace ioman {

/// 顶层运行时的共享状态，集中持有执行器、任务登记表和计时服务。
struct ioman::Impl {
    /// 单次投递的控制记录，在待执行表、执行包装和取消入口之间共享。
    struct Operation {
        /// 从投递包装接管取消回调，将它与本任务独立的取消状态关联。
        explicit Operation(Cancel callback) : cancel(std::move(callback)) {}

        /// 未开始任务的结果发布回调。任务被移出待执行表后，由取消或关闭流程调用。
        const Cancel cancel;

        /// 单次任务的取消状态。执行期间绑定到协程或池线程，供取消检查点读取异常。
        detail::CancellationState cancellation;
    };

    /// 任务控制记录的共享句柄，以记录地址标识一次投递。
    using Ticket = std::shared_ptr<Operation>;

    /// 尚未开始的任务控制记录集合。开始执行和取消都通过移除记录取得处理权。
    using Pending = std::unordered_set<Ticket>;

    /// 先准备调度器。执行线程在运行时能提供有效的弱引用后启动。
    explicit Impl(const std::size_t scheduler_workers) : scheduler(scheduler_workers) {}

    /**
     * @brief 与关闭和取消竞争任务的开始权，取得后执行用户任务。
     * @param ticket 对应待执行表中的控制记录。
     * @param run 执行用户函数并发布结果的包装。
     */
    void run_task(const Ticket& ticket, const std::function<void()>& run) {
        {
            const std::lock_guard<std::mutex> lock(task_mutex);
            // 取消或关闭已移走记录时，用户函数不得执行。
            if (0 == pending_tasks.erase(ticket)) {
                return;
            }
            ++active_count;
        }

        // 活动计数守卫，覆盖用户函数的完整执行期，包括协程暂时挂起的时间。
        struct ActiveTask {
            /// 借用的运行时状态，提供需要归还的活动计数及其锁。
            Impl& owner;

            /// 最后一个活动任务退出时，通知等待中的 shutdown()。
            ~ActiveTask() {
                const std::lock_guard<std::mutex> lock(owner.task_mutex);
                --owner.active_count;
                if (0 == owner.active_count) {
                    owner.idle.notify_all();
                }
            }
        };
        const ActiveTask active{*this};
        // 用户代码通过 cancellation_point() 读取该控制记录中的取消状态。
        const detail::CancellationScope cancellation(ticket->cancellation);
        run();
    }

    /**
     * @brief 尝试跳过尚未开始的任务，或通知已开始任务协作退出。
     * @param ticket 目标任务的控制记录。
     * @return 此次是否首次向该控制记录发布取消请求。
     */
    bool cancel_task(const Ticket& ticket) {
        bool pending = false;
        {
            const std::lock_guard<std::mutex> lock(task_mutex);
            pending = 0 != pending_tasks.erase(ticket);
        }
        if (pending) {
            // 任务已从待执行表移除。立即完成 Future，等待方无需等待 worker 取队列。
            const auto error = std::make_exception_ptr(TaskCancelled{});
            ticket->cancel(error);
        }
        return ticket->cancellation.request();
    }

    /// 在计时线程中按期限扫描注册项，唤醒到期且仍存活的事件。
    void run_timer() {
        std::unique_lock<std::mutex> lock(timer_mutex);
        while (!timer_closed) {
            if (timers.empty()) {
                timer_changed.wait(lock);
                continue;
            }

            const auto deadline = timers.begin()->first;
            if (Clock::now() < deadline) {
                // 新注册的期限可能更早。收到通知后重新读取首项。
                timer_changed.wait_until(lock, deadline);
                continue;
            }

            const auto event = timers.begin()->second.lock();
            timers.erase(timers.begin());
            // 完成回调可能再次注册计时事件，因此在计时锁外调用。
            lock.unlock();
            if (nullptr != event) {
                event->complete(detail::WakeReason::TIMEOUT);
            }
            lock.lock();
        }
    }

    /// 停止计时线程，并通知仍在等待的计时事件服务已经关闭。
    void stop_timer() {
        Timers remaining;
        {
            const std::lock_guard<std::mutex> lock(timer_mutex);
            timer_closed = true;
            remaining.swap(timers);
        }
        timer_changed.notify_one();
        timer_thread.join();
        for (const auto& entry : remaining) {
            const auto event = entry.second.lock();
            if (nullptr != event) {
                event->complete(detail::WakeReason::STOPPED);
            }
        }
    }

    /// 协程执行器。将用户任务投递到 worker，协程挂起后仍由原 worker 恢复。
    Scheduler scheduler;

    /// 独立的线程池执行器。其 worker 直接运行同步阻塞或计算密集的函数。
    std::unique_ptr<ThreadPool> pool;

    /// 关闭流程的互斥锁。跨越整个 shutdown() 持有，让并发关闭调用依次完成。
    std::mutex shutdown_mutex;

    /// 任务登记锁。串行化队列登记、活动计数更新和 accepting_tasks 的写入。
    std::mutex task_mutex;

    /// 活动任务清空的条件变量。shutdown() 在此等待，最后一个活动任务退出时通知它。
    std::condition_variable idle;

    /// 待开始任务的登记表。执行器的队列另存包装函数，由此表决定包装能否执行用户代码。
    Pending pending_tasks;

    /// 活动用户任务数，包含已经开始但暂时挂起的协程。关闭流程等它归零后再停止执行器。
    std::size_t active_count = 0;

    /// 顶层投递入口状态。shutdown() 在任务登记锁内将其设为 false。
    std::atomic<bool> accepting_tasks{true};

    /// 完整关闭标志，允许创建线程观察另一个线程执行的关闭是否已结束。
    std::atomic<bool> stopped{false};

    /// 按期限排序的计时登记表类型。同一时刻可有多个事件，登记项只持有事件的弱引用。
    using Timers = std::multimap<Clock::time_point, std::weak_ptr<detail::WaitEvent>>;

    /// 计时服务的互斥锁。保护 timers 和 timer_closed，执行事件回调及等待线程退出时释放。
    std::mutex timer_mutex;

    /// 计时线程的条件变量。登记新期限或请求关闭时，用它唤醒线程重新检查队首和关闭标志。
    std::condition_variable timer_changed;

    /// 当前计时登记表。首项给出最早期限，已退出等待的事件可随调用方释放。
    Timers timers;

    /// 计时服务的关闭标志。由 timer_mutex 保护，置位后新登记收到停止通知，计时线程退出。
    bool timer_closed = false;

    /// 独立计时线程的句柄。负责等待期限和发布到期通知，由 stop_timer() 回收。
    std::thread timer_thread;
};

/// 当前 OS 线程的运行时归属。析构默认实例时，两种弱引用仍然有效。
struct ioman::ThreadBinding {
    std::weak_ptr<Impl> owned; ///< 本线程创建的实例，不延长显式实例的生命周期。
    std::weak_ptr<Impl> managed; ///< 本线程作为执行线程时所属的运行时。
    std::unique_ptr<ioman> default_runtime; ///< 按需创建且随普通线程销毁的实例。
};

ioman::ThreadBinding& ioman::_M_thread_binding() noexcept {
    static thread_local ThreadBinding binding;
    return binding;
}

ioman::ioman(const std::size_t scheduler_workers, const std::size_t pool_workers)
{
    auto& binding = _M_thread_binding();
    if (!binding.managed.expired()) {
        throw std::logic_error("An ioman worker cannot create its own runtime");
    }
    const auto owned = binding.owned.lock();
    if (nullptr != owned && !owned->stopped.load()) {
        throw std::logic_error("This thread already owns an active ioman runtime");
    }

    _M_impl = std::make_shared<Impl>(scheduler_workers);
    // 一次绑定覆盖整个 OS 线程，包括任务完成回调和计时通知。
    const auto bind_worker = [owner = std::weak_ptr<Impl>(_M_impl)]() noexcept {
        _M_thread_binding().managed = owner;
    };
    _M_impl->pool.reset(new ThreadPool(pool_workers, bind_worker));
    _M_impl->scheduler._L_start(bind_worker);
    auto* const runtime = _M_impl.get();
    runtime->timer_thread = std::thread([runtime, bind_worker] {
        bind_worker();
        runtime->run_timer();
    });
    binding.owned = _M_impl;
}

ioman::ioman(std::shared_ptr<Impl> impl) noexcept
    : _M_impl(std::move(impl)), _M_owns_runtime(false) {}

ioman::~ioman() {
    if (_M_owns_runtime) {
        shutdown();
    }
}

void ioman::sleep_for(const Clock::duration duration) {
    sleep_until(_M_deadline(duration));
}

void ioman::sleep_until(const Clock::time_point deadline) {
    cancellation_point();
    const auto event = std::make_shared<detail::WaitEvent>();
    _L_arm_timer(deadline, event);
    // 对 sleep 而言，计时到期代表正常完成。计时服务停止则结束这次等待。
    if (detail::WakeReason::TIMEOUT != detail::wait_interruptibly(event)) {
        throw TaskCancelled{};
    }
}

void ioman::yield_now() {
    cancellation_point();
    auto* const coroutine = Coroutine::current();
    if (nullptr == coroutine) {
        std::this_thread::yield();
    } else {
        coroutine->yield();
    }
    // 执行权交还期间可能收到取消，恢复后再检查一次。
    cancellation_point();
}

TaskGroup ioman::task_group() {
    return TaskGroup(*this);
}

void ioman::shutdown() {
    auto& impl = *_M_impl;
    // 调度器、线程池和计时线程都不能等待自己的退出。
    if (_M_impl == _M_thread_binding().managed.lock()) {
        throw std::logic_error("Cannot shut down ioman from its own managed thread");
    }

    const std::lock_guard<std::mutex> shutdown_lock(impl.shutdown_mutex);
    Impl::Pending cancelled;
    {
        const std::lock_guard<std::mutex> lock(impl.task_mutex);
        if (!impl.accepting_tasks.load(std::memory_order_relaxed)) {
            return;
        }
        impl.accepting_tasks.store(false, std::memory_order_relaxed);
        // 在同一把锁下关闭入口并摘除待执行项，worker 此后只能跳过这些包装。
        cancelled.swap(impl.pending_tasks);
    }

    // 立即发布取消，即使所有 worker 都在等待尚未开始的任务也能继续。
    if (!cancelled.empty()) {
        const auto error = std::make_exception_ptr(TaskCancelled{});
        for (const auto& ticket : cancelled) {
            ticket->cancel(error);
        }
    }

    {
        // 保留两个执行器和计时线程，让活动任务仍能等待其他任务或计时事件。
        std::unique_lock<std::mutex> lock(impl.task_mutex);
        impl.idle.wait(lock, [&impl] { return 0 == impl.active_count; });
    }

    // 两边都已完成用户任务，剩余队列只包含会跳过用户代码的包装函数。
    impl.scheduler.stop();
    impl.pool->shutdown();
    impl.stop_timer();
    impl.stopped.store(true);
}

void ioman::_M_check_open(const Execution execution) const {
    const auto& impl = *_M_impl;
    if (!impl.accepting_tasks.load(std::memory_order_relaxed)) {
        throw std::logic_error("ioman is shutting down");
    }

    const bool executor_closed = Execution::COROUTINE == execution
        ? impl.scheduler.is_closed()
        : impl.pool->is_closed();
    if (executor_closed) {
        throw std::logic_error("The target ioman executor is not accepting tasks");
    }
}

std::function<bool()> ioman::_L_submit(
    const Execution execution, std::function<void()> run, Cancel cancel
) {
    auto& impl = *_M_impl;
    // 包装用户函数期间可能开始关闭。此处先检查，再分配任务登记记录。
    if (!impl.accepting_tasks.load(std::memory_order_relaxed)) {
        throw std::logic_error("ioman is shutting down");
    }

    const auto ticket = std::make_shared<Impl::Operation>(std::move(cancel));
    // 任务句柄可以比运行时活得更久，取消入口只借用仍存活的运行时。
    std::function<bool()> request = [owner = std::weak_ptr<Impl>(_M_impl), ticket] {
        const auto runtime = owner.lock();
        return nullptr != runtime && runtime->cancel_task(ticket);
    };
    // 保留本地包装函数直到释放锁，避免投递失败时在锁内销毁用户对象。
    const auto wrapper = [&impl, ticket, operation = std::move(run)] {
        impl.run_task(ticket, operation);
    };

    const std::lock_guard<std::mutex> lock(impl.task_mutex);
    // 关闭与登记使用同一把锁，以此处的检查决定是否接收任务。
    if (!impl.accepting_tasks.load(std::memory_order_relaxed)) {
        throw std::logic_error("ioman is shutting down");
    }

    // 先登记再入队。worker 开始执行前也要取得此锁，因此一定能看到登记结果。
    impl.pending_tasks.insert(ticket);
    try {
        if (Execution::COROUTINE == execution) {
            impl.scheduler.post(wrapper);
        } else {
            impl.pool->_L_enqueue(wrapper);
        }
    } catch (...) {
        // 执行器未接收任务时撤销登记，关闭流程无需再处理它。
        impl.pending_tasks.erase(ticket);
        throw;
    }
    return request;
}

ioman::Clock::time_point ioman::_M_deadline(const Clock::duration duration) noexcept {
    const auto now = Clock::now();
    if (duration <= Clock::duration::zero()) {
        return now;
    }
    // 先计算能安全加上 duration 的最晚起点，避免时间点加法溢出。
    const auto latest_start = Clock::time_point::max() - duration;
    return latest_start < now ? Clock::time_point::max() : now + duration;
}

void ioman::_L_arm_timer(
    const Clock::time_point deadline, const std::shared_ptr<detail::WaitEvent>& event
) {
    auto& impl = *_M_impl;
    auto outcome = detail::WakeReason::TIMEOUT;
    {
        const std::lock_guard<std::mutex> lock(impl.timer_mutex);
        if (impl.timer_closed) {
            outcome = detail::WakeReason::STOPPED;
        } else if (Clock::now() < deadline) {
            // 因结果就绪或取消而提前退出的等待，其计时事件可能已经释放。
            for (auto iterator = impl.timers.begin(); iterator != impl.timers.end();) {
                if (iterator->second.expired()) {
                    iterator = impl.timers.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            impl.timers.emplace(deadline, event);
            impl.timer_changed.notify_one();
            return;
        }
    }
    // 已到期或服务关闭时直接通知，回调在计时锁之外运行。
    event->complete(outcome);
}

void ioman::_L_wait_until(const TaskHandle& completion, const Clock::time_point deadline) {
    cancellation_point();
    if (completion.done()) {
        return;
    }
    // 完成订阅、计时器和调用方取消共用一次等待事件，只接受先到的通知。
    const auto event = std::make_shared<detail::WaitEvent>();
    const auto subscription = completion.subscribe([event] {
        event->complete(detail::WakeReason::READY);
    });
    _L_arm_timer(deadline, event);
    const auto reason = detail::wait_interruptibly(event);
    if (detail::WakeReason::TIMEOUT == reason) {
        throw TimeoutError{};
    }
    if (detail::WakeReason::READY != reason) {
        throw TaskCancelled{};
    }
}

/// 子任务分组的共享状态，汇总完成通知并交付 join() 的最终结果。
struct TaskGroup::State {
    /// 分组已关闭且全部子任务退出时，向 join() 发布一次结果。
    void publish_if_finished() {
        std::exception_ptr error;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (!closed || 0 != remaining || published) {
                return;
            }
            published = true;
            error = failure;
        }
        // 在锁外发布，避免等待者或完成回调重新进入分组时争用同一把锁。
        if (nullptr == error) {
            result.set_value();
        } else {
            result.set_exception(error);
        }
    }

    /**
     * @brief 结算一次子任务完成，或归还一次失败投递的预留计数。
     * @param error 子任务保存的异常。正常结束或归还预留时为空。
     */
    void finish(std::exception_ptr error) {
        if (nullptr != error) {
            try {
                std::rethrow_exception(error);
            } catch (const TaskCancelled&) {
                error = nullptr;
            } catch (...) {
                // 保留首个非取消异常供 join() 读取。
            }
        }

        std::vector<std::function<void()>> cancel;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            --remaining;
            if (nullptr != error && nullptr == failure) {
                failure = error;
                closed = true;
                cancel.swap(cancel_callbacks);
            }
        }
        // 取消排队任务时可能同步触发该任务的完成回调，因此必须在分组锁外调用。
        for (const auto& request : cancel) {
            request();
        }
        publish_if_finished();
    }

    /**
     * @brief 禁止分组继续投递子任务，必要时请求其余任务退出。
     * @param cancel_pending 为 true 时发出取消请求，为 false 时仅关闭投递入口。
     */
    void close(const bool cancel_pending) {
        std::vector<std::function<void()>> cancel;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            closed = true;
            if (cancel_pending) {
                cancel.swap(cancel_callbacks);
            }
        }
        for (const auto& request : cancel) {
            request();
        }
        publish_if_finished();
    }

    /// 分组完成结果的生产端。关闭且待结算计数归零后，向 join() 发布成功或首个失败。
    Promise<void> result;

    /// 分组互斥锁。保护取消回调表、错误记录、待结算计数和两个状态标志。
    std::mutex mutex;

    /// 子任务取消回调表。触发分组取消时整体移出，在锁外逐个发出请求。
    std::vector<std::function<void()>> cancel_callbacks;

    /// 分组失败记录。保留首个观察到的非取消异常，最终交给 join() 抛出。
    std::exception_ptr failure;

    /// 待结算的子任务数，包含尚在投递或登记中的预留项。完成或投递失败时减一。
    std::size_t remaining = 0;

    /// 子任务投递入口的关闭标志。join()、显式取消或首次失败均可置位。
    bool closed = false;

    /// 分组结果的发布权标志。锁内置位后，只有取得发布权的一方会在锁外写入 result。
    bool published = false;
};

TaskGroup::TaskGroup(ioman& loop)
    : _M_loop(loop._M_impl), _M_state(std::make_shared<State>()),
      _M_result(_M_state->result.get_future()) {}

TaskGroup::~TaskGroup() {
    cancel();
}

void TaskGroup::cancel() {
    _M_state->close(true);
}

void TaskGroup::join() {
    _M_state->close(false);
    try {
        _M_result.wait();
    } catch (...) {
        // 调用方取消时，先请求子任务退出，再传播调用方的异常。
        cancel();
        _M_result._M_completion.wait_ready();
        throw;
    }
    _M_result._M_read();
}

void TaskGroup::_L_reserve() {
    const std::lock_guard<std::mutex> lock(_M_state->mutex);
    if (_M_state->closed) {
        throw std::logic_error("Task group is closed");
    }
    // 投递前增加计数，防止快速完成的其他任务提前发布分组结果。
    ++_M_state->remaining;
}

void TaskGroup::_L_unreserve() {
    _M_state->finish(nullptr);
}

void TaskGroup::_L_track_completion(const TaskHandle& completion, std::function<void()> cancel) {
    // 先为订阅句柄留出空间。订阅已完成的子任务可能立即结算计数。
    _M_subscriptions.reserve(_M_subscriptions.size() + 1);
    bool closed = false;
    {
        const std::lock_guard<std::mutex> lock(_M_state->mutex);
        closed = _M_state->closed;
        if (!closed) {
            _M_state->cancel_callbacks.push_back(cancel);
        }
    }

    // 分组销毁后允许回调跳过处理，无需由子任务延长分组的生命周期。
    const std::weak_ptr<State> owner = _M_state;
    auto subscription = completion.subscribe([owner, completion] {
        const auto state = owner.lock();
        if (nullptr != state) {
            state->finish(completion.error());
        }
    });
    _M_subscriptions.push_back(std::move(subscription));
    if (closed) {
        // 预留与订阅之间可能有其他子任务失败并关闭分组。
        cancel();
    }
}

ioman detail::RuntimeAccess::current() {
    auto& binding = ioman::_M_thread_binding();
    // 受管线程沿用所属运行时，关闭期间的计时请求也走同一实例。
    if (auto managed = binding.managed.lock(); nullptr != managed) {
        return ioman(std::move(managed));
    }
    if (auto owned = binding.owned.lock(); nullptr != owned && !owned->stopped.load()) {
        return ioman(std::move(owned));
    }

    // 构造函数先登记新实例，再由线程局部记录接管其生命周期。
    binding.default_runtime = std::make_unique<ioman>();
    return ioman(binding.default_runtime->_M_impl);
}

void detail::RuntimeAccess::shutdown_current() {
    auto& binding = ioman::_M_thread_binding();
    // 当前线程继续持有归属时才执行关闭，避免为一次无事可做的关闭创建运行时。
    if (!binding.managed.expired()) {
        throw std::logic_error("Cannot shut down ioman from a managed thread");
    }
    if (auto owned = binding.owned.lock(); nullptr != owned) {
        ioman(std::move(owned)).shutdown();
    }
}

void sleep_for(const ioman::Clock::duration duration) {
    detail::RuntimeAccess::current().sleep_for(duration);
}

void sleep_until(const ioman::Clock::time_point deadline) {
    detail::RuntimeAccess::current().sleep_until(deadline);
}

void yield_now() {
    ioman::yield_now();
}

TaskGroup task_group() {
    return detail::RuntimeAccess::current().task_group();
}

void shutdown() {
    detail::RuntimeAccess::shutdown_current();
}

} // namespace ioman
