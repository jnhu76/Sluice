// src/async/queue_test_seam.cpp
//
// Implementation of the internal-testing deterministic pause seam for the
// QueuePort snapshot projections (Queue snapshot lifecycle compliance). The
// ENTIRE translation unit is gated on SLUICE_ASYNC_INTERNAL_TESTING: under the
// production `sluice_async` target (the macro is undefined) this file compiles
// to an empty TU, so the production archive carries NO pause symbol. Only the
// `sluice_async_internal_testing` variant (which defines the macro, PUBLIC)
// compiles the real seam.
//
// The gate is a process-global pointer; the pointer and the gate's
// paused/resume flags use acquire/release. This is correct because (a) the
// gate exists ONLY to pause a single death-test child's snapshot at a
// documented boundary, and (b) the pause carries NO production
// synchronization meaning beyond the documented protocol: the snapshot
// publishes `paused` AFTER its G+S lifecycle admission (active_port_calls_
// already incremented under G+S), so a test thread that observes `paused`
// before calling begin_teardown() provably runs after admission. The spin on
// `resume` is a test wait, not production ordering proof.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/detail/queue_test_seam.hpp>

#include <atomic>
#include <thread>

namespace sluice::async::detail {

namespace {

// The single armed gate. nullptr = no pause. Release store on arm (publish
// the gate to concurrent snapshot calls), acquire load in the seam (observe
// an armed gate).
std::atomic<QueueSnapshotPauseGate*> g_queue_snapshot_pause_gate{nullptr};

}  // namespace

void maybe_pause_queue_snapshot() noexcept {
    auto* gate = g_queue_snapshot_pause_gate.load(std::memory_order_acquire);
    if (gate == nullptr) {
        return;
    }
    // The snapshot has completed its G+S lifecycle admission
    // (active_port_calls_ incremented under G+S); publish that it is inside
    // the ordinary-call interval, then wait for the test to release it.
    gate->paused.store(true, std::memory_order_release);
    while (!gate->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

namespace test_hooks {

void arm_queue_snapshot_pause(QueueSnapshotPauseGate& gate) noexcept {
    gate.paused.store(false, std::memory_order_relaxed);
    gate.resume.store(false, std::memory_order_relaxed);
    g_queue_snapshot_pause_gate.store(&gate, std::memory_order_release);
}

void queue_snapshot_wait_paused(const QueueSnapshotPauseGate& gate) noexcept {
    while (!gate.paused.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void release_queue_snapshot_pause(QueueSnapshotPauseGate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
}

void disarm_queue_snapshot_pause() noexcept {
    g_queue_snapshot_pause_gate.store(nullptr, std::memory_order_release);
}

}  // namespace test_hooks

}  // namespace sluice::async::detail

#endif  // SLUICE_ASYNC_INTERNAL_TESTING
