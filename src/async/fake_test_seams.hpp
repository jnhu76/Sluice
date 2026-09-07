













#pragma once

#include <sluice/async/fake_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>
#include <thread>

namespace sluice::async {








struct FakeAsyncBackend::SubmitPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
};

inline void FakeAsyncBackend::set_submit_pause_after_commit(
    SubmitPauseGate* gate) noexcept {
    submit_pause_gate_ = gate;
}

inline void FakeAsyncBackend::wait_submit_pause_() noexcept {
    SubmitPauseGate* g = submit_pause_gate_.load(std::memory_order_relaxed);
    if (g == nullptr) return;
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

}

#endif
