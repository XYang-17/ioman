#pragma once

#include "promise.h"
#include "task.h"
#include "completion_stream.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ioman {

class TaskGroup;
namespace detail {
struct RuntimeAccess;
}

/**
 * @brief 统一提供协程投递、线程池投递、计时和组合等待。
 * @details 构造后即可投递。两个执行器都通过 Future 交付值或异常。
 *          示例中的 loop 为 ioman::ioman 实例，使用前包含 <ioman/ioman.h>，
 *          并按示例需要包含 <chrono>、<thread>、<vector> 或 <stdexcept>。
 */
class ioman {
public:
    /**
     * @brief 启动协程调度器和计时服务，并配置按需启动的线程池。
     * @param scheduler_workers 执行协程任务的线程数。
     * @param pool_workers 线程池最多可启动的 worker 数。两个参数都必须大于零。
     * @throws std::invalid_argument 任一 worker 数量为零。
     * @throws std::logic_error 当前线程已拥有活跃实例，或由 ioman 管理。
     * @throws std::system_error 启动线程失败。
     * @code
     * ioman::ioman loop(1, 4);
     * @endcode
     */
    explicit ioman(std::size_t scheduler_workers = 1, std::size_t pool_workers = 1);

    /**
     * @brief 离开作用域时调用 shutdown() 回收运行时。
     * @note 须从本运行时的 worker 之外销毁。若要确保某项工作已执行，先等待它完成。
     * @code
     * {
     *     ioman::ioman loop;
     *     loop.submit([] {}).get();
     * } // 作用域结束时回收 worker 和计时线程。
     * @endcode
     */
    ~ioman();

    /// 每个实例独占自己的执行器，禁止复制构造。
    ioman(const ioman&) = delete;

    /// 禁止通过复制赋值替换运行中的实例。
    ioman& operator=(const ioman&) = delete;

    /**
     * @brief 将无参函数作为协程任务投递到调度器。
     * @tparam F 可无参调用的函数对象类型。
     * @param fn 调度器 worker 要执行的函数。函数可通过 Future 等待主动挂起。
     * @return 可等待并取得函数结果的 Future。
     * @throws std::logic_error 事件循环已开始关闭。
     * @code
     * auto result = loop.submit([] { return 6 * 7; });
     * const int value = result.get();
     * @endcode
     */
    template <typename F>
    auto submit(F&& fn) -> Future<std::invoke_result_t<std::decay_t<F>&>> {
        return submit_task(std::forward<F>(fn)).future();
    }

    /**
     * @brief 将无参函数投递到独立线程池，适用于阻塞或计算密集任务。
     * @tparam F 可无参调用的函数对象类型。
     * @param fn 在线程池 worker 上直接执行的函数。
     * @return 可等待并取得函数结果的 Future。
     * @throws std::logic_error 事件循环已开始关闭。
     * @code
     * auto result = loop.submit_blocking([] {
     *     std::this_thread::sleep_for(std::chrono::milliseconds{5});
     *     return 42;
     * });
     * const int value = result.get();
     * @endcode
     */
    template <typename F>
    auto submit_blocking(F&& fn) -> Future<std::invoke_result_t<std::decay_t<F>&>> {
        return submit_blocking_task(std::forward<F>(fn)).future();
    }

    /**
     * @brief 投递协程任务并取得取消入口。
     * @tparam F 可无参调用的函数对象类型。
     * @param fn 由调度器恢复执行的函数。
     * @return Task，可请求取消，也可等待和读取结果。
     * @throws std::logic_error 事件循环已开始关闭。
     * @code
     * auto task = loop.submit_task([&loop] {
     *     loop.sleep_for(std::chrono::seconds{1});
     * });
     * task.cancel();
     * task.wait(); // 等任务退出。用 get() 可取得异常。
     * @endcode
     */
    template <typename F>
    auto submit_task(F&& fn) -> Task<std::invoke_result_t<std::decay_t<F>&>> {
        return _M_submit(std::forward<F>(fn), Execution::COROUTINE);
    }

    /**
     * @brief 投递线程池任务并取得取消入口。
     * @tparam F 可无参调用的函数对象类型。
     * @param fn 在线程池执行的函数。可通过 cancellation_point() 响应取消。
     * @return Task。任务运行后采用协作式取消，阻塞调用须自行支持中断。
     * @throws std::logic_error 事件循环已开始关闭。
     * @code
     * auto task = loop.submit_blocking_task([] {
     *     ioman::cancellation_point();
     *     return 42;
     * });
     * const int value = task.get();
     * @endcode
     */
    template <typename F>
    auto submit_blocking_task(F&& fn) -> Task<std::invoke_result_t<std::decay_t<F>&>> {
        return _M_submit(std::forward<F>(fn), Execution::BLOCKING);
    }

    /// 计时服务使用的单调时钟，绝对期限必须使用同一时钟。
    using Clock = std::chrono::steady_clock;

    /**
     * @brief 挂起当前协程或阻塞当前线程，直到持续时间结束。
     * @param duration 从现在起等待的时长，非正值立即到期。
     * @throws TaskCancelled 当前任务被取消或计时服务已关闭。
     * @code
     * loop.submit([&loop] {
     *     loop.sleep_for(std::chrono::milliseconds{5});
     * }).get();
     * @endcode
     */
    void sleep_for(Clock::duration duration);

