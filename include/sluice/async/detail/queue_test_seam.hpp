// sluice::async::detail — internal-testing deterministic pause seam for the
// QueuePort snapshot projections (Queue snapshot lifecycle compliance).
//
// LAYERING (mirrors mutex_test_seam.hpp; AGENTS.md §3.9):
//
//   production snapshot bodies   (src/async/queue_port.cpp)
//       |
//       v   detail::maybe_pause_queue_snapshot()   [under the macro]
//   internal-testing seam state (src/async/queue_test_seam.cpp)
//       ^
//       |   detail::test_hooks::*                  [test authority]
//   tests/async_queue_lifecycle_death_test.cpp
//
// `queue_port.cpp` depends only on `sluice::async::detail`, never on the test
// controller. The seam state lives inside `sluice_async_internal_testing`:
// src/async/queue_test_seam.cpp compiles to an EMPTY translation unit under
// the production `sluice_async` target, so the production archive carries NO
// pause symbol and the production queue_port.cpp contains NO pause call
// (verified by nm in the evidence report).
//
// The pause protocol is the ordering proof, not a timing heuristic: the
// snapshot publishes `paused` AFTER its G+S lifecycle admission (so
// active_port_calls_ == 1 is already committed and visible under G+S), then
// spins until `resume`. A test thread that waits on `paused` before calling
// begin_teardown() therefore provably runs after the snapshot is inside the
// ordinary-call interval. No sleeps; the gate is process-global and arms a
// single death-test child at a time.
#pragma once

#include <atomic>

namespace sluice::async::detail {

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

// Test-side pause gate for one paused snapshot call.
struct QueueSnapshotPauseGate final {
    std::atomic<bool> paused{false};  // published after lifecycle admission
    std::atomic<bool> resume{false};  // set by the test to release the snapshot
};

// Called by every snapshot body between lifecycle admission and projection.
// No-op when no gate is armed. Defined in src/async/queue_test_seam.cpp.
void maybe_pause_queue_snapshot() noexcept;

// Test-authority hooks used by the Queue lifecycle death tests. They live in
// sluice::async::detail (not sluice_async_test) because they mutate the
// library-internal seam state; the death-test cases are thin facades over
// them.
namespace test_hooks {

// Arm `gate` for the next snapshot that runs. Disarms any previously armed
// gate (a fresh child arms once, so overlap is a test bug by construction).
void arm_queue_snapshot_pause(QueueSnapshotPauseGate& gate) noexcept;

// Block until the armed snapshot has published `paused` — i.e. it has
// completed its G+S lifecycle admission and holds active_port_calls_ == 1.
void queue_snapshot_wait_paused(const QueueSnapshotPauseGate& gate) noexcept;

// Release the paused snapshot (it resumes and retires its CallGuard).
void release_queue_snapshot_pause(QueueSnapshotPauseGate& gate) noexcept;

// Disarm the gate. No-op unless a gate is armed.
void disarm_queue_snapshot_pause() noexcept;

}  // namespace test_hooks

#endif  // SLUICE_ASYNC_INTERNAL_TESTING

}  // namespace sluice::async::detail
