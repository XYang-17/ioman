#include "sync/wait_group.h"

#include "promise.h"

#include <limits>
#include <stdexcept>

namespace ioman {

struct WaitGroup::Round {
    Promise<void> completion; ///< 本轮完成状态的写入端，由将计数减为零的调用方发布完成通知。
    const Future<void> finished = completion.get_future(); ///< 本轮完成状态的读取端，供本轮等待者共同等待。
};

void WaitGroup::add(const int n) {
    std::shared_ptr<Round> finished;
    {
        const std::lock_guard<std::mutex> lock(_M_mutex);
        if (n < -_M_count) {
            throw std::logic_error("WaitGroup counter cannot be negative");
        }
        if (std::numeric_limits<int>::max() - _M_count < n) {
            throw std::overflow_error("WaitGroup counter overflow");
        }

        const int next = _M_count + n;
        if (0 == _M_count && 0 < next) {
            _M_round = std::make_shared<Round>();
        } else if (0 == next) {
            finished = std::move(_M_round);
        }
        _M_count = next;
    }
    // 锁外发布旧轮次的完成通知。此时其他调用方已经可以登记新一轮工作。
    if (nullptr != finished) {
        finished->completion.set_value();
    }
}

void WaitGroup::done() {
    add(-1);
}

void WaitGroup::wait() const {
    cancellation_point();
    std::shared_ptr<Round> round;
    {
        const std::lock_guard<std::mutex> lock(_M_mutex);
        // 持有当前轮次的引用，释放内部锁后仍能等待它归零。
        round = _M_round;
    }
    if (nullptr != round) {
        round->finished.wait();
    }
}

} // namespace ioman
