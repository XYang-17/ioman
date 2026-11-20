#include <test_support.h>
#include <ioman/ioman.h>
#include <ioman/sync/mutex.h>
#include <ioman/sync/condition_variable.h>
#include <ioman/sync/wait_group.h>

#include <atomic>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

namespace {
using testing::check;
using testing::throws;
using namespace std::chrono_literals;

void empty_and_duplicates() {
    ioman::CompletionStream<int> empty({});
    check(!empty.next(), "empty stream");
    ioman::Promise<int> producer;
    auto future = producer.get_future();
    auto stream = ioman::as_completed(std::vector{future, future});
    producer.set_value(4);
    auto first = stream.next();
    auto second = stream.next();
    check(first && second && first->index == 0 && second->index == 1 && !stream.next(), "duplicate positions");
    check(first->future.get() == 4 && second->future.get() == 4, "duplicate values");
}

void completion_order_and_failure() {
    ioman::Promise<int> first, second;
    auto stream = ioman::as_completed(std::vector{first.get_future(), second.get_future()});
    second.set_exception(std::make_exception_ptr(std::runtime_error("failed")));
    first.set_value(9);
    auto first_completed = stream.next();
    auto second_completed = stream.next();
    check(first_completed && second_completed && first_completed->index == 1 &&
        second_completed->index == 0, "completion order");
    throws<std::runtime_error>([&] { first_completed->future.get(); });
    check(second_completed->future.get() == 9 && !stream.next(), "remaining result");
}

void combinator_edges() {
    ioman::ioman loop;
    check(ioman::gather(std::vector<ioman::Future<int>>{}).get().empty(), "empty gather");
    ioman::gather(std::vector<ioman::Future<void>>{}).get();
    check(loop.wait(std::vector<ioman::Future<int>>{}).done.empty(), "empty wait");
    ioman::Promise<int> first, second;
    const std::vector inputs{first.get_future(), second.get_future()};
    second.set_value(2);
    auto snapshot = loop.wait(inputs, ioman::WaitMode::FIRST_COMPLETED);
    check(snapshot.done == std::vector<std::size_t>{1} && snapshot.pending == std::vector<std::size_t>{0}, "snapshot");
    snapshot = loop.wait(inputs, ioman::WaitMode::ALL_COMPLETED, ioman::ioman::Clock::now());
    check(snapshot.pending.size() == 1, "deadline snapshot");
    first.set_exception(std::make_exception_ptr(std::domain_error("bad")));
    check(loop.wait(inputs, ioman::WaitMode::FIRST_EXCEPTION).done.size() == 2, "exception mode");
    throws<std::domain_error>([&] { ioman::gather(inputs).get(); });
    auto settled = ioman::gather_settled(inputs).get();
    check(settled[0].exception() && settled[1].get() == 2, "settled order");
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"empty_and_duplicates", empty_and_duplicates},
        {"completion_order_and_failure", completion_order_and_failure},
        {"combinator_edges", combinator_edges},
    });
}
