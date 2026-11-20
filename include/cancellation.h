#pragma once

#include "task_handler.h"

#include <atomic>
#include <exception>
#include <memory>

namespace ioman {

/**
 * @brief 表示普通取消的公开异常。
 * @details 任务在执行前被取消时通过 Future 交付。正在执行的代码也可以在取消检查点收到它。
 */
class TaskCancelled final : public std::exception {
public:
    /** @return 指向静态字符串的指针，由异常对象以外的存储持有。 */
    const char* what() const noexcept override;
};

/**
 * @brief 表示调用方设置的期限已经到达。
 * @details 带期限的等待可直接抛出此异常。timeout() 作用域只在捕获到自己触发的
 *          DeadlineCancelled 时才将其转换为 TimeoutError。
 */
class TimeoutError final : public std::exception {
public:
    /** @return 指向固定超时说明的指针，字符串在进程生命周期内有效。 */
    const char* what() const noexcept override;
};

namespace detail {

/** @brief 等待结束的原因。调用方据此决定读取结果、返回或抛出异常。 */
enum class WakeReason {
    PENDING,    ///< 尚未结束。
    READY,      ///< 任务已完成。
    CANCELLED,  ///< 任务被取消。
    TIMEOUT,    ///< 计时器超时。
    STOPPED     ///< 计时器关闭。
};

/**
 * @brief 表示一次等待的结束，可由结果发布方、计时器或取消回调通知。
 * @details 同一个事件只接受一次结束通知。通知可以先于 wait() 到达，
 *          后续调用 wait() 时仍能读到已保存的原因。
 */
class WaitEvent {
public:
    /** @brief 创建初始原因为 PENDING 的事件，等待者在调用 wait() 时才登记。 */
    WaitEvent();

    /**
     * @brief 提交一次唤醒通知。并发通知中只有首次写入会唤醒等待者。
     * @param reason 实际唤醒原因，须为 PENDING 以外的值。
     * @return 本次成功发布时返回 true，已有通知时返回 false。
     * @details 可从其他线程调用。已接受过通知时，本次调用直接返回。
     *          首次成功提交的线程还会执行仍然有效的订阅回调。
     */
    bool complete(WakeReason reason);

    /**
     * @brief 等到 complete() 接受一次通知并唤醒调用方。
     * @details 普通线程阻塞，调度协程挂起。需要响应当前任务的取消时，
     *          由 wait_interruptibly() 将该取消转成对本事件的通知。
     * @return 第一次成功提交给 complete() 的原因。
     */
    WakeReason wait() const;

    /**
     * @brief 直接读取当前原因，无需等待事件完成。
     * @return 已提交的原因，尚无通知时为 PENDING。
     * @note complete() 先写原因再通知，因此可能先读到原因，而等待者尚未被唤醒。
     */
    WakeReason reason() const noexcept;

    /**
     * @brief 取得本事件的 TaskHandle，用 subscribe() 注册事件完成后的操作。
     * @details 例如 TimeoutScope 在此注册回调，计时器调用 complete(TIMEOUT)
     *          后，该回调会向当前超时作用域请求取消。
     * @return 由本事件持有的 TaskHandle 引用。需要在事件销毁后访问时，应复制句柄。
     */
    const TaskHandle& signal() const noexcept;

private:
    /// 保存首次成功提交的原因，由 wait() 返回。
    std::atomic<WakeReason> _M_reason{WakeReason::PENDING};

    /// wait() 在此阻塞线程或挂起协程。complete() 写入原因后将此句柄标为完成，
    /// 使 wait() 可以返回，并执行通过 signal().subscribe() 注册的回调。
    TaskHandle _M_signal;
};

/**
 * @brief 记录一个任务或作用域的取消请求，以及检查点应抛出的异常。
 * @details 发出请求的线程通过 request() 发布原因。任务通过 requested() 或取消检查点观察，
 *          正在等待的任务可通过订阅接收唤醒。记录取消本身不会结束正在运行的用户函数。
 */
class CancellationState {
public:
    /// 创建尚未请求取消的状态。
    CancellationState();

    /**
     * @brief 发布取消原因并通知订阅者。
     * @param error 要在取消检查点重新抛出的非空异常，默认是 TaskCancelled。
     * @return 首次取得发布权时返回 true。后续请求返回 false。
     * @details 可从其他线程调用。首次发布会执行取消订阅，后续请求不会覆盖原异常。
     */
    bool request(std::exception_ptr error = std::make_exception_ptr(TaskCancelled{}));

    /**
     * @brief 查询取消原因是否已写入并对读取方可见。
     * @return 已发布时返回 true。发布方刚取得发布权但尚未写入异常时仍返回 false。
     */
    bool requested() const noexcept;

    /**
     * @brief 取得代表一次取消请求的 TaskHandle。
     * @details wait_interruptibly() 用 subscribe() 注册回调，以便 request() 发出
     *          取消时结束正在进行的等待。cancellation_point() 用 error() 读取
     *          并重新抛出 request() 保存的异常。
     * @return 由本取消状态持有的 TaskHandle 引用。需要独立保留时，应复制句柄。
     */
    const TaskHandle& signal() const noexcept;

private:
    /// request() 用此原子标志争夺唯一的发布权。置为 true 时，异常可能尚未写入。
    std::atomic<bool> _M_requested{false};

