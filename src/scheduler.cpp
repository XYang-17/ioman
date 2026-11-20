#include "scheduler.h"

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ioman {
namespace {

// worker 进入调度循环时设置，供该线程上的协程查询所属调度器。
thread_local Scheduler* current_scheduler = nullptr;

} // namespace

struct Scheduler::Impl {
    struct Task;

    /// 一个 worker 线程及其专用就绪队列。
    struct Worker {
        /// 队列有任务或调度器停止时，唤醒正在等待的 worker。
        std::condition_variable ready;

        /// 按入队顺序执行的任务队列。任务由 Impl::tasks 持有。
        std::list<Task*> queue;

        /// 执行调度循环的线程，由 start() 创建，由 stop() 等待退出。
        std::thread thread;
    };

    /// 调度器维护的单个协程任务。
    struct Task {
        Task(std::unique_ptr<Coroutine> owned_coroutine, const TaskHandle& task_handle)
            : coroutine(std::move(owned_coroutine)), handle(task_handle), slot{this} {}

        /// 由任务独占的协程，完成通知后随任务释放。
        std::unique_ptr<Coroutine> coroutine;

        /// 与 post() 返回的句柄共享状态，用于发布状态变化和执行结果。
        TaskHandle handle;

        /// 投递时绑定的 worker，后续恢复都在同一线程上进行。
        Worker* owner = nullptr;

        /// 显式挂起请求，供 worker 在协程交还控制权后决定是否等待外部唤醒。
        bool suspend_requested = false;

        /// 记录协程进入挂起状态前收到的唤醒。
        bool wake_pending = false;

        /// 任务未排队时保存其唯一的链表节点，入队时将该节点移入 worker 队列。
        std::list<Task*> slot;
    };

    /// 固定 worker 数量，使任务保存的 worker 指针保持有效。
    explicit Impl(const std::size_t worker_count) : workers(worker_count) {}

    // 持队列锁将任务放到所属 worker 的队尾。
    void enqueue(Task& task) {
        Worker& owner = *task.owner;
        task.handle._M_set_state(CoroutineState::READY);
        owner.queue.splice(owner.queue.end(), task.slot);
        owner.ready.notify_one();
    }

    /// 接管新协程，在队列锁下为它分配 worker 并入队。
    void submit(std::unique_ptr<Coroutine> coroutine, const TaskHandle& handle) {
        Coroutine* const key = coroutine.get();
        const CoroutineState phase = coroutine->state();
        if (CoroutineState::READY != phase) {
            throw std::invalid_argument("Only a new coroutine can be posted");
        }

        auto task = std::make_unique<Task>(std::move(coroutine), handle);
        const std::lock_guard<std::mutex> lock(mutex);
        if (!accepting_tasks.load(std::memory_order_relaxed)) {
            throw std::logic_error("Scheduler is not accepting tasks");
        }

        // 轮询分配新任务，任务再次入队时沿用此绑定。
        task->owner = &workers[next_worker];
        const auto inserted = tasks.try_emplace(key);
        inserted.first->second = std::move(task);
        next_worker = (next_worker + 1) % workers.size();
        enqueue(*inserted.first->second);
    }

    /// 在指定 worker 上取出并恢复任务，处理其挂起、重新入队或完成。
    void run_worker(Scheduler& scheduler, Worker& worker) {
        current_scheduler = &scheduler;
        while (true) {
            Task* task = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                worker.ready.wait(lock, [this, &worker]() {
                    return stopping || !worker.queue.empty();
                });
                // 收到停止信号后，先处理本 worker 尚未执行完的就绪任务。
                if (worker.queue.empty()) {
                    break;
                }

                task = worker.queue.front();
                task->slot.splice(task->slot.end(), worker.queue, worker.queue.begin());
                task->handle._M_set_state(CoroutineState::RUNNING);
            }

            // 切入协程前释放队列锁，协程可以在执行期间投递任务或请求挂起。
            std::exception_ptr error;
            try {
                task->coroutine->resume();
            } catch (...) {
                error = std::current_exception();
            }

            std::unique_ptr<Task> completed;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (nullptr != error || task->coroutine->finished()) {
                    // 先从任务表摘除，保留所有权直到完成通知结束。
                    const auto found = tasks.find(task->coroutine.get());
                    completed = std::move(found->second);
                    tasks.erase(found);
                } else if (task->suspend_requested && !task->wake_pending && !stopping) {
                    // 显式挂起且尚未收到唤醒，留在任务表中等待。
                    task->suspend_requested = false;
                    task->handle._M_set_state(CoroutineState::SUSPENDED);
                } else {
                    // 主动让出、已提前收到唤醒或正在停止时，重新排队。
                    if (task->suspend_requested) {
                        task->suspend_requested = false;
                        task->wake_pending = false;
                    }
                    enqueue(*task);
                }
            }

            // 在队列锁外通知等待者，完成回调也可以调用调度器。
            if (nullptr != completed) {
                completed->handle._L_finish(std::move(error));
            }
        }
        current_scheduler = nullptr;
    }

    /// 串行化 start() 和 stop()，包括线程创建和等待退出。
    std::mutex lifecycle_mutex;

    /// 保护任务表、就绪队列、轮询索引、挂起与唤醒标志和运行状态。
    std::mutex mutex;

    /// 构造时分配的 worker 集合，运行期间保持长度和元素地址稳定。
    std::vector<Worker> workers;

    /// 按协程地址索引并持有就绪、运行或挂起的任务。任务结束后先移出，再通知。
    std::unordered_map<Coroutine*, std::unique_ptr<Task>> tasks;

    /// 下一个新任务通过轮询分配到的 worker 下标。
    std::size_t next_worker = 0;

    /// 投递入口状态。worker 全部启动后设为 true，stop() 开始时设为 false。
    std::atomic<bool> accepting_tasks{false};

    /// worker 退出请求。stop() 或启动失败回滚时置位，回收全部 worker 后清除。
    bool stopping = false;
};

