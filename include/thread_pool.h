#pragma once

#include "promise.h"

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ioman {

class ioman;

/** @brief 在独立线程中执行计算任务或阻塞操作。 */
class ThreadPool {
public:
    /**
     * @brief 创建按需启动的线程池。
     * @param worker_count 最多可启动的 worker 数，必须大于零。
     * @details 投递时才会启动所需线程，已启动的线程留在池中复用。
     */
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief 投递一个可无参调用的任务。
     * @param fn 在线程池 worker 上执行的函数对象。
     * @return 可在线程或调度协程中等待的结果句柄。
     * @throws std::logic_error 线程池已经开始关闭。
     * @throws std::system_error 当前没有可执行任务的 worker，且启动首个 worker 失败。
     * @details 在复制或移动 fn 前检查关闭状态，入队时再次检查以处理并发关闭。
     */
    template <typename F>
    auto submit(F&& fn) -> Future<std::invoke_result_t<std::decay_t<F>&>> {
        using Result = std::invoke_result_t<std::decay_t<F>&>;

        _M_check_open();
        auto promise = std::make_shared<Promise<Result>>();
        auto future = promise->get_future();
        auto job = [promise, operation = std::forward<F>(fn)]() mutable {
            try {
                if constexpr (std::is_void_v<Result>) {
                    std::invoke(operation);
                    promise->set_value();
                } else {
                    decltype(auto) result = std::invoke(operation);
                    promise->set_value(std::forward<Result>(result));
                }
            } catch (...) {
                const auto error = std::current_exception();
                promise->set_exception(error);
            }
        };

        // 用共享所有权封装任务，使队列也能接收只可移动的函数对象。
        auto task = std::make_shared<decltype(job)>(std::move(job));
        _L_enqueue([task] { (*task)(); });
        return future;
    }

    /**
     * @brief 停止接收任务，执行完已接收的任务后等待所有 worker 退出。
     * @details 可重复调用。须由线程池外部的线程调用。
     */
    void shutdown();

    /**
     * @brief 查询线程池是否已停止接受新任务。
     * @return shutdown() 关闭投递入口后返回 true，此时已接收的任务仍可能在执行。
     * @note 此查询可跨线程调用。尚未启动任何 worker 的线程池仍可接受任务。
     */
    bool is_closed() const noexcept;

private:
    friend class ioman;
    struct Impl;

    /// 保存顶层运行时的绑定函数，待 worker 按需启动时在线程入口调用。
    ThreadPool(std::size_t worker_count, const std::function<void()>& bind_worker);

    /// 判断当前线程是否属于本线程池。
    bool _M_is_current_worker() const noexcept;

    /// 原子读取投递入口的状态，已关闭时抛出 std::logic_error。
    void _M_check_open() const;

    /// 持队列锁接收任务，并在已有容量不足时尝试启动一个 worker。
    void _L_enqueue(std::function<void()> task);

    std::unique_ptr<Impl> _M_impl;
};

} // namespace ioman