    /**
     * @brief 等到单调时钟的指定时刻，等待方式与 sleep_for() 相同。
     * @param deadline 绝对期限。已经到达的期限立即完成等待。
     * @throws TaskCancelled 当前任务被取消或计时服务已关闭。
     * @code
     * const auto deadline = ioman::ioman::Clock::now() + std::chrono::milliseconds{5};
     * loop.sleep_until(deadline);
     * @endcode
     */
    void sleep_until(Clock::time_point deadline);

    /**
     * @brief 主动让出当前协程。在普通线程中调用 std::this_thread::yield()。
     * @details 调度任务让出后回到原 worker 的就绪队尾。切出前和恢复后都检查取消。
     * @throws TaskCancelled 当前任务已收到取消请求。
     * @code
     * auto task = loop.submit([] {
     *     ioman::ioman::yield_now();
     *     return 42;
     * });
     * const int value = task.get();
     * @endcode
     */
    static void yield_now();

    /**
     * @brief 等待 Future 就绪，超时只结束本次等待。
     * @tparam T Future 的结果类型。
     * @param future 要观察的结果。可用 shield(task) 取得受保护的 Future。
     * @param duration 最多等待的时长。非正值可用来尝试读取已就绪结果。
     * @return 已发布的值或引用。T 为 void 时只等待完成。
     * @throws TimeoutError 结果未能在期限内就绪。
     * @details 目标任务继续执行。就绪后的读取遵循 Future::get() 的结果提取规则。
     * @code
     * auto task = loop.submit_task([] { return 42; });
     * try {
     *     const int value = loop.wait_for(ioman::shield(task), std::chrono::seconds{1});
     * } catch (const ioman::TimeoutError&) {
     *     // 原任务仍可运行，之后可以调用 task.get()。
     * }
     * @endcode
     */
    template <typename T>
    T wait_for(const Future<T>& future, const Clock::duration duration) {
        _L_wait_until(future._M_completion, _M_deadline(duration));
        return future._M_read();
    }

    /**
     * @brief 等待可取消任务。超时或当前等待被取消时，向目标发出取消请求。
     * @tparam T 任务的结果类型。
     * @param task 要等待并可请求取消的目标任务。
     * @param duration 发出超时取消请求前允许等待的时长。
     * @return 期限内就绪的任务结果。T 为 void 时只等待完成。
     * @throws TimeoutError 到期后发出取消请求，并等待目标退出后抛出。
     * @details 总耗时可能超过 duration。若等待方本身被取消，则向目标转发请求并传播异常。
     * @code
     * auto task = loop.submit_task([&loop] { loop.sleep_for(std::chrono::seconds{1}); });
     * try {
     *     loop.wait_for(task, std::chrono::milliseconds{5});
     * } catch (const ioman::TimeoutError&) {
     *     // 目标任务已经退出。
     * }
     * @endcode
     */
    template <typename T>
    T wait_for(const Task<T>& task, const Clock::duration duration) {
        const auto future = task.future();
        try {
            _L_wait_until(future._M_completion, _M_deadline(duration));
        } catch (const TimeoutError&) {
            task.cancel();
            future._M_completion.wait_ready();
            throw;
        } catch (const TaskCancelled&) {
            task.cancel();
            throw;
        } catch (const detail::DeadlineCancelled&) {
            task.cancel();
            throw;
        }
        return future._M_read();
    }

    /**
     * @brief 按完成通知顺序逐项获取协程或线程池的结果。
     * @tparam T 所有输入的共同结果类型。
     * @param futures 要观察的一组 Future，可以来自不同执行器。
     * @return 单消费者结果流。next() 返回原下标和 Future，取完后返回空值。
     * @code
     * auto first = loop.submit([] { return 1; });
     * auto second = loop.submit_blocking([] { return 2; });
     * auto stream = loop.as_completed(std::vector{first, second});
     * while (auto item = stream.next()) {
     *     item->future.get(); // 可用 item->index 查找对应输入。
     * }
     * @endcode
     */
    template <typename T>
    static CompletionStream<T> as_completed(std::vector<Future<T>> futures) {
        return CompletionStream<T>(std::move(futures));
    }

    /**
     * @brief 等待一组 Future 满足条件或到达期限。
     * @tparam T 所有输入的共同结果类型。
     * @param futures 按原顺序编号的输入列表。
     * @param mode 停止条件，可以是首个完成、首个异常或全部完成。
     * @param deadline 可选绝对期限。缺省表示不限时。
     * @return 返回前查询到的已完成和待完成下标。超时仍返回这一划分。
     * @note 不提取输入的值，具体结果或异常由对应 Future 交付。
     * @code
     * auto first = loop.submit([] { return 1; });
     * auto second = loop.submit_blocking([] { return 2; });
     * const auto state = loop.wait(std::vector{first, second}, ioman::WaitMode::FIRST_COMPLETED);
     * @endcode
     */
    template <typename T>
    WaitResult wait(
        const std::vector<Future<T>>& futures,
        const WaitMode mode = WaitMode::ALL_COMPLETED,
        const std::optional<Clock::time_point> deadline = std::nullopt
    ) {
        auto stream = as_completed(futures);
        const auto arm = [this, deadline](const auto& event) {
            if (deadline.has_value()) {
                _L_arm_timer(*deadline, event);
            }
        };
        while (const auto completed = stream._L_next(arm)) {
            if (WaitMode::FIRST_COMPLETED == mode ||
                (WaitMode::FIRST_EXCEPTION == mode && nullptr != completed->future.exception())) {
                break;
            }
        }

        WaitResult result;
        for (std::size_t index = 0; index < futures.size(); ++index) {
            auto& target = futures[index].is_ready() ? result.done : result.pending;
            target.push_back(index);
        }
        return result;
    }

