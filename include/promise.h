#pragma once

#include "future.h"

#include <exception>
#include <future>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ioman {

/**
 * @brief 为关联的 Future 发布一次值、完成信号或异常。
 * @tparam T 要交付的结果类型。T 可以是值、引用或 void。
 * @details 以下示例包含 <ioman/promise.h>。调用方须保证生产者在各发布调用完成前存活。
 */
template <typename T>
class Promise {
public:
    /**
     * @brief 创建尚未发布结果的生产者。
     * @details 构造后可调用 get_future() 交出结果句柄，也可先发布结果。
     * @code
     * ioman::Promise<int> producer;
     * const auto result = producer.get_future();
     * @endcode
     */
    Promise() : _M_completion(nullptr) {}

    /**
     * @brief 释放结果生产端。
     * @details 尚未发布结果时，先发布 broken_promise，再通知等待者。
     *          已取得的 Future 可通过 get() 观察到该异常。
     * @code
     * const auto result = [] {
     *     ioman::Promise<int> producer;
     *     return producer.get_future();
     * }();
     * try {
     *     result.get();
     * } catch (const std::future_error&) {
     *     // 生产者已销毁，结果以 broken_promise 结束。
     * }
     * @endcode
     */
    ~Promise() {
        if (!_M_completion.done()) {
            // 销毁标准 promise 会向其 future 发布 broken_promise。
            _M_result.reset();
            try {
                throw std::future_error(std::future_errc::broken_promise);
            } catch (...) {
                _M_completion._L_finish(std::current_exception());
            }
        }
    }

    /// 每个结果状态由一个 Promise 负责发布，禁止复制生产端。
    Promise(const Promise& other) = delete;

    /// 禁止通过复制赋值改绑生产端。
    Promise& operator=(const Promise& other) = delete;

    /**
     * @brief 获取与生产端关联的结果句柄，可在发布结果前或发布后调用。
     * @return 可供多个调用方复制和等待的 Future。
     * @throws std::future_error 当前 Promise 已经发放过结果句柄。
     * @code
     * ioman::Promise<int> producer;
     * producer.set_value(42);
     * const auto result = producer.get_future();
     * const int value = result.get(); // 42
     * @endcode
     */
    Future<T> get_future() {
        auto result = _M_result->get_future();
        return Future<T>(std::move(result), _M_completion);
    }

    /**
     * @brief 保存值或引用，然后通知等待者结果已就绪。
     * @tparam Value 与 T 的发布方式兼容的参数类型。
     * @param value 要交付的值或引用。
     * @details 值参数按调用时的值类别转发。T 为引用时，调用方应保证引用对象
     *          在使用结果期间有效。
     * @throws std::future_error 已发布过值、完成信号或异常。
     * @code
     * ioman::Promise<int> producer;
     * const auto result = producer.get_future();
     * producer.set_value(42);
     * const int value = result.get(); // 42
     * @endcode
     */
    template <typename Value>
    void set_value(Value&& value) {
        _M_result->set_value(std::forward<Value>(value));
        _M_completion._L_finish(nullptr);
    }

    /**
     * @brief 发布成功完成信号并通知等待者。
     * @details 用于 Promise<void>。
     * @throws std::future_error 已发布过完成信号或异常。
     * @code
     * ioman::Promise<void> producer;
     * const auto result = producer.get_future();
     * producer.set_value();
     * result.get(); // 只确认正常完成。
     * @endcode
     */
    void set_value() {
        _M_result->set_value();
        _M_completion._L_finish(nullptr);
    }

    /**
     * @brief 发布任务异常并通知等待者。
     * @param error 调用 Future::get() 时重新抛出的非空异常指针。
     * @throws std::invalid_argument error 为空。
     * @throws std::future_error 已发布过值、完成信号或异常。
     * @code
     * ioman::Promise<int> producer;
     * const auto result = producer.get_future();
     * producer.set_exception(std::make_exception_ptr(std::runtime_error("failed")));
     * try {
     *     result.get();
     * } catch (const std::runtime_error&) {
     *     // 在读取端处理生产者发布的异常。
     * }
     * @endcode
     */
    void set_exception(const std::exception_ptr& error) {
        if (nullptr == error) {
            throw std::invalid_argument("Promise exception cannot be empty");
        }
        _M_result->set_exception(error);
        _M_completion._L_finish(error);
    }

private:
    /// 保存结果生产端。未完成时可提前销毁它，先发布 broken_promise 再通知。
    std::optional<std::promise<T>> _M_result{std::in_place};

    /// 与 Future 共享的完成句柄，在结果或异常发布后通知等待者。
    TaskHandle _M_completion;
};

} // namespace ioman