Scheduler::Scheduler(const std::size_t worker_count) {
    if (0 == worker_count) {
        throw std::invalid_argument("Scheduler requires at least one worker");
    }
    _M_impl = std::make_unique<Impl>(worker_count);
}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    _L_start({});
}

void Scheduler::_L_start(const std::function<void()>& bind_worker) {
    if (this == current()) {
        throw std::logic_error("Cannot start a scheduler from its own worker");
    }

    auto& impl = *_M_impl;
    const std::lock_guard<std::mutex> lifecycle_lock(impl.lifecycle_mutex);
    std::unique_lock<std::mutex> lock(impl.mutex);
    if (impl.accepting_tasks.load(std::memory_order_relaxed)) {
        return;
    }

    try {
        for (auto& worker : impl.workers) {
            worker.thread = std::thread([this, &impl, &worker, bind_worker]() {
                if (nullptr != bind_worker) {
                    bind_worker();
                }
                impl.run_worker(*this, worker);
            });
        }
        impl.accepting_tasks.store(true, std::memory_order_relaxed);
    } catch (...) {
        // 创建线程失败时，通知已启动的 worker 退出，再传播异常。
        impl.stopping = true;
        lock.unlock();
        for (auto& worker : impl.workers) {
            if (worker.thread.joinable()) {
                worker.ready.notify_one();
                worker.thread.join();
            }
        }
        lock.lock();
        impl.stopping = false;
        throw;
    }
}

void Scheduler::stop() {
    if (this == current()) {
        throw std::logic_error("Cannot stop a scheduler from its own worker");
    }

    auto& impl = *_M_impl;
    const std::lock_guard<std::mutex> lifecycle_lock(impl.lifecycle_mutex);
    {
        const std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.accepting_tasks.load(std::memory_order_relaxed)) {
            return;
        }
        impl.accepting_tasks.store(false, std::memory_order_relaxed);
        impl.stopping = true;
        // 让挂起的协程恢复到 suspend_current()，由该方法抛出停止异常。
        for (const auto& entry : impl.tasks) {
            Impl::Task& task = *entry.second;
            if (CoroutineState::SUSPENDED == task.handle.state()) {
                impl.enqueue(task);
            }
        }
    }

    // 在队列锁外等待 worker 退出，让它们可以继续取任务并收尾。
    for (auto& worker : impl.workers) {
        worker.ready.notify_one();
        worker.thread.join();
    }

    const std::lock_guard<std::mutex> lock(impl.mutex);
    impl.stopping = false;
    impl.next_worker = 0;
}

