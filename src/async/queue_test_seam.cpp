


















#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/detail/queue_test_seam.hpp>

#include <atomic>
#include <thread>

namespace sluice::async::detail {

namespace {




std::atomic<QueueSnapshotPauseGate*> g_queue_snapshot_pause_gate{nullptr};

}

void maybe_pause_queue_snapshot() noexcept {
    auto* gate = g_queue_snapshot_pause_gate.load(std::memory_order_acquire);
    if (gate == nullptr) {
        return;
    }



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

}

}

#endif