    /**
     * @brief 合并一组任务的结果，成功时保持输入顺序。
     * @tparam T 可复制的值类型或 void。引用和只可移动值可用 gather_settled()。
     * @param futures 要聚合的 Future 列表。
     * @return 值任务返回 Future<std::vector<T>>，void 任务返回 Future<void>。
     * @details 首个观察到的异常会令聚合结果就绪，其余任务仍可继续执行。
     *          空输入立即返回已就绪的聚合结果。
     * @code
     * auto first = loop.submit([] { return 1; });
     * auto second = loop.submit_blocking([] { return 2; });
     * const auto values = loop.gather(std::vector{first, second}).get(); // {1, 2}
     * @endcode
     */
    template <typename T>
    static auto gather(std::vector<Future<T>> futures) {
        static_assert(std::is_void_v<T> || (!std::is_reference_v<T> &&
            std::is_copy_constructible_v<T>), "Use gather_settled for reference or move-only results");
        using Result = std::conditional_t<std::is_void_v<T>, void, std::vector<T>>;
        const auto collect = [](const std::vector<Future<T>>& inputs) -> Result {
            if constexpr (!std::is_void_v<T>) {
                Result results;
                results.reserve(inputs.size());
                for (const auto& future : inputs) {
                    results.push_back(future._M_read());
                }
                return results;
            }
        };
        return _M_collect<Result>(std::move(futures), collect, true);
    }

    /**
     * @brief 等待所有输入都结束，并保留每项的值或异常。
     * @tparam T 所有输入的共同结果类型，可以是引用、只可移动值或 void。
     * @param futures 要观察的 Future 列表。
     * @return 按输入顺序交付原 Future 列表的聚合 Future。
     * @details 子任务失败或取消后仍等待其余输入。通过各项的 get() 读取实际结果。
     * @code
     * auto first = loop.submit([] { return 1; });
     * auto second = loop.submit([] { return 2; });
     * const auto items = loop.gather_settled(std::vector{first, second}).get();
     * const int value = items[1].get();
     * @endcode
     */
    template <typename T>
    static Future<std::vector<Future<T>>> gather_settled(std::vector<Future<T>> futures) {
        using Result = std::vector<Future<T>>;
        const auto collect = [](const Result& inputs) { return inputs; };
        return _M_collect<Result>(std::move(futures), collect, false);
    }

    /**
     * @brief 给当前执行的这段代码设置一个相对期限。
     * @tparam F 可无参调用的函数对象类型。
     * @param duration 从进入作用域起计算的时长，非正值立即到期。
     * @param operation 在当前线程或协程内执行的函数。
     * @return 函数的返回结果，保留引用和 void。
     * @throws TimeoutError 本作用域到期且函数执行到取消检查点时抛出。
     * @note 同一段代码中的多次等待共享此期限。阻塞的外部调用须自行支持中断。
     * @code
     * try {
     *     loop.timeout(std::chrono::milliseconds{5}, [&loop] {
     *         loop.sleep_for(std::chrono::seconds{1});
     *     });
     * } catch (const ioman::TimeoutError&) {
     *     // 本作用域的期限已到。
     * }
     * @endcode
     */
    template <typename F>
    decltype(auto) timeout(const Clock::duration duration, F&& operation) {
        return timeout_at(_M_deadline(duration), std::forward<F>(operation));
    }

    /**
     * @brief 给当前执行的代码设置单调时钟的绝对期限。
     * @tparam F 可无参调用的函数对象类型。
     * @param deadline 使用 Clock 计算的绝对期限。
     * @param operation 在期限作用域内同步调用的函数。
     * @return 函数的返回结果，保留引用和 void。
     * @throws TimeoutError 本作用域触发的取消传播到其边界时抛出。
     * @details 作用域可嵌套。外层触发的取消会继续向外传播，由外层处理。
     * @code
     * const auto deadline = ioman::ioman::Clock::now() + std::chrono::seconds{1};
     * const int value = loop.timeout_at(deadline, [] { return 42; });
     * @endcode
     */
    template <typename F>
    decltype(auto) timeout_at(const Clock::time_point deadline, F&& operation) {
        const detail::TimeoutScope scope;
        _L_arm_timer(deadline, scope.event());
        try {
            cancellation_point();
            return std::invoke(std::forward<F>(operation));
        } catch (const detail::DeadlineCancelled& error) {
            if (scope.owns(error)) {
                throw TimeoutError{};
            }
            throw;
        }
    }

    /**
     * @brief 创建一个子任务分组。
     * @return 绑定到当前运行时的 TaskGroup。分组的生命周期须短于运行时。
     * @details 通过分组投递子任务，然后调用 join() 等待全部退出。
     * @code
     * auto group = loop.task_group();
     * auto task = group.spawn([] { return 42; });
     * group.join();
     * const int value = task.get();
     * @endcode
     */
    TaskGroup task_group();

