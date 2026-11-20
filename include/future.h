#pragma once

#include "cancellation.h"

#include <future>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace ioman {

class ioman;
class TaskGroup;
template <typename T>
class Task;

/**
 * @brief 可跨线程或协程等待的异步结果句柄。
 * @tparam T 结果类型。T 可以是值、引用或 void。
 * @details 可通过 Promise::get_future() 或任务投递接口取得。以下示例包含 <ioman/promise.h>。
 */
template <typename T>
class Future {
public:
    /**
     * @brief 复制句柄，共享结果和完成状态。
     * @param other 要复制的结果句柄。只可移动的结果仍由所有副本共同提取一次。
     * @code
     * ioman::Promise<int> producer;
     * const auto original = producer.get_future();
     * const auto copy = original;
     * producer.set_value(42);
     * const int value = copy.get(); // 42
     * @endcode
     */
    Future(const Future& other) noexcept = default;

    /**
     * @brief 改为共享另一个句柄的结果和完成状态。
     * @param other 要共享其结果的句柄。
     * @return 当前句柄的引用。
     * @details 赋值释放当前句柄对旧结果的引用，旧结果的生产者仍可继续执行。
     * @code
     * ioman::Promise<int> first;
     * ioman::Promise<int> second;
     * auto result = first.get_future();
     * result = second.get_future();
     * second.set_value(42);
     * const int value = result.get(); // 42
     * @endcode
     */
    Future& operator=(const Future& other) noexcept = default;

    /**
     * @brief 立即查询结果是否已经发布。
     * @return 值、void 完成信号或异常已发布时返回 true。
     *         只可移动的值被取走后，结果仍处于已发布状态。
     * @note 结果尚未就绪时立即返回 false。
     * @code
     * ioman::Promise<int> producer;
     * const auto result = producer.get_future();
     * const bool before = result.is_ready(); // false
     * producer.set_value(42);
     * const bool after = result.is_ready(); // true
     * @endcode
     */
    bool is_ready() const noexcept {
        return _M_completion.done();
    }

    /**
     * @brief 等待生产者发布结果，保留结果供 get() 读取。
     * @details 普通线程会阻塞，调度协程会挂起并让出 worker。
     *          生产者发布异常时，等待可以正常返回，可通过 exception() 查看异常
     *          或通过 get() 重新抛出。取消本次等待时，生产者可以继续执行。
     * @throws TaskCancelled 当前等待方收到取消请求。
     * @note 返回时结果可以读取，完成回调可能仍在执行。
     * @code
     * ioman::Promise<void> producer;
     * const auto result = producer.get_future();
     * producer.set_value();
     * result.wait();
     * @endcode
     */
    void wait() const {
        detail::wait_for_completion(_M_completion);
    }

    /**
     * @brief 订阅一次结果就绪通知，值和异常都会触发通知。
     * @param callback 要执行的无参函数。已就绪时在调用线程中立即执行，
     *                 否则在发布结果的线程中执行。
     * @return 订阅句柄。保留它以接收通知。释放最后一个副本后，
     *         发布方尚未取得的回调会被跳过，已取得的回调仍可能执行。
     * @throws std::invalid_argument 回调为空。
     * @details 同步执行的回调异常会传给调用方。发布线程执行的回调异常会被隔离。
     * @note 回调若引用外部对象，调用方须保证对象在回调执行期间有效。
     * @code
     * ioman::Promise<int> producer;
     * const auto result = producer.get_future();
     * bool notified = false;
     * const auto subscription = result.on_ready([&notified] { notified = true; });
     * producer.set_value(42); // 本例的发布线程同步执行回调。
     * @endcode
     */
    auto on_ready(std::function<void()> callback) const {
        return _M_completion.subscribe(std::move(callback));
    }

    /**
     * @brief 查询生产者已发布的异常，直接读取当前状态。
     * @return 保存的异常指针。成功或尚未发布结果时返回空指针。
     * @note 查询不等待结果就绪，也不消耗结果。需要区分成功和未完成时，可配合 is_ready()。
     * @code
     * ioman::Promise<int> producer;
     * const auto result = producer.get_future();
     * producer.set_exception(std::make_exception_ptr(std::runtime_error("failed")));
     * const auto error = result.exception(); // 可保存或通过 std::rethrow_exception 抛出。
     * @endcode
     */
    std::exception_ptr exception() const {
        return _M_completion.error();
    }

    /**
     * @brief 等待并取得结果，或重新抛出生产者保存的异常。
     * @details 等待期间遵循 wait() 的取消规则。可复制的值和引用可以多次读取，
     *          只可移动的结果由所有共享句柄共同提取一次。
     * @return 已发布的值或引用。T 为 void 时，正常完成后直接返回。
     * @throws TaskCancelled 当前等待方被取消，或生产者发布了取消异常。
     * @throws std::future_error 生产者未发布结果就被销毁，或一次性结果已被提取。
     * @code
     * ioman::Promise<int> producer;
     * const auto result = producer.get_future();
     * producer.set_value(42);
     * const int value = result.get(); // 42
     * @endcode
     */
    T get() const {
        wait();
        return _M_read();
    }

private:
    friend class Promise<T>;
    friend class ioman;
    friend class TaskGroup;
    friend class Task<T>;

    /**
     * @brief 读取已发布的结果，由已确认就绪的内部调用方使用。
     * @return 保存的值或引用。T 为 void 时仅检查完成结果。
     * @details 读取失败时重新抛出保存的异常。提取一次性结果时持有结果状态锁。
     */
    T _M_read() const {
        if constexpr (_M_share_result) {
            const Result result = _M_result->value;
            return result.get();
        } else {
            const std::lock_guard<std::mutex> lock(_M_result->mutex);
            if (!_M_result->value.valid()) {
                throw std::future_error(std::future_errc::no_state);
            }
            return _M_result->value.get();
        }
    }

    /// void、引用和可复制值支持重复读取。
    static constexpr bool _M_share_result =
        std::is_void_v<T> || std::is_reference_v<T> || std::is_copy_constructible_v<T>;

    /// 可重复读取的结果使用 shared_future，只可移动的值使用一次性 future。
    using Result = std::conditional_t<_M_share_result, std::shared_future<T>, std::future<T>>;

    /// 多个 Future 句柄共享的结果状态。
    struct State {
        /**
         * @brief 接管标准 future，并按结果类型选择共享或一次性读取。
         * @param result 与生产者的标准 promise 关联的结果句柄。
         */
        explicit State(std::future<T> result) : value(std::move(result)) {}

        /// 读取由生产者发布的值、完成信号或异常。
        Result value;

        /// 串行化只可移动结果的提取，保证同一结果只被取走一次。
        std::mutex mutex;
    };

    /**
     * @brief 绑定结果与对应的完成信号，由 Promise::get_future() 调用。
     * @param result 提供实际结果或异常的标准 future。
     * @param completion 与生产者共享的完成句柄。
     */
    Future(std::future<T> result, const TaskHandle& completion)
        : _M_result(std::make_shared<State>(std::move(result))),
          _M_completion(completion) {}

    /// 持有结果状态，所有 Future 副本共享它。
    std::shared_ptr<State> _M_result;

    /// 负责等待结果就绪，并让普通线程或调度协程在发布后恢复。
    TaskHandle _M_completion;

    /// 可选聚合状态，随输出 Future 一起存活。
    std::shared_ptr<void> _M_keepalive;
};

} // namespace ioman
