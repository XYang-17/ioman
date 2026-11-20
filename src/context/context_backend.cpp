#include "context/context_backend.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <system_error>
#include <ucontext.h>

namespace ioman::detail {

struct ContextBackend::State {
    /// 平台原生执行现场，包含寄存器、信号掩码及栈配置。
    ucontext_t native{};

    /// 新上下文首次获得执行权时调用的入口。
    Entry entry = nullptr;

    /// 透传给入口的参数，其生命周期由调用方管理。
    void* argument = nullptr;

    /**
     * @brief 将 makecontext 传入的两个整数还原为 State 地址，再调用实际入口。
     * @param low 地址的低 32 位。
     * @param high 地址的高 32 位。
     */
    static void trampoline(unsigned int low, unsigned int high) noexcept {
        // 先在 64 位整数中拼接，再转换为目标平台的指针宽度。
        const auto address = (std::uint64_t{high} << 32) | std::uint64_t{low};
        const auto pointer_value = static_cast<std::uintptr_t>(address);
        const auto* const state = reinterpret_cast<const State*>(pointer_value);
        state->entry(state->argument);
        // 入口负责在结束时恢复调用者。返回到此处表示入口违反切换约定。
        std::terminate();
    }
};

// 将原生执行现场放在地址稳定的存储中，供首次切入时的入口函数使用。
ContextBackend::ContextBackend() : _M_state(std::make_unique<State>()) {}

ContextBackend::~ContextBackend() = default;

void ContextBackend::initialize(
    std::byte* const stack,
    const std::size_t stack_size,
    const Entry entry,
    void* const argument
) {
    auto& state = *_M_state;
    // 先获取初始执行现场，再配置新上下文的栈和入口。
    if (-1 == getcontext(&state.native)) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "getcontext failed");
    }

    state.native.uc_stack.ss_sp = stack;
    state.native.uc_stack.ss_size = stack_size;
    state.native.uc_stack.ss_flags = 0;
    // 由 Context 在入口结束时显式恢复最近一次切入它的调用者。
    state.native.uc_link = nullptr;
    state.entry = entry;
    state.argument = argument;

    // makecontext 按整型参数传递入口信息，将状态地址拆成两个 32 位片段。
    static_assert(sizeof(std::uintptr_t) <= sizeof(std::uint64_t));
    static_assert(sizeof(unsigned int) == sizeof(std::uint32_t));
    const auto pointer_value = reinterpret_cast<std::uintptr_t>(&state);
    const auto address = static_cast<std::uint64_t>(pointer_value);
    const auto low = static_cast<unsigned int>(address);
    const auto high = static_cast<unsigned int>(address >> 32);
    const auto trampoline = reinterpret_cast<void (*)()>(&State::trampoline);
    makecontext(&state.native, trampoline, 2, low, high);
}

void ContextBackend::swap_to(const ContextBackend& target) {
    // 保存当前现场后切入目标。当前现场再次被恢复时，从此调用继续。
    if (-1 == swapcontext(&_M_state->native, &target._M_state->native)) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "swapcontext failed");
    }
}

void ContextBackend::resume() const noexcept {
    // 直接恢复目标现场，无需保留已经结束的入口现场。
    setcontext(&_M_state->native);
    // 成功时 setcontext 不会返回。返回表示无法完成切换。
    std::terminate();
}

} // namespace ioman::detail