    /**
     * @brief 拒绝新任务，取消未开始的任务，并等待已开始的任务结束。
     * @details 被取消任务的 Future::get() 抛出 TaskCancelled。已开始的
     *          协程可以继续挂起和被唤醒。可从本事件循环之外重复调用。
     * @throws std::logic_error 从本运行时的任一 worker 调用。
     * @code
     * loop.submit([] {}).get();
     * loop.shutdown();
     * @endcode
     */
    void shutdown();

private:
    friend class TaskGroup;
    friend struct detail::RuntimeAccess;

    /// 持有两组执行器、计时器和任务登记。任务取消入口通过弱引用访问它。
    struct Impl;
    struct ThreadBinding;

    /// 借用已有运行时。此对象析构时只释放共享引用。
    explicit ioman(std::shared_ptr<Impl> impl) noexcept;

    /**
     * @brief 取得调用线程的运行时归属记录，供构造函数、worker 入口和自由函数使用。
     * @return 当前 OS 线程独有的 ThreadBinding 引用，有效至该线程退出。
     * @details 普通线程的记录保存它自己创建的实例，受管线程的记录保存它所属的实例。
     *          首次调用只初始化一份空记录。需要默认运行时时，由 RuntimeAccess::current() 创建。
     * @note 返回的引用仅供当前线程访问。引用存在时，记录里也可能没有活跃实例。
     */
    static ThreadBinding& _M_thread_binding() noexcept;

    /// 决定投递包装函数所使用的执行器。
    enum class Execution {
        COROUTINE, ///< 调度器协程 worker。
        BLOCKING   ///< 独立线程池 worker。
    };

    /// 将未开始任务的取消异常发布到其 Promise。
    using Cancel = std::function<void(const std::exception_ptr&)>;

    /// 在包装用户函数前检查顶层投递入口和目标执行器的状态。
    void _M_check_open(Execution execution) const;

    /**
     * @brief 在任务登记锁内检查关闭状态，再登记并投递任务。
     * @param execution 目标执行器。
     * @param run 执行用户函数并发布结果的包装。
     * @param cancel 任务尚未开始时发布取消的回调。
     * @return 可向该任务请求取消的函数。
     */
    std::function<bool()> _L_submit(Execution execution, std::function<void()> run, Cancel cancel);

    /**
     * @brief 将相对时长转成计时服务使用的绝对期限。
     * @param duration 相对时长。
     * @return 期限。非正值返回当前时间，超出时钟范围时取最大时刻。
     */
    static Clock::time_point _M_deadline(Clock::duration duration) noexcept;

    /**
     * @brief 注册计时事件，或者立即发布已到期或计时服务关闭的通知。
     * @param deadline 要触发的绝对期限。
     * @param event 等待方持有的事件，计时服务只保留弱引用。
     */
    void _L_arm_timer(Clock::time_point deadline, const std::shared_ptr<detail::WaitEvent>& event);

    /**
     * @brief 等待完成通知、计时到期或当前等待方取消。
     * @param completion 目标的完成句柄。
     * @param deadline 最迟等待到的绝对时刻。
     * @throws TimeoutError 计时通知先到。
     * @details 只处理等待过程，目标的返回值或异常由调用者读取。
     */
    void _L_wait_until(const TaskHandle& completion, Clock::time_point deadline);

    /**
     * @brief 为 gather() 和 gather_settled() 建立一组完成订阅。
     * @tparam Result 聚合结果的类型。
     * @tparam T 各输入 Future 的共同结果类型。
     * @tparam Collect 从输入生成聚合结果的函数类型。
     * @param inputs 要观察的 Future 列表。
     * @param collect 全部完成时生成结果的函数。
     * @param fail_fast 为 true 时立即发布首个观察到的输入异常。
     * @return 持有订阅状态并最终交付聚合结果的 Future。
     */
    template <typename Result, typename T, typename Collect>
    static Future<Result> _M_collect(
        std::vector<Future<T>> inputs, const Collect& collect, const bool fail_fast
    ) {
        using Subscription = std::shared_ptr<const std::function<void()>>;
        // 聚合 Future 持有此状态。输入的就绪回调通过弱引用访问它。
        struct State {
            /// 按输入项数预先建立订阅位置和待完成计数。
            State(std::vector<Future<T>> futures, const Collect& operation)
                : inputs(std::move(futures)), collect(operation), subscriptions(inputs.size()),
                  remaining(inputs.size()) {}

            /// 由取得发布权的回调在锁外发布结果，聚合过程中发生的异常也交给 Future。
            void publish(const std::exception_ptr& error) {
                try {
                    if (nullptr != error) {
                        promise.set_exception(error);
                    } else if constexpr (std::is_void_v<Result>) {
                        collect(inputs);
                        promise.set_value();
                    } else {
                        promise.set_value(collect(inputs));
                    }
                } catch (...) {
                    const auto failure = std::current_exception();
                    promise.set_exception(failure);
                }
            }

            const std::vector<Future<T>> inputs; ///< 保留输入顺序及各项结果。
            const Collect collect; ///< 在全部就绪时生成最终结果。
            std::vector<Subscription> subscriptions; ///< 持有每个输入的就绪回调。
            Promise<Result> promise; ///< 向聚合 Future 发布结果或异常。
            std::mutex mutex; ///< 保护待完成计数和发布权。
            std::size_t remaining; ///< 尚未处理就绪通知的输入数。
            bool published = false; ///< 为 true 时表示已有回调取得发布权。
        };

        const auto state = std::make_shared<State>(std::move(inputs), collect);
        auto result = state->promise.get_future();
        result._M_keepalive = state;
        if (state->inputs.empty()) {
            state->publish(nullptr);
            return result;
        }
        const std::weak_ptr<State> owner = state;
        for (std::size_t index = 0; index < state->inputs.size(); ++index) {
            state->subscriptions[index] = state->inputs[index].on_ready([owner, index, fail_fast] {
                const auto aggregate = owner.lock();
                if (nullptr == aggregate) {
                    return;
                }
                const auto error = fail_fast ? aggregate->inputs[index].exception() : nullptr;
                {
                    const std::lock_guard<std::mutex> lock(aggregate->mutex);
                    if (aggregate->published) {
                        return;
                    }
                    --aggregate->remaining;
                    if (nullptr == error && 0 != aggregate->remaining) {
                        return;
                    }
                    // 首个要求立即传播的异常，或最后一个完成项，取得发布权。
                    aggregate->published = true;
                }
                aggregate->publish(error);
            });
        }
        return result;
    }

