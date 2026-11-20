#include "context/context.h"
#include "context/context_backend.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace ioman {

struct Context::Impl {
    /// 协程独立执行栈。平台后端销毁前，栈内存始终有效。
    std::unique_ptr<std::byte[]> stack;

    /// 平台后端持有本上下文的寄存器现场。
    detail::ContextBackend backend;

    /// 首次切入此上下文时执行的函数。
    Entry entry;

    /// 最近一次切入此上下文的调用者。
    Context* caller = nullptr;

    /// 入口函数是否已经结束。
    bool finished = false;
};

/// 每个线程独立记录当前上下文。
static thread_local Context* tls_current_context = nullptr;

Context::Context(std::unique_ptr<Impl> impl)
    : _M_impl(std::move(impl)) {}

Context::~Context() = default;

std::unique_ptr<Context> Context::create(
    Entry entry,
    const std::size_t stack_size
) {
    if (nullptr == entry) {
        throw std::invalid_argument("Context entry function cannot be empty");
    }

    if (0 == stack_size) {
        throw std::invalid_argument("Context stack size cannot be zero");
    }

    auto impl = std::make_unique<Impl>();
    impl->entry = std::move(entry);
    impl->stack = std::make_unique<std::byte[]>(stack_size);

    auto context = std::unique_ptr<Context>(new Context(std::move(impl)));
    auto& state = *context->_M_impl;
    state.backend.initialize(state.stack.get(), stack_size, &_M_entry_trampoline, context.get());
    return context;
}

void Context::swap(Context* const from, Context* const to) {
    if (nullptr == from || nullptr == to) {
        throw std::invalid_argument("Context cannot be null");
    }

    if (from == to) {
        throw std::invalid_argument("Cannot switch to the same context");
    }

    if (current() != from) {
        throw std::logic_error("from is not the current context");
    }

    if (to->_M_impl->finished) {
        throw std::logic_error("Cannot resume a finished context");
    }

    // 入口函数结束时，恢复最近一次切入它的调用者。
    Context* const previous_caller = to->_M_impl->caller;
    to->_M_impl->caller = from;
    tls_current_context = to;

    try {
        from->_M_impl->backend.swap_to(to->_M_impl->backend);
    } catch (...) {
        // 切换失败，恢复调用者及当前上下文标记。
        to->_M_impl->caller = previous_caller;
        tls_current_context = from;
        throw;
    }

    tls_current_context = from;
}

Context* Context::current() {
    if (nullptr != tls_current_context) {
        return tls_current_context;
    }

    // 主上下文使用现有的线程栈，首次切出时由后端保存执行现场。
    static thread_local std::unique_ptr<Context> main_context;
    if (nullptr == main_context) {
        main_context.reset(new Context(std::make_unique<Impl>()));
        tls_current_context = main_context.get();
    }

    return tls_current_context;
}

bool Context::finished() const noexcept {
    return _M_impl->finished;
}

void Context::_M_entry_trampoline(void* const argument) noexcept {
    auto* const self = static_cast<Context*>(argument);
    if (nullptr == self || nullptr == self->_M_impl) {
        std::terminate();
    }

    try {
        self->_M_impl->entry();
    } catch (...) {
        // 上层 Coroutine 负责保存任务异常，异常不能穿过平台入口边界。
        std::terminate();
    }

    self->_M_impl->finished = true;
    Context* const caller = self->_M_impl->caller;
    if (nullptr == caller) {
        std::terminate();
    }

    tls_current_context = caller;
    caller->_M_impl->backend.resume();
}

} // namespace ioman
