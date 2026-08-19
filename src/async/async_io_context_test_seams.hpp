// async_io_context_test_seams.hpp - NON-INSTALLED internal-testing seam
// header for AsyncIoContext (C4 / issue #135: the internal-testing control
// plane must not shape the installed production header).
//
// Contains, under SLUICE_ASYNC_INTERNAL_TESTING only, the out-of-line
// definitions of the deterministic context-level wait-source progress pause
// gate and the guarded seam bodies. The installed
// <sluice/async/async_io_context.hpp> keeps only the declarations plus the
// layout-bearing test member, and includes this header at its bottom under
// the same guard; production TUs (macro undefined) compile none of it.
#pragma once

#include <sluice/async/async_io_context.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>

namespace sluice::async {

// Deterministic context-level pause (D4-RM13 detector seam): the
// inter-iteration control-wake detector
// (ctx_wait_one_inter_iteration_control_wake_not_lost in
// async_io_context_split_wait_c2e_test) pauses wait_one AFTER
// wait_for_change reports `progress` and BEFORE the next internal
// snapshot — the exact inter-iteration window where a control wake used
// to be absorbed into a fresh snapshot (rebaselined away), drained, and
// reparked forever. Compiled out of production builds; the layout cost in
// the internal-testing target is accepted and documented (AGENTS.md §15).
struct AsyncIoContext::WaitSourceProgressPauseGate {
    std::atomic<bool> paused{false};  // reached the pause point
    std::atomic<bool> exited{false};  // pause exited (for RAII release)
    // `resume` is private on purpose: the paused thread blocks in
    // resume.wait(false), and ONLY a notifying atomic operation
    // (notify_one/notify_all) wakes an atomic::wait consumer — a plain
    // store of the value does not wake a consumer that is already parked
    // (store-racing-the-park is exactly the lost-wake this gate exists
    // to eliminate). Making the field private forces every publisher
    // through resume_wait_source_progress_gate_for_test below, so the
    // required store+notify pair cannot be forgotten at a call site
    // (issue #92 resume_threadpool_gate model). AsyncIoContext is the
    // friend performing the blocking wait (pause_after_wait_source_
    // progress_).
  private:
    friend class AsyncIoContext;
    std::atomic<bool> resume{false};  // released only via the helper below
};

inline void AsyncIoContext::set_wait_source_progress_pause_gate_for_test(
    WaitSourceProgressPauseGate* gate) noexcept {
    wait_source_progress_gate_.store(gate, std::memory_order_release);
}

// The ONLY supported resume publisher (the structural rule above):
// release-store then notify_all. notify_all is a harmless no-op when no
// thread is in atomic::wait and does not rest on a single-waiter
// assumption.
inline void AsyncIoContext::resume_wait_source_progress_gate_for_test(
    WaitSourceProgressPauseGate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

}  // namespace sluice::async

#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