    /**
     * @brief 将用户函数包装为可执行、可取消并能发布结果的任务。
     * @tparam F 可无参调用的函数对象类型。
     * @param fn 用户提交的函数。
     * @param execution 目标执行器。
     * @return 绑定到该次投递的 Task。
     */
    template <typename F>
    auto _M_submit(F&& fn, const Execution execution)
        -> Task<std::invoke_result_t<std::decay_t<F>&>> {
        using Result = std::invoke_result_t<std::decay_t<F>&>;
        using Function = std::decay_t<F>;

        _M_check_open(execution);
        const auto promise = std::make_shared<Promise<Result>>();
        // 用共享对象存放函数，使包装回调可以复制，也能接收只能移动的用户函数。
        const auto operation = std::make_shared<Function>(std::forward<F>(fn));
        const auto future = promise->get_future();

        const auto run = [promise, operation] {
            try {
                cancellation_point();
                if constexpr (std::is_void_v<Result>) {
                    std::invoke(*operation);
                    promise->set_value();
                } else {
                    decltype(auto) result = std::invoke(*operation);
                    promise->set_value(std::forward<Result>(result));
                }
            } catch (...) {
                const auto error = std::current_exception();
                promise->set_exception(error);
            }
        };
        // 尚未开始的任务跳过 run()，由这个回调交付取消结果。
        const auto cancel = [promise](const std::exception_ptr& error) {
            promise->set_exception(error);
        };
        auto request_cancel = _L_submit(execution, run, cancel);
        return Task<Result>(future, std::move(request_cancel));
    }

    /// 持有运行时状态。已取得状态的取消调用可以在其调用期间安全访问它。
    std::shared_ptr<Impl> _M_impl;

    /// 区分负责关闭的实例与只持有状态的内部访问对象。
    bool _M_owns_runtime = true;
};

/**
 * @brief 管理一组由同一运行时执行的子任务。
 * @details 通过 ioman::task_group() 创建。分组的操作由创建方调用，
 *          子任务完成通知可从任一执行器到达。示例中的 loop 为 ioman::ioman 实例。
 */
class TaskGroup {
public:
    /**
     * @brief 离开作用域时请求取消尚未完成的子任务。
     * @note 析构只发出请求。需要等子任务清理完毕时，显式调用 join()。
     * @code
     * {
     *     auto group = loop.task_group();
     *     group.spawn([] {});
     *     group.join();
     * } // 分组在运行时之前销毁。
     * @endcode
     */
    ~TaskGroup();

    /// 分组拥有一组独立的子任务订阅，禁止复制构造。
    TaskGroup(const TaskGroup&) = delete;

    /// 禁止通过复制赋值替换当前分组。
    TaskGroup& operator=(const TaskGroup&) = delete;

    /**
     * @brief 创建并登记一个协程子任务。
     * @tparam F 可无参调用的函数对象类型。
     * @param fn 调度协程要执行的函数。
     * @return 可检查、等待或取消该子任务的 Task。
     * @throws std::logic_error 分组或运行时已经关闭。
     * @code
     * auto group = loop.task_group();
     * auto child = group.spawn([] { return 42; });
     * group.join();
     * const int value = child.get();
     * @endcode
     */
    template <typename F>
    auto spawn(F&& fn) {
        return _L_spawn([&] { return _M_loop.submit_task(std::forward<F>(fn)); });
    }

    /**
     * @brief 在独立线程池中创建并登记一个子任务。
     * @tparam F 可无参调用的函数对象类型。
     * @param fn 线程池 worker 要执行的函数。
     * @return 可检查、等待或取消该子任务的 Task。
     * @throws std::logic_error 分组或运行时已经关闭。
     * @code
     * auto group = loop.task_group();
     * auto child = group.spawn_blocking([] { return 42; });
     * group.join();
     * const int value = child.get();
     * @endcode
     */
    template <typename F>
    auto spawn_blocking(F&& fn) {
        return _L_spawn([&] { return _M_loop.submit_blocking_task(std::forward<F>(fn)); });
    }

