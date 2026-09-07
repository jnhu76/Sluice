#include <sluice/async/wait_policy.hpp>

namespace sluice::async {

WaitPolicy& default_wait_policy() noexcept {
    static ThreadedWaitPolicy* p = new ThreadedWaitPolicy();
    return *p;
}

} // namespace sluice::async