TaskHandle Scheduler::post(std::function<void()> fn) {
    _M_check_open();
    return _L_post(std::move(fn));
}

bool Scheduler::is_closed() const noexcept {
    return !_M_impl->accepting_tasks.load(std::memory_order_relaxed);
}

void Scheduler::post_coroutine(Coroutine* const co) {
    if (nullptr == co) {
        throw std::invalid_argument("Cannot post a null coroutine");
    }

    // 立即接管所有权，后续投递失败时也会释放协程。
    std::unique_ptr<Coroutine> coroutine(co);
    _M_check_open();
    const TaskHandle handle(co);
    _M_impl->submit(std::move(coroutine), handle);
}

Scheduler* Scheduler::current() noexcept {
    return current_scheduler;
}

void Scheduler::suspend_current() {
    Coroutine* const coroutine = Coroutine::current();
    if (this != current() || nullptr == coroutine) {
        throw std::logic_error("Only a scheduled coroutine can suspend itself");
    }

    auto& impl = *_M_impl;
    Impl::Task* task = nullptr;
    {
        const std::lock_guard<std::mutex> lock(impl.mutex);
        const auto found = impl.tasks.find(coroutine);
        if (impl.tasks.end() == found ||
            CoroutineState::RUNNING != found->second->handle.state()) {
            throw std::logic_error("Current coroutine is not a running task of this scheduler");
        }
        if (impl.stopping) {
            throw std::runtime_error("Scheduler is stopping");
        }
        task = found->second.get();
        // 先登记挂起意图。协程交还控制权后，worker 再决定是否入队。
        task->suspend_requested = true;
    }

    try {
        // 此处已释放队列锁，允许其他线程在切换期间调用 resume_later()。
        coroutine->yield();
    } catch (...) {
        // 切换失败时撤销挂起意图，协程仍可处理异常并继续执行。
        const std::lock_guard<std::mutex> lock(impl.mutex);
        task->suspend_requested = false;
        throw;
    }

    const std::lock_guard<std::mutex> lock(impl.mutex);
    task->wake_pending = false;
    if (impl.stopping) {
        throw std::runtime_error("Scheduler is stopping");
    }
}

void Scheduler::resume_later(Coroutine* const co) {
    auto& impl = *_M_impl;
    const std::lock_guard<std::mutex> lock(impl.mutex);
    const auto found = impl.tasks.find(co);
    if (impl.tasks.end() == found || impl.stopping) {
        return;
    }

    Impl::Task& task = *found->second;
    const CoroutineState phase = task.handle.state();
    if (CoroutineState::SUSPENDED == phase) {
        impl.enqueue(task);
    } else if (CoroutineState::RUNNING == phase) {
        // 协程尚未停在挂起点，让 worker 在处理挂起请求时消费这次唤醒。
        task.wake_pending = true;
    }
}

std::size_t Scheduler::pending_count() const {
    auto& impl = *_M_impl;
    const std::lock_guard<std::mutex> lock(impl.mutex);
    std::size_t count = 0;
    for (const auto& worker : impl.workers) {
        count += worker.queue.size();
    }
    return count;
}

void Scheduler::_M_check_open() const {
    if (is_closed()) {
        throw std::logic_error("Scheduler is not accepting tasks");
    }
}

TaskHandle Scheduler::_L_post(std::function<void()> fn) {
    auto coroutine = std::make_unique<Coroutine>(std::move(fn));
    const TaskHandle handle(coroutine.get());
    _M_impl->submit(std::move(coroutine), handle);
    return handle;
}

} // namespace ioman
