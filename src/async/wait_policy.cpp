// Implementation of the Threaded default WaitPolicy (sluice-CORE-E0A).
#include <sluice/async/wait_policy.hpp>

namespace sluice::async {

WaitPolicy& default_wait_policy() noexcept {
    // Function-local static: constructed on first use, never destroyed during
    // the program (leaked on exit, which is fine for a stateless policy). This
    // avoids the static-destruction-order fiasco a Future might hit at shutdown.
    static ThreadedWaitPolicy* p = new ThreadedWaitPolicy();
    return *p;
}

}  // namespace sluice::async