    /**
     * @brief 关闭分组的投递入口，并请求取消尚未完成的子任务。
     * @details 已开始的任务需要到取消检查点响应。此方法可能在子任务退出前返回。
     * @code
     * auto group = loop.task_group();
     * group.spawn([&loop] { loop.sleep_for(std::chrono::seconds{1}); });
     * group.cancel();
     * group.join(); // 等待子任务实际退出。
     * @endcode
     */
    void cancel();

    /**
     * @brief 关闭分组，等待全部子任务退出，再报告首个观察到的非取消异常。
     * @details 普通线程阻塞，调度协程挂起。等待方取消时，先请求各子任务退出，
     *          等待清理完成后再传播等待方的异常。
     * @code
     * auto group = loop.task_group();
     * group.spawn([] { throw std::runtime_error("child failed"); });
     * try {
     *     group.join();
     * } catch (const std::runtime_error&) {
     *     // 已等待所有子任务退出。
     * }
     * @endcode
     */
    void join();

private:
    friend class ioman;
    /// 完成回调与分组共享的计数、异常和取消入口。
    struct State;

    /**
     * @brief 为指定运行时建立分组并取得用于 join() 的 Future。
     * @param loop 分组借用的运行时。
     */
    explicit TaskGroup(ioman& loop);

    /**
     * @brief 为一次投递预留计数并订阅子任务完成，失败时撤销计数。
     * @tparam Submit 执行实际投递的可调用对象类型。
     * @param submit 调用 ioman 投递接口的函数。
     * @return 已登记的子任务句柄。
     */
    template <typename Submit>
    auto _L_spawn(const Submit& submit) {
        _L_reserve();
        auto task = [&] {
            try {
                return submit();
            } catch (...) {
                _L_unreserve();
                throw;
            }
        }();
        try {
            const auto future = task.future();
            _L_track_completion(future._M_completion, [task] { task.cancel(); });
        } catch (...) {
            _L_unreserve();
            task.cancel();
            throw;
        }
        return task;
    }

    /// 在投递前预留一个未完成计数。分组关闭时抛出 std::logic_error。
    void _L_reserve();
    /// 投递或订阅失败时归还预留的计数。
    void _L_unreserve();

    /**
     * @brief 登记子任务的取消入口和完成回调。
     * @param completion 子任务的完成句柄。
     * @param cancel 向该子任务发出取消请求的函数。
     * @details 需要事先预留计数。若分组在投递期间关闭，登记完成后请求取消子任务。
     */
    void _L_track_completion(const TaskHandle& completion, std::function<void()> cancel);

    /// 保留运行时状态供分组投递，分组的销毁不会关闭运行时。
    ioman _M_loop;

    /// 与并发到达的子任务完成通知共享。
    std::shared_ptr<State> _M_state;

    /// 在分组关闭且子任务全部结束后通知 join()。
    Future<void> _M_result;

    /// 持有各子任务的完成回调。
    std::vector<std::shared_ptr<const std::function<void()>>> _M_subscriptions;
};

namespace detail {

/**
 * @brief 自由函数使用的内部运行时访问器。
 * @details 按当前 OS 线程的身份选择运行时，并在调用期间持有其共享状态。
 *          用户通过命名空间下的自由函数或显式实例操作运行时。
 */
struct RuntimeAccess {
    /**
     * @brief 取得本次调用应使用的运行时。
     * @return 共享所选运行时状态的内部访问对象。该对象析构时只释放引用。
     * @details 先查受管线程的所属实例，再查普通线程拥有的实例。普通线程没有活跃
     *          实例时会构造默认实例。处于关闭过程的实例仍是当前调用的目标。
     * @throws std::system_error 创建默认实例时无法启动线程。
     */
    static ioman current();

    /**
     * @brief 关闭当前普通线程拥有的运行时。
     * @details 没有实例时直接返回。若另一个线程正在关闭该实例，则等待关闭完成。
     * @throws std::logic_error 当前线程是由 ioman 管理的执行线程。
     */
    static void shutdown_current();
};

} // namespace detail

// 以下公开示例包含 <ioman/ioman.h>。使用计时和集合示例时，也可直接包含 <chrono> 和 <vector>。

/**
 * @brief 将协程任务投递到当前线程对应的运行时。
 * @tparam F 可无参调用的函数对象类型。
 * @param fn 由调度器 worker 执行的函数。
 * @return 交付函数返回值或异常的 Future，支持在协程中挂起等待。
 * @details worker 使用所属实例。普通线程使用自己拥有的实例，必要时创建默认实例。
 * @throws std::logic_error 目标运行时已开始关闭。
 * @code
 * const auto result = ioman::submit([] { return 42; });
 * const int value = result.get();
 * @endcode
 */
template <typename F>
auto submit(F&& fn) {
    return detail::RuntimeAccess::current().submit(std::forward<F>(fn));
}

/**
 * @brief 在线程对应的运行时投递阻塞或计算密集任务。
 * @tparam F 可无参调用的函数对象类型。
 * @param fn 在独立线程池的 worker 上执行的函数。
 * @return 交付函数返回值或异常的 Future。
 * @throws std::logic_error 目标运行时已开始关闭。
 * @code
 * const auto result = ioman::submit_blocking([] { return 42; });
 * const int value = result.get();
 * @endcode
 */
template <typename F>
auto submit_blocking(F&& fn) {
    return detail::RuntimeAccess::current().submit_blocking(std::forward<F>(fn));
}

