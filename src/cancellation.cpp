#include "cancellation.h"

namespace ioman {
namespace {

// 普通线程当前使用的取消状态，由 CancellationScope 安装和恢复。
// 指针由作用域外的任务或超时状态持有。协程通过自身字段保存同类绑定。
thread_local detail::CancellationState* thread_cancellation = nullptr;

} // namespace

const char* TaskCancelled::what() const noexcept {
    return "ioman task cancelled";
}

const char* TimeoutError::what() const noexcept {
    return "ioman wait timed out";
}

namespace detail {

WaitEvent::WaitEvent() : _M_signal(nullptr) {}

bool WaitEvent::complete(const WakeReason reason) {
    // expected 是原子比较的前置状态。只有事件仍为 PENDING 时，当前线程才负责通知。
    auto expected = WakeReason::PENDING;
    if (_M_reason.compare_exchange_strong(expected, reason)) {
        // 等待原因已经写入 _M_reason。此处用 TaskHandle 唤醒，无需往其中写入异常。
        _M_signal._L_finish(nullptr);
        return true;
    }
    return false;
}

WakeReason WaitEvent::wait() const {
    _M_signal.wait();
    return reason();
}

WakeReason WaitEvent::reason() const noexcept {
    return _M_reason.load();
}

const TaskHandle& WaitEvent::signal() const noexcept {
    return _M_signal;
}

CancellationState::CancellationState() : _M_signal(nullptr) {}

bool CancellationState::request(std::exception_ptr error) {
    // 取得发布权的线程负责同时保存异常和通知订阅者。
    if (_M_requested.exchange(true)) {
        return false;
    }
    _M_signal._L_finish(std::move(error));
    return true;
}

bool CancellationState::requested() const noexcept {
    // 抢占发布权与写入原因之间有间隔，检查点以完成信号为准。
    return _M_signal.done();
}

const TaskHandle& CancellationState::signal() const noexcept {
    return _M_signal;
}

CancellationScope::CancellationScope(CancellationState& state) noexcept
    : _M_coroutine(Coroutine::current()), _M_previous(current()) {
    // 在替换绑定前保存原指针。协程恢复时仍从自己的对象读取绑定。
    if (nullptr == _M_coroutine) {
        thread_cancellation = &state;
    } else {
        _M_coroutine->_M_cancellation = &state;
    }
}

CancellationScope::~CancellationScope() {
    // 归还进入作用域时使用的槽位，恢复父级状态或空指针。
    if (nullptr == _M_coroutine) {
        thread_cancellation = _M_previous;
    } else {
        _M_coroutine->_M_cancellation = _M_previous;
    }
}

CancellationState* CancellationScope::current() noexcept {
    const Coroutine* const coroutine = Coroutine::current();
    return nullptr == coroutine ? thread_cancellation : coroutine->_M_cancellation;
}

TimeoutScope::TimeoutScope()
    : _M_parent(CancellationScope::current()),
      _M_state(std::make_shared<CancellationState>()),
      _M_scope(*_M_state),
      _M_event(std::make_shared<WaitEvent>()) {
    if (nullptr != _M_parent) {
        // 回调持有父级句柄副本。即使通知与作用域退出交错，也能读取发布的异常。
        const auto parent = _M_parent->signal();
        _M_parent_subscription = parent.subscribe([parent, state = _M_state] {
            // 保留异常对象中的来源，让触发期限的外层作用域负责转换异常。
            state->request(parent.error());
        });
    }

    // 回调用弱引用获取事件，作用域已经释放事件时可以跳过通知。
    _M_timer_subscription = _M_event->signal().subscribe(
        [event = std::weak_ptr<WaitEvent>(_M_event), state = _M_state] {
            const auto timer = event.lock();
            if (nullptr != timer) {
                if (WakeReason::TIMEOUT == timer->reason()) {
                    // 保存发出该超时的状态地址，供 timeout_at() 识别由谁触发。
                    const auto error = std::make_exception_ptr(DeadlineCancelled{state.get()});
                    state->request(error);
                } else {
                    // 计时服务关闭使用普通取消，区别于本地期限到达。
                    state->request();
                }
            }
        }
    );
}

const std::shared_ptr<WaitEvent>& TimeoutScope::event() const noexcept {
    return _M_event;
}

bool TimeoutScope::owns(const DeadlineCancelled& error) const noexcept {
    return _M_state.get() == error.owner;
}

WakeReason wait_interruptibly(const std::shared_ptr<WaitEvent>& event) {
    const auto* const cancellation = CancellationScope::current();
    if (nullptr == cancellation) {
        return event->wait();
    }

    // 保留订阅句柄到等待返回。收到取消时，尝试以 CANCELLED 完成本次等待事件。
    const auto subscription = cancellation->signal().subscribe([event] {
        event->complete(WakeReason::CANCELLED);
    });
    const auto reason = event->wait();
    if (WakeReason::CANCELLED == reason) {
        // 事件只保存分类。原异常从当前取消状态读取，可能包含超时作用域的归属。
        cancellation_point();
    }
    return reason;
}

void wait_for_completion(const TaskHandle& completion) {
    if (nullptr == CancellationScope::current()) {
        // 没有可取消状态时直接等待，省去事件和订阅。
        completion.wait_ready();
        return;
    }

    cancellation_point();
    if (!completion.done()) {
        // 目标完成与调用方取消共用这个事件，任一方先通知即可结束此次等待。
        const auto event = std::make_shared<WaitEvent>();
        const auto subscription = completion.subscribe([event] {
            event->complete(WakeReason::READY);
        });
        if (WakeReason::CANCELLED == wait_interruptibly(event)) {
            throw TaskCancelled{};
        }
    }
    // 这里只确认完成状态，结果异常留给后续读取操作处理。
    completion.wait_ready();
}

} // namespace detail

bool cancellation_requested() noexcept {
    const auto* const cancellation = detail::CancellationScope::current();
    return nullptr != cancellation && cancellation->requested();
}

void cancellation_point() {
    const auto* const cancellation = detail::CancellationScope::current();
    if (nullptr != cancellation && cancellation->requested()) {
        const auto error = cancellation->signal().error();
        std::rethrow_exception(error);
    }
}

} // namespace ioman
