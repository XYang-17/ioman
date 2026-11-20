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

void outside_scope() {
    check(!ioman::cancellation_requested(), "unexpected scope");
    ioman::cancellation_point();
}

void nested_deadline_and_recovery() {
    ioman::ioman loop;
    loop.submit([] {
        throws<ioman::TimeoutError>([] {
            ioman::timeout(1s, [] { ioman::timeout(0ms, [] { ioman::cancellation_point(); }); });
        });
        check(!ioman::cancellation_requested(), "scope leaked");
    }).get();
    check(loop.submit([] { ioman::cancellation_point();
        return 8; }).get() == 8, "next task polluted");
}

} // namespace

int main() {
    return testing::run(__FILE__, {
        {"outside_scope", outside_scope},
        {"nested_deadline_and_recovery", nested_deadline_and_recovery},
    });
}
