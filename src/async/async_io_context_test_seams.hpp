#pragma once

#include <sluice/async/async_io_context.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>

namespace sluice::async {

struct AsyncIoContext::WaitSourceProgressPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> exited{false};

  private:
    friend class AsyncIoContext;
    std::atomic<bool> resume{false};
};

inline void AsyncIoContext::set_wait_source_progress_pause_gate_for_test(
    WaitSourceProgressPauseGate* gate) noexcept {
    wait_source_progress_gate_.store(gate, std::memory_order_release);
}

inline void AsyncIoContext::resume_wait_source_progress_gate_for_test(
    WaitSourceProgressPauseGate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

} // namespace sluice::async

#endif
