// event_authority_probe — NEGATIVE compile probe (E12-A-EVENT-CORRECTIVE-1,
// Corrective A1 / T24).
//
// This file MUST NOT COMPILE. It is the mechanical gate that proves ordinary
// production code CANNOT obtain an Event's underlying WaitQueue and therefore
// CANNOT call Scheduler::wake_wait_one on an UNSET Event to synthesize a
// RESOURCE_WAKE (F-EVENT-AUTH).
//
// The verify gate compiles this file expecting FAILURE. If the bypass ever
// compiles, the Event public authority has regressed and the corrective has
// failed.
//
// It deliberately includes ONLY public production headers and uses fully-
// qualified names (no test TU friend struct). The friend struct
// EventHooks is defined ONLY in the e12_event test TU; an ordinary
// production TU cannot name it.
#include <sluice/async/event.hpp>
#include <sluice/async/scheduler.hpp>

// Attempt the forbidden bypass. This must fail to compile because Event has NO
// public wait_queue() accessor (it is private, reachable only via the test-only
// friend struct EventHooks defined in the e12 test TU).
void bypass_event_authority(sluice::async::Event& event,
                            sluice::async::Scheduler& scheduler) {
    // F-EVENT-AUTH: this would resolve an Event waiter as Woken while the Event
    // remains UNSET and set() was never called. It MUST NOT compile.
    scheduler.wake_wait_one(event.wait_queue());  // expected: no member wait_queue
}