/**
 * @brief 将协程任务投递到线程对应的运行时，并取得取消入口。
 * @tparam F 可无参调用的函数对象类型。
 * @param fn 由调度器 worker 执行的函数。
 * @return 可通过 cancel() 请求取消、通过 get() 读取结果的 Task。
 * @details 已开始的任务在协作式检查点响应取消。
 * @throws std::logic_error 目标运行时已开始关闭。
 * @code
 * const auto task = ioman::submit_task([] {
 *     ioman::sleep_for(std::chrono::seconds{1});
 * });
 * task.cancel();
 * task.wait();
 * @endcode
 */
template <typename F>
auto submit_task(F&& fn) {
    return detail::RuntimeAccess::current().submit_task(std::forward<F>(fn));
}

/**
 * @brief 向线程对应运行时的线程池投递任务，并取得取消入口。
 * @tparam F 可无参调用的函数对象类型。
 * @param fn 由独立线程池执行的函数。
 * @return 可取消、可等待并可读取结果的 Task。
 * @note 运行中的代码须主动到达取消检查点。外部阻塞操作须自行支持中断。
 * @throws std::logic_error 目标运行时已开始关闭。
 * @code
 * const auto task = ioman::submit_blocking_task([] {
 *     ioman::cancellation_point();
 *     return 42;
 * });
 * const int value = task.get();
 * @endcode
 */
template <typename F>
auto submit_blocking_task(F&& fn) {
    return detail::RuntimeAccess::current().submit_blocking_task(std::forward<F>(fn));
}

/**
 * @brief 使用线程对应运行时的计时服务等待一段时长。
 * @param duration 相对时长，非正值立即到期。
 * @details 普通线程阻塞，调度协程挂起并让出 worker。
 * @throws TaskCancelled 当前等待被取消或计时服务停止。
 * @code
 * ioman::sleep_for(std::chrono::milliseconds{5});
 * @endcode
 */
void sleep_for(ioman::Clock::duration duration);

/**
 * @brief 等到指定的单调时钟时刻。
 * @param deadline 绝对期限。已到达的期限立即完成等待。
 * @details 普通线程阻塞，调度协程挂起。可将同一个期限用于多个等待。
 * @throws TaskCancelled 当前等待被取消或计时服务停止。
 * @code
 * const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5};
 * ioman::sleep_until(deadline);
 * @endcode
 */
void sleep_until(ioman::Clock::time_point deadline);

/**
 * @brief 主动让出当前执行权，无需创建运行时。
 * @details 调度协程回到原 worker 的就绪队尾。普通线程调用 std::this_thread::yield()。
 *          让出前和重新获得执行权后都会检查取消。
 * @throws TaskCancelled 当前任务已收到取消请求。
 * @code
 * ioman::submit([] { ioman::yield_now(); }).get();
 * @endcode
 */
void yield_now();

/**
 * @brief 等待 Future 就绪，超时只结束本次等待。
 * @tparam T 目标 Future 的结果类型。
 * @param future 要等待的结果句柄。可传入 shield(task) 取得的结果视图。
 * @param duration 相对时长，非正值只尝试读取已就绪结果。
 * @return 任务的值或引用。T 为 void 时只等待结束。
 * @details 目标任务可以继续执行。就绪后按 Future::get() 的规则提取结果或抛出异常。
 * @throws TimeoutError 本次等待先到期。
 * @code
 * const auto task = ioman::submit_task([] { return 42; });
 * try {
 *     ioman::wait_for(ioman::shield(task), std::chrono::milliseconds{5});
 * } catch (const ioman::TimeoutError&) {
 *     // 原任务仍可继续，之后可调用 task.get()。
 * }
 * @endcode
 */
template <typename T>
T wait_for(const Future<T>& future, const ioman::Clock::duration duration) {
    return detail::RuntimeAccess::current().wait_for(future, duration);
}

/**
 * @brief 带期限地等待一个可取消任务。
 * @tparam T 任务的结果类型。
 * @param task 要等待并可请求取消的目标。
 * @param duration 到期前允许等待的时长。
 * @return 期限内就绪的结果。T 为 void 时只等待结束。
 * @details 超时后会请求取消并等待目标退出，因此总耗时可能超过 duration。
 *          等待方自身被取消时，也会向目标转发取消请求。
 * @throws TimeoutError 期限到达且已经等到目标退出。
 * @code
 * const auto task = ioman::submit_task([] { ioman::sleep_for(std::chrono::seconds{1}); });
 * try {
 *     ioman::wait_for(task, std::chrono::milliseconds{5});
 * } catch (const ioman::TimeoutError&) {
 *     // 目标任务已退出。
 * }
 * @endcode
 */
template <typename T>
T wait_for(const Task<T>& task, const ioman::Clock::duration duration) {
    return detail::RuntimeAccess::current().wait_for(task, duration);
}

/**
 * @brief 给当前执行的一段代码设置相对期限。
 * @tparam F 可无参调用的函数对象类型。
 * @param duration 相对时长，非正值立即到期。
 * @param operation 在当前线程或协程中调用的函数。
 * @return 保留 operation 返回的值、引用或 void。
 * @note 期限通过取消检查点生效。同一个作用域中的多次等待共享该期限。
 * @throws TimeoutError 本作用域触发的取消传播到其边界。
 * @code
 * try {
 *     ioman::timeout(std::chrono::milliseconds{5}, [] {
 *         ioman::sleep_for(std::chrono::seconds{1});
 *     });
 * } catch (const ioman::TimeoutError&) {}
 * @endcode
 */