    /// request() 将取消异常写入此句柄并标记完成。requested() 据此判断异常是否已可读，
    /// 已登记的订阅回调则可结束各自正在进行的等待。
    TaskHandle _M_signal;
};

/**
 * @brief 决定当前代码调用 cancellation_point() 时应检查哪个取消状态。
 * @details 普通线程的绑定存入线程局部变量，协程的绑定存入协程对象。
 *          同一 worker 切换到其他协程时，各协程继续使用自己的绑定。
 * @note 作用域可嵌套，须按进入顺序的逆序销毁。
 */
class CancellationScope {
public:
    /**
     * @brief 在当前执行现场安装取消状态。
     * @param state 调用方持有的状态，须在作用域存续期间有效。
     */
    explicit CancellationScope(CancellationState& state) noexcept;

    /// 恢复进入时保存的绑定，应在创建它的执行现场析构。
    ~CancellationScope();

    /// 每个作用域单独保存并恢复绑定，禁止复制。
    CancellationScope(const CancellationScope&) = delete;

    /// 禁止通过赋值替换作用域绑定。
    CancellationScope& operator=(const CancellationScope&) = delete;

    /**
     * @brief 查找当前协程或普通线程正在使用的取消状态。
     * @return 非拥有指针，未绑定时为空。调用方应在相应绑定仍有效时使用它。
     */
    static CancellationState* current() noexcept;

private:
    /// 协程内指向当前协程。为空时绑定保存在当前线程的局部存储中。
    Coroutine* const _M_coroutine;

    /// 进入前的非拥有指针，析构时原样放回，原先未绑定时为空。
    CancellationState* const _M_previous;
};

/**
 * @brief 在嵌套超时作用域之间传递超时取消，同时标记哪个期限触发了它。
 * @details 只有触发该期限的作用域负责转换为 TimeoutError。内层作用域会继续传播外层原因。
 */
struct DeadlineCancelled {
    /// 发出超时取消的状态地址，仅用于比较归属。
    const CancellationState* owner;
};

/**
 * @brief 为 timeout() / timeout_at() 内执行的代码绑定独立的取消状态。
 * @details 父级发出取消时，转发原异常。本地计时器到期时，发布带有自身标记的取消异常。
 *          两种通知都调用同一个 CancellationState::request()，因此只保留第一次。
 * @note 退出时释放订阅并恢复父级绑定。期限通过协作式检查生效，代码在检查点响应。
 */
class TimeoutScope {
public:
    /**
     * @brief 安装内层取消状态，并建立父级取消与计时事件的回调。
     * @details 仅准备事件。调用方用 event() 将它交给计时服务并设置期限。
     */
    TimeoutScope();

    /**
     * @brief 取得应交给计时服务的事件。
     * @return 本作用域持有的事件指针引用。计时服务在到期或关闭时通知该事件。
     */
    const std::shared_ptr<WaitEvent>& event() const noexcept;

    /**
     * @brief 判断异常是否由本作用域的期限触发。
     * @param error 捕获到的内部超时取消异常。
     * @return 由本作用域触发时返回 true。
     */
    bool owns(const DeadlineCancelled& error) const noexcept;

private:
    /// 持有回调的强引用。销毁句柄后，发布方尚未取得的回调会被跳过。
    using Subscription = std::shared_ptr<const std::function<void()>>;

    /// 安装内层状态前取得的父级状态，用于在构造时复制父级的取消句柄。
    CancellationState* const _M_parent;

    /// 内层代码在检查点读取的状态。订阅回调也持有它，以便安全完成已开始的通知。
    const std::shared_ptr<CancellationState> _M_state;

    /// 将内层状态绑定到当前线程或协程，析构时恢复原绑定。
    const CancellationScope _M_scope;

    /// 计时服务到期或关闭时完成此事件，已登记的回调随后发布内层取消。
    const std::shared_ptr<WaitEvent> _M_event;

    /// 持有父级取消回调。父级完成发布时，回调复制原异常到内层状态。
    Subscription _M_parent_subscription;

    /// 持有计时事件回调。它将到期转成带归属的取消，将计时服务关闭转成普通取消。
    Subscription _M_timer_subscription;
};

/**
 * @brief 等待事件，并在当前绑定发布取消时尝试提前结束等待。
 * @param event 非空事件。结果发布方或计时器可向同一事件提交通知。
 * @return 事件保存的结束原因，例如 READY、TIMEOUT 或 STOPPED。
 * @details 如果当前绑定的取消通知最先到达，则重新抛出绑定保存的异常。
 *          未绑定取消状态时直接等待事件。退出时释放本次等待的取消订阅。
 */
WakeReason wait_interruptibly(const std::shared_ptr<WaitEvent>& event);

/**
 * @brief 等待目标就绪，当前调用方可在等待期间响应取消。
 * @param completion 目标的完成句柄。目标发布的异常留给读取结果时处理。
 * @details 先检查调用方是否已被取消。目标仍未就绪时，等待目标完成或调用方取消。
 *          此处只结束当前等待。向目标请求取消由 Task 等上层接口决定。
 */
void wait_for_completion(const TaskHandle& completion);

} // namespace detail

/**
 * @brief 查询当前协程或线程绑定的状态是否已发布取消异常。
 * @return 当前状态已发布取消时返回 true。未绑定或取消尚未发布时返回 false。
 */
bool cancellation_requested() noexcept;

/**
 * @brief 显式取消检查点。当前状态已发布取消时，重新抛出保存的原因。
 * @throws TaskCancelled 当前任务收到普通取消请求。
 * @note 期限触发的内部取消由所属超时作用域转换为 TimeoutError。
 */
void cancellation_point();

} // namespace ioman
