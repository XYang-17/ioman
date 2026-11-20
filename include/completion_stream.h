#pragma once

#include "future.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace ioman {

/**
 * @brief 完成流交付的一项结果，记录它在输入列表中的位置。
 * @tparam T 对应 Future 的结果类型。
 */
template <typename T>
struct Completed {
    /// 对应 Future 在传给 CompletionStream 的列表中的零基下标。
    std::size_t index;

    /// 已就绪的原 Future 的副本。调用 get() 可取得值或重新抛出该项异常。
    Future<T> future;
};

/**
 * @brief 逐项交付已就绪的 Future，供一个线程或协程读取。
 * @tparam T 所有输入 Future 的共同结果类型。
 * @details 每个输入位置交付一次，顺序由完成回调取得队列锁并写入下标的先后决定。
 *          构造时已经就绪的 Future 会在注册时立即入队，按注册顺序处理。
 * @note 同一个流只允许有一个正在进行的 next() 调用。销毁流会释放订阅，
 *       已投递的任务仍可继续执行。
 */
template <typename T>
class CompletionStream {
public:
    /**
     * @brief 保存输入并为每一项注册完成回调。
     * @param futures 按调用方顺序排列的 Future 列表，可混合协程和线程池任务。
     * @details 流持有列表中的句柄，保留原输入下标。空列表的 next() 可立即返回空值。
     * @code
     * auto a = loop.submit([] { return 1; });
     * auto b = loop.submit_blocking([] { return 2; });
     * ioman::CompletionStream<int> stream(std::vector{a, b});
     * @endcode
     */
    explicit CompletionStream(std::vector<Future<T>> futures)
        : _M_state(std::make_shared<State>(std::move(futures))) {
        // 回调可能由其他线程执行，流释放状态后，尚未取得状态的回调直接跳过。
        const std::weak_ptr<State> owner = _M_state;
        for (std::size_t index = 0; index < _M_state->futures.size(); ++index) {
            const auto& future = _M_state->futures[index];
            _M_state->subscriptions[index] = future.on_ready([owner, index] {
                const auto state = owner.lock();
                if (nullptr != state) {
                    state->_L_push(index);
                }
            });
        }
    }

    /// 禁止复制读取游标和订阅所有权。
    CompletionStream(const CompletionStream&) = delete;

    /// 禁止用另一条流覆盖当前读取进度。
    CompletionStream& operator=(const CompletionStream&) = delete;

    /**
     * @brief 取出最早入队且还未读取的完成项。
     * @details 队列暂空时，普通线程阻塞，调度协程挂起。失败或被取消的输入也会入队，
     *          它的异常由返回的 Future::get() 交付。
     * @return 完成项，包含原输入下标和已就绪的 Future。全部取完后返回 std::nullopt。
     * @throws TaskCancelled 当前读取方收到取消请求。
     * @code
     * while (auto item = stream.next()) {
     *     const auto input_index = item->index;
     *     const int value = item->future.get();
     *     // 使用 input_index 将 value 对应回原输入。
     * }
     * @endcode
     */
    std::optional<Completed<T>> next() {
        return _L_next([](const auto&) {});
    }

private:
    friend class ioman;

    /// 持有 Future::on_ready() 返回的强引用，使完成回调在流存活期间保持登记。
    using Subscription = std::shared_ptr<const std::function<void()>>;

    /// 流与各生产者回调共享的数据，包括输入、完成顺序和当前等待事件。
    struct State {
        /**
         * @brief 接管输入，按输入项数预分配完成下标和订阅位置。
         * @param inputs 要观察的 Future 列表。
         */
        explicit State(std::vector<Future<T>> inputs)
            : futures(std::move(inputs)), ready(futures.size()), subscriptions(futures.size()) {}

        /**
         * @brief 记录一个已就绪的输入位置，必要时唤醒正在等待的消费者。
         * @param index 已完成的输入在 futures 中的下标，每个位置只登记一次。
         * @details 可由不同执行线程调用。锁内写入下标并取得等待事件，锁外通知。
         */
        void _L_push(const std::size_t index) {
            std::shared_ptr<detail::WaitEvent> event;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                ready[written++] = index;
                event = waiter.lock();
            }
            if (nullptr != event) {
                event->complete(detail::WakeReason::READY);
            }
        }

        /// 保留原输入顺序，读取下标后可返回对应 Future。
        const std::vector<Future<T>> futures;

        /// 预分配的完成下标缓冲区。区间 [read, written) 是还未取出的项。
        std::vector<std::size_t> ready;

        /// 每个输入位置对应的订阅，构造时填入，随共享状态释放。
        std::vector<Subscription> subscriptions;

        /// 保护缓冲区、读写游标和当前等待事件。
        std::mutex mutex;

        /// 指向消费者这一次等待的事件，消费者离开等待后事件可自行释放。
        std::weak_ptr<detail::WaitEvent> waiter;

        /// 下一个待取下标的位置，由消费者推进。
        std::size_t read = 0;

        /// 下一个写入位置，由各输入的完成回调在锁内推进。
        std::size_t written = 0;
    };

    /**
     * @brief 实现 next() 和 ioman::wait() 共用的取项逻辑。
     * @tparam Arm 为新等待事件注册计时的函数类型。
     * @param arm 在队列锁外调用，接收本次等待的事件。next() 传入空操作。
     * @return 一个完成项。流已读完或计时器先触发时返回 std::nullopt。
     * @details 等待超时后，已入队的项仍可在下一次读取时取得。
     * @throws TaskCancelled 当前读取方被取消或计时服务停止。
     */
    template <typename Arm>
    std::optional<Completed<T>> _L_next(const Arm& arm) {
        cancellation_point();
        while (true) {
            std::shared_ptr<detail::WaitEvent> event;
            {
                const std::lock_guard<std::mutex> lock(_M_state->mutex);
                if (_M_state->read < _M_state->written) {
                    const auto index = _M_state->ready[_M_state->read++];
                    return Completed<T>{index, _M_state->futures[index]};
                }
                if (_M_state->read == _M_state->futures.size()) {
                    return std::nullopt;
                }
                // 在检查队列的同一把锁下登记事件，避免完成回调在登记前漏掉唤醒。
                event = std::make_shared<detail::WaitEvent>();
                _M_state->waiter = event;
            }
            arm(event);
            const auto reason = detail::wait_interruptibly(event);
            if (detail::WakeReason::TIMEOUT == reason) {
                return std::nullopt;
            }
            if (detail::WakeReason::READY != reason) {
                throw TaskCancelled{};
            }
            // 恢复后回到锁内读取队列，完成通知只负责唤醒。
        }
    }

    /// 流持有共享状态，完成回调只能通过弱引用尝试访问它。
    std::shared_ptr<State> _M_state;
};

/** @brief 指定 ioman::wait() 何时停止等待输入。 */
enum class WaitMode {
    FIRST_COMPLETED, ///< 任一输入就绪时返回，包括成功、异常和取消。
    FIRST_EXCEPTION, ///< 发现带异常的输入时返回，包括取消。全成功时等到全部就绪。
    ALL_COMPLETED    ///< 等待所有输入进入就绪状态。
};

/**
 * @brief ioman::wait() 返回前对输入就绪状态的分类。
 * @details 两组下标各自按输入顺序排列。并发任务可能在分类期间或返回后完成。
 */
struct WaitResult {
    /// 逐项检查时已就绪的输入下标。对应结果可以是值或异常。
    std::vector<std::size_t> done;

    /// 逐项检查时尚未就绪的输入下标。仍可通过原 Future 继续等待。
    std::vector<std::size_t> pending;
};

} // namespace ioman
