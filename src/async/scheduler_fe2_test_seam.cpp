// src/async/scheduler_fe2_test_seam.cpp
//
// Out-of-line definitions of the FE-2 minimal stackless frontend seams
// declared in Scheduler::AsyncTestAccess (scheduler_test_access.hpp).
//
// The ENTIRE translation unit is gated on SLUICE_ASYNC_INTERNAL_TESTING:
// under the production `sluice_async` target (the macro is undefined) this
// file compiles to an empty TU, so the production archive carries NO FE-2
// seam symbol. Only the `sluice_async_internal_testing` variant compiles
// the real bodies.
//
// These definitions live OUT-OF-LINE (C4 / issue #135) because they touch
// Event's private state through the Scheduler friendship and therefore need
// the COMPLETE Event type; the installed scheduler.hpp must not gain that
// include footprint, so the seam header only declares.
//
// Semantic authority note: every body below runs PRODUCTION seams — the
// shared Event admission ladder (Scheduler::event_wait_admit_locked), the
// production Event cancellation seam (event_cancel_wait), and the
// deferred/take delivery split. This TU adds NO authority of its own.
#include <sluice/async/scheduler.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/event.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_test_access.hpp"

namespace sluice::async {

bool Scheduler::AsyncTestAccess::event_wait_deferred_for_test(
    Scheduler& s, Event& event, WaitNode& node, FeDeferredRecord& record) {
    LockGuard lk(s.global_mtx_);
    LockGuard qlk(event.waiters_.mtx());
    if (s.event_wait_admit_locked(event.waiters_, event.set_, node,
                                  WaitResume::deferred(&record),
                                  /*timed=*/false,
                                  deadline_t{}) !=
        Scheduler::EventAdmitDisposition::authorized) {
        return false;
    }
    record.arm();
    return true;
}

bool Scheduler::AsyncTestAccess::event_wait_deferred_deadline_for_test(
    Scheduler& s, Event& event, WaitNode& node, FeDeferredRecord& record,
    deadline_t deadline) {
    LockGuard lk(s.global_mtx_);
    LockGuard qlk(event.waiters_.mtx());
    if (s.event_wait_admit_locked(event.waiters_, event.set_, node,
                                  WaitResume::deferred(&record),
                                  /*timed=*/true, deadline) !=
        Scheduler::EventAdmitDisposition::authorized) {
        return false;
    }
    record.arm();
    return true;
}

bool Scheduler::AsyncTestAccess::event_cancel_deferred_for_test(
    Scheduler& s, Event& event, WaitNode& node) {
    return s.event_cancel_wait(event.waiters_, node);
}

}  // namespace sluice::async

#endif  // SLUICE_ASYNC_INTERNAL_TESTING
