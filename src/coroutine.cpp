#include "coroutine.h"

#include <stdexcept>
#include <utility>

namespace ioman {

thread_local Coroutine* Coroutine::_M_tls_current_coroutine = nullptr;


Coroutine::Coroutine(Entry entry, std::size_t stack_size)
    : _M_entry(std::move(entry)) {
    if (nullptr == _M_entry) {
        throw std::invalid_argument(
            "Coroutine entry function cannot be empty"
        );
    }

    // 创建底层执行上下文。
    // 此时不会执行入口函数，只有首次 resume() 才会执行。
    _M_context = Context::create(
        [this]() {
            run();
        },
        stack_size
    );
}


Coroutine::~Coroutine() = default;


void Coroutine::resume() {
    if (CoroutineState::READY != _M_state &&
        CoroutineState::SUSPENDED != _M_state) {
        throw std::logic_error(
            "Coroutine cannot be resumed in its current state"
        );
    }

    const CoroutineState previous_state = _M_state;
    Coroutine* previous_coroutine = _M_tls_current_coroutine;

    // 记录当前恢复协程的执行上下文。
    _M_resumer_context = Context::current();

    // 设置当前协程状态。
    _M_state = CoroutineState::RUNNING;
    _M_tls_current_coroutine = this;

    try {
        Context::swap(
            _M_resumer_context,
            _M_context.get()
        );
    } catch (...) {
        // 底层切换失败时恢复原状态。
        _M_state = previous_state;
        _M_tls_current_coroutine = previous_coroutine;
        throw;
    }

    // 协程已经挂起或执行结束，恢复调用者的协程标记。
    _M_tls_current_coroutine = previous_coroutine;

    if (_M_context->finished()) {
        _M_state = CoroutineState::DEAD;
    }

    // 入口函数中的异常在协程执行结束后传播给调用者。
    if (CoroutineState::DEAD == _M_state &&
        nullptr != _M_exception) {
        std::rethrow_exception(_M_exception);
    }
}


void Coroutine::yield() {
    if (this != _M_tls_current_coroutine ||
        CoroutineState::RUNNING != _M_state) {
        throw std::logic_error(
            "Only the running coroutine can yield"
        );
    }

    if (nullptr == _M_resumer_context) {
        throw std::logic_error(
            "Coroutine has no resumer context"
        );
    }

    // 保存挂起状态。
    _M_state = CoroutineState::SUSPENDED;

    try {
        // 切回最近一次恢复当前协程的上下文。
        Context::swap(
            Context::current(),
            _M_resumer_context
        );
    } catch (...) {
        _M_state = CoroutineState::RUNNING;
        throw;
    }

    // 再次恢复后，从这里继续执行。
    _M_state = CoroutineState::RUNNING;
}


void Coroutine::run() {
    try {
        _M_entry();
    } catch (...) {
        _M_exception = std::current_exception();
    }
}

} // namespace ioman