template <typename F>
decltype(auto) timeout(const ioman::Clock::duration duration, F&& operation) {
    return detail::RuntimeAccess::current().timeout(duration, std::forward<F>(operation));
}

/**
 * @brief 给当前执行的一段代码设置绝对期限。
 * @tparam F 可无参调用的函数对象类型。
 * @param deadline 以 std::chrono::steady_clock 表示的期限。
 * @param operation 在期限作用域内同步调用的函数。
 * @return 保留 operation 返回的值、引用或 void。
 * @details 可嵌套使用。外层期限触发的取消继续向外传播。
 * @throws TimeoutError 本作用域触发的取消传播到其边界。
 * @code
 * const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
 * const int value = ioman::timeout_at(deadline, [] { return 42; });
 * @endcode
 */
template <typename F>
decltype(auto) timeout_at(const ioman::Clock::time_point deadline, F&& operation) {
    return detail::RuntimeAccess::current().timeout_at(deadline, std::forward<F>(operation));
}

/**
 * @brief 按完成通知入队的顺序交付 Future，无需创建运行时。
 * @tparam T 输入的共同结果类型。
 * @param futures 要观察的 Future，可来自不同执行器或运行时。
 * @return 单消费者完成流。next() 给出原输入下标及 Future，全部取完时返回空值。
 * @note 单个输入的异常通过它的 Future::get() 读取。
 * @code
 * const auto a = ioman::submit([] { return 1; });
 * const auto b = ioman::submit_blocking([] { return 2; });
 * auto stream = ioman::as_completed(std::vector{a, b});
 * while (auto item = stream.next()) {
 *     item->future.get();
 * }
 * @endcode
 */
template <typename T>
CompletionStream<T> as_completed(std::vector<Future<T>> futures) {
    return ioman::as_completed(std::move(futures));
}

/**
 * @brief 等待一组 Future 达到指定条件或期限。
 * @tparam T 输入的共同结果类型。
 * @param futures 要观察的输入列表。
 * @param mode 首个完成、首个异常或全部完成，默认为全部完成。
 * @param deadline 可选的单调时钟绝对期限。
 * @return 按原输入顺序分类得到的 done 和 pending 下标。
 * @details 到期时仍返回分类。输入异常保留在各自的 Future 中。
 * @code
 * const auto a = ioman::submit([] { return 1; });
 * const auto b = ioman::submit_blocking([] { return 2; });
 * const auto state = ioman::wait(std::vector{a, b}, ioman::WaitMode::FIRST_COMPLETED);
 * @endcode
 */
template <typename T>
WaitResult wait(
    const std::vector<Future<T>>& futures, const WaitMode mode = WaitMode::ALL_COMPLETED,
    const std::optional<ioman::Clock::time_point> deadline = std::nullopt
) {
    return detail::RuntimeAccess::current().wait(futures, mode, deadline);
}

/**
 * @brief 按输入顺序聚合成功结果，无需创建运行时。
 * @tparam T 可复制的值类型或 void。
 * @param futures 要聚合的 Future 列表。
 * @return 值任务返回 Future<std::vector<T>>，void 任务返回 Future<void>。
 * @details 首个观察到的失败会立即使聚合结果就绪，其余输入可继续。
 * @code
 * const auto a = ioman::submit([] { return 1; });
 * const auto b = ioman::submit([] { return 2; });
 * const auto values = ioman::gather(std::vector{a, b}).get(); // {1, 2}
 * @endcode
 */
template <typename T>
auto gather(std::vector<Future<T>> futures) {
    return ioman::gather(std::move(futures));
}

/**
 * @brief 等待所有输入结束，并按原顺序交付它们的 Future。
 * @tparam T 输入的共同结果类型，可为引用、只可移动值或 void。
 * @param futures 要观察的 Future 列表。
 * @return 持有原 Future 列表的聚合 Future。
 * @details 输入失败或取消时仍等待其余输入。此函数无需创建运行时。
 * @code
 * const auto a = ioman::submit([] { return 1; });
 * const auto b = ioman::submit([] { return 2; });
 * const auto items = ioman::gather_settled(std::vector{a, b}).get();
 * const int value = items[1].get();
 * @endcode
 */
template <typename T>
auto gather_settled(std::vector<Future<T>> futures) {
    return ioman::gather_settled(std::move(futures));
}

/**
 * @brief 创建绑定到线程对应运行时的子任务组。
 * @return 可通过 spawn() 或 spawn_blocking() 投递子任务的 TaskGroup。
 * @note join() 关闭投递入口，等待子任务退出，并重新抛出首个观察到的非取消异常。
 * @code
 * auto group = ioman::task_group();
 * const auto child = group.spawn([] { return 42; });
 * group.join();
 * const int value = child.get();
 * @endcode
 */
TaskGroup task_group();

/**
 * @brief 关闭当前普通线程拥有的运行时。
 * @details 取消未开始任务，等待已开始任务退出并回收执行线程。线程没有实例时直接返回。
 *          完全关闭后，下次调用需要运行时的自由函数可创建新实例。
 * @throws std::logic_error 从受管线程调用。
 * @code
 * ioman::submit([] {}).get();
 * ioman::shutdown();
 * @endcode
 */
void shutdown();

} // namespace ioman
