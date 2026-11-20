#include "thread_pool.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ioman {
namespace {

// 在获取关闭锁前识别本线程池的 worker，避免任务等待自己退出。
thread_local const ThreadPool* current_pool = nullptr;

} // namespace

struct ThreadPool::Impl {
    Impl(ThreadPool& pool, const std::size_t worker_count, const std::function<void()>& bind_worker)
        : worker_limit(worker_count),
          worker_entry([this, &pool, bind_worker] {
              if (nullptr != bind_worker) {
                  bind_worker();
              }
              run_worker(pool);
          }) {
        workers.reserve(worker_limit);
    }

    /// 取出 FIFO 队列中的任务，在队列锁外运行用户代码。
    void run_worker(const ThreadPool& pool) {
        current_pool = &pool;
        std::unique_lock<std::mutex> lock(queue_mutex);
        while (true) {
            ready.wait(lock, [this] {
                return !accepting_tasks.load(std::memory_order_relaxed) || !tasks.empty();
            });
            if (tasks.empty()) {
                break;
            }

            ++busy_workers;
            {
                auto task = std::move(tasks.front());
                tasks.pop_front();
                lock.unlock();
                task();
                // 函数对象也在锁外释放，释放完成后 worker 才能接下一项任务。
            }
            lock.lock();
            --busy_workers;
        }
        current_pool = nullptr;
    }

    /// 串行化关闭流程，确保每个 worker 只被等待一次。
    std::mutex shutdown_mutex;

    /// 保护任务队列、worker 登记及忙碌计数，并串行化入队和入口关闭。
    std::mutex queue_mutex;

    /// 有任务入队或线程池进入关闭状态时通知 worker。
    std::condition_variable ready;

    /// 等待 worker 领取的 FIFO 队列。首个 worker 启动失败时可撤销末尾的投递。
    std::deque<std::function<void()>> tasks;

    /// worker 数量上限，由构造参数指定。
    const std::size_t worker_limit;

    /// 各线程共用的入口函数。先绑定所属运行时，再进入取任务循环。
    const std::function<void()> worker_entry;

    /// 已经启动的 worker 句柄，包括尚未开始取任务的线程。
    std::vector<std::thread> workers;

    /// 已领取任务且仍在执行或释放任务的 worker 数。
    std::size_t busy_workers = 0;

    /// 投递入口状态。构造后即可接收任务，shutdown() 在队列锁内将其设为 false。
    std::atomic<bool> accepting_tasks{true};
};

ThreadPool::ThreadPool(const std::size_t worker_count) : ThreadPool(worker_count, {}) {}

ThreadPool::ThreadPool(
    const std::size_t worker_count,
    const std::function<void()>& bind_worker
) {
    if (0 == worker_count) {
        throw std::invalid_argument("ThreadPool requires at least one worker");
    }

    _M_impl = std::make_unique<Impl>(*this, worker_count, bind_worker);
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    if (_M_is_current_worker()) {
        throw std::logic_error("Cannot shut down a thread pool from its own worker");
    }

    auto& impl = *_M_impl;
    const std::lock_guard<std::mutex> shutdown_lock(impl.shutdown_mutex);
    {
        const std::lock_guard<std::mutex> queue_lock(impl.queue_mutex);
        if (!impl.accepting_tasks.load(std::memory_order_relaxed)) {
            return;
        }
        impl.accepting_tasks.store(false, std::memory_order_relaxed);
    }

    // 释放队列锁后再等待线程退出，让 worker 继续取出已接收的任务。
    impl.ready.notify_all();
    for (auto& worker : impl.workers) {
        worker.join();
    }
}

bool ThreadPool::_M_is_current_worker() const noexcept {
    return this == current_pool;
}

bool ThreadPool::is_closed() const noexcept {
    return !_M_impl->accepting_tasks.load(std::memory_order_relaxed);
}

void ThreadPool::_M_check_open() const {
    if (is_closed()) {
        throw std::logic_error("ThreadPool is shutting down");
    }
}

void ThreadPool::_L_enqueue(std::function<void()> task) {
    auto& impl = *_M_impl;
    {
        const std::lock_guard<std::mutex> lock(impl.queue_mutex);
        // 与 shutdown() 的写入使用同一把队列锁，入口检查过后仍可能开始关闭。
        _M_check_open();
        impl.tasks.push_back(std::move(task));

        // 尚未取任务的新 worker 也算可用容量，已排队的任务会占用这部分容量。
        const auto available = impl.workers.size() - impl.busy_workers;
        if (available < impl.tasks.size() && impl.workers.size() < impl.worker_limit) {
            try {
                impl.workers.emplace_back(std::cref(impl.worker_entry));
            } catch (...) {
                if (impl.workers.empty()) {
                    // 没有 worker 可以执行任务。移回参数，解锁后再销毁。
                    task = std::move(impl.tasks.back());
                    impl.tasks.pop_back();
                    throw;
                }
                // 已有 worker 可以继续排空队列，后续投递可以再次尝试扩容。
            }
        }
    }
    impl.ready.notify_one();
}

} // namespace ioman
