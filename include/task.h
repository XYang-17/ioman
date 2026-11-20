#pragma once

#include "future.h"

namespace ioman {

/**
 * @brief 可请求取消的任务句柄，结果通过 Future 共享。
 * @tparam T 用户函数的结果类型，可以是值、左值引用或 void。
 * @details 由 submit_task()、submit_blocking_task() 或 TaskGroup 的投递方法返回。
 *          释放句柄不会自动取消任务。以下示例包含 <ioman/ioman.h>。
 */
template <typename T>
class Task {
public:
    /**
     * @brief 复制任务句柄，共享同一任务的结果和取消入口。
     * @param other 要复制的任务句柄。
     * @code
     * const auto original = ioman::submit_task([] { return 42; });
     * const auto copy = original;
     * const int value = copy.get(); // 42
     * @endcode
     */
    Task(const Task& other) = default;

    /**
     * @brief 让当前句柄改为引用另一个任务，原先引用的任务可继续执行。
     * @param other 要共享其结果和取消入口的句柄。
     * @return 当前句柄的引用。
     * @code
     * auto selected = ioman::submit_task([] { return 1; });
     * const auto second = ioman::submit_task([] { return 2; });
     * selected = second;
     * const int value = selected.get(); // 2
     * @endcode
     */
    Task& operator=(const Task& other) = default;

    /**
     * @brief 向任务请求协作式取消。
     * @details 尚未开始的任务会被跳过，其结果以 TaskCancelled 结束。
     *          已开始的任务在等待点或 cancellation_point() 处响应取消。
     *          此方法返回时，已开始的任务可能仍在运行或清理资源。
     * @return 发出新的取消请求时返回 true。已经就绪、已经请求过取消，
     *         或所属运行时已不可用时返回 false。
     * @code
     * const auto task = ioman::submit_task([] {
     *     ioman::sleep_for(std::chrono::seconds{1});
     * });
     * task.cancel();
     * task.wait(); // 确认目标任务已结束。
     * @endcode
     */
    bool cancel() const {
        return !_M_future.is_ready() && _M_cancel();
    }

    /**
     * @brief 立即查询任务结果是否已经发布。
     * @return 正常返回、异常或取消结果已发布时返回 true。
     * @note 一次性结果被取走后，此查询仍返回 true。
     * @code
     * const auto task = ioman::submit_task([] {});
     * task.wait();
     * const bool ready = task.is_ready(); // true
     * @endcode
     */
    bool is_ready() const noexcept {
        return _M_future.is_ready();
    }

    /**
     * @brief 取得同一任务的结果视图。
     * @details 通过此视图等待时，等待方的取消不会向目标任务传播。
     * @return 与当前句柄共享结果的 Future。
     * @code
     * const auto task = ioman::submit_task([] { return 42; });
     * const auto result = task.future();
     * const int value = result.get(); // 42
     * @endcode
     */
    Future<T> future() const {
        return _M_future;
    }

    /**
     * @brief 等待目标任务发布结果，保留结果供 get() 读取。
     * @details 普通线程会阻塞，调度协程会挂起并让出 worker。
     *          目标任务以值、异常或取消结束时，本方法均可正常返回。
     *          当前等待方被取消时，会向目标发出取消请求，再传播等待方的异常。
     *          在超时作用域内，由该作用域将自身触发的取消转换为 TimeoutError。
     * @throws TaskCancelled 当前等待方收到取消请求。
     * @code
     * const auto task = ioman::submit_task([] { return 42; });
     * task.wait();
     * const int value = task.get(); // 结果仍可读取。
     * @endcode
     */
    void wait() const {
        try {
            _M_future.wait();
        } catch (const TaskCancelled&) {
            cancel();
            throw;
        } catch (const detail::DeadlineCancelled&) {
            cancel();
            throw;
        }
    }

    /**
     * @brief 等待目标任务完成并读取结果。
     * @details 等待期间遵循 wait() 的取消传播规则，目标任务的异常在读取结果时抛出。
     *          可复制结果可以重复读取，只可移动的结果由所有共享句柄共同提取一次。
     * @return 目标任务的结果。T 为 void 时，正常完成后直接返回。
     * @throws TaskCancelled 当前等待方被取消，或目标任务以取消结束。
     * @throws std::future_error 只可移动的结果已被取走。
     * @code
     * const auto task = ioman::submit_blocking_task([] { return 42; });
     * const int value = task.get(); // 42
     * @endcode
     */
    T get() const {
        wait();
        return _M_future._M_read();
    }

private:
    friend class ioman;

    /**
     * @brief 由顶层运行时组装任务句柄。
     * @param future 对应任务的结果视图。
     * @param cancel 向该任务发出取消请求的函数，返回本次是否发出了新请求。
     */
    Task(const Future<T>& future, std::function<bool()> cancel)
        : _M_future(future), _M_cancel(std::move(cancel)) {}

    /// 结果句柄，与同一任务的其他句柄共享值或异常。
    Future<T> _M_future;

    /// 取消回调，由运行时提供，用来寻址该次投递并发布取消请求。
    std::function<bool()> _M_cancel;
};

/**
 * @brief 保护目标任务，阻断此次等待的取消传播。
 * @details 等待方仍可因取消或超时退出。目标任务自身的异常或被直接取消的结果
 *          仍会通过返回的 Future 交付。
 * @tparam T 目标任务的结果类型。
 * @param task 要观察的目标任务。
 * @return 与目标任务共享结果的 Future，可传给 wait_for() 以仅限制等待时间。
 * @code
 * const auto task = ioman::submit_task([] { return 42; });
 * try {
 *     ioman::wait_for(ioman::shield(task), std::chrono::milliseconds{5});
 * } catch (const ioman::TimeoutError&) {
 *     // 这次等待已结束，task 仍可继续。
 * }
 * @endcode
 */
template <typename T>
Future<T> shield(const Task<T>& task) {
    return task.future();
}

/**
 * @brief 复制已有的结果视图。通过 Future 等待时，取消本来就只影响等待方。
 * @tparam T 输入 Future 的结果类型。
 * @param future 要观察的结果视图。
 * @return 与输入共享结果的 Future。
 * @code
 * const auto result = ioman::submit([] { return 42; });
 * const auto observation = ioman::shield(result);
 * const int value = observation.get(); // 42
 * @endcode
 */
template <typename T>
Future<T> shield(const Future<T>& future) {
    return future;
}

} // namespace ioman
