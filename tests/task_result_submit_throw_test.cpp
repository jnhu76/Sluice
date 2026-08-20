// task_result_submit_throw_test — P1 regression (#135 C7, review issue #150):
// run_task_to_result's submit() leg must be a TYPED exception boundary.
//
// Defect being pinned: ApplicationRuntime::submit() rolls back its admission
// reservation and RETHROWS what Group::async_evented threw (P2-02 — e.g.
// std::bad_alloc from a bookkeeping reserve). The Runtime is already Running
// at that point; before the fix, the escaping exception unwound into
// ~ApplicationRuntime in a non-quiescent state, which fail-fasts
// (group_lifetime_fail_fast) — an allocation failure became PROCESS
// TERMINATION instead of the typed no_space translation.
//
// Determinism (no allocation pressure, no timing): the internal-testing
// injection seam arms the runtime's ROOT group admission (the existing P2-01
// Group fail-point) so the next submit() deterministically throws bad_alloc
// at a reserve boundary. One-shot; compiled out of production entirely
// (this binary links sluice_async_internal_testing).
//
// Before the fix, the pinned case terminates the process (group_lifetime_
// fail_fast during unwind); after the fix it returns the typed result and
// the process continues — the control case proves the seam is inert when
// not armed.
#include "harness.hpp"

#include <sluice/async/fake_backend.hpp>
#include <sluice/async/task_result.hpp>

#include <memory>
#include <utility>

namespace {

namespace sa = sluice::async;

SLUICE_TEST_CASE(submit_time_throw_returns_typed_no_space_not_termination) {
    // Arm the one-shot injection: the NEXT run_task_to_result's submit()
    // takes the rollback-and-rethrow path.
    sa::detail::task_result_submit_throw_armed = true;

    bool task_body_ran = false;
    auto r = sa::run_task_to_result<int>(
        1u, std::make_unique<sa::FakeAsyncBackend>(),
        [&task_body_ran](sa::RuntimeTaskContext&,
                         sa::TaskResultSlot<sluice::Result<int>>& slot) {
            task_body_ran = true;
            slot.publish(sluice::Result<int>{42});
        });

    // The exception was netted into the typed translation: bad_alloc ->
    // no_space (translate_task_exception), NOT process termination.
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::no_space);
    // The task was never admitted, so its body never ran and never
    // published.
    SLUICE_CHECK(!task_body_ran);
    // One-shot: the flag was consumed by the bridge call itself.
    SLUICE_CHECK(!sa::detail::task_result_submit_throw_armed);
}

SLUICE_TEST_CASE(submit_no_injection_returns_task_result_control) {
    // Control: the identical call without the injection runs the task to
    // completion and returns its published result verbatim (the seam is
    // inert when disarmed).
    auto r = sa::run_task_to_result<int>(
        1u, std::make_unique<sa::FakeAsyncBackend>(),
        [](sa::RuntimeTaskContext&, sa::TaskResultSlot<sluice::Result<int>>& slot) {
            slot.publish(sluice::Result<int>{7});
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 7);
}

}  // namespace

SLUICE_MAIN()
