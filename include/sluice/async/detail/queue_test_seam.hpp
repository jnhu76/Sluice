#pragma once

#include <atomic>

namespace sluice::async::detail {

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

struct QueueSnapshotPauseGate final {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
};

void maybe_pause_queue_snapshot() noexcept;

namespace test_hooks {

void arm_queue_snapshot_pause(QueueSnapshotPauseGate& gate) noexcept;

void queue_snapshot_wait_paused(const QueueSnapshotPauseGate& gate) noexcept;

void release_queue_snapshot_pause(QueueSnapshotPauseGate& gate) noexcept;

void disarm_queue_snapshot_pause() noexcept;

} // namespace test_hooks

#endif

} // namespace sluice::async::detail
