// fake_test_seams.hpp - NON-INSTALLED internal-testing seam header for
// FakeAsyncBackend (C4 / issue #135: the internal-testing control plane must
// not shape the installed production header).
//
// Contains, under SLUICE_ASYNC_INTERNAL_TESTING only, the out-of-line
// definitions of the deterministic submit-pause gate struct and the bodies of
// the guarded test seams. The installed <sluice/async/fake_backend.hpp>
// keeps only the declarations plus the layout-bearing test member, and
// includes this header at its bottom under the same guard; production TUs
// (macro undefined) compile none of it. The PUBLIC synthetic-backend
// capability (auto_* completion mode, complete_oldest_*, arena introspection)
// is deliberately NOT here: that is FakeAsyncBackend's documented public
// scriptable surface, usable by downstream tests linking the production
// library without any internal macro (issue #135 C4/C8 classification).
#pragma once

#include <sluice/async/fake_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>
#include <thread>

namespace sluice::async {

// Deterministic causal seam (Phase B / review test-gap 1): pause the submit
// path between commit and enqueue so a backend-level test can interleave
// cancel exactly in the Scheme-B window (the window AsyncIoContext::
// access_mtx_ serialization hides). Test-only: production builds of this
// header (no macro) carry no field and no pause; the layout cost is
// accepted and documented (AGENTS.md §8 — internal-testing variants may
// carry guarded seams).
struct FakeAsyncBackend::SubmitPauseGate {
    std::atomic<bool> paused{false};  // the submit path set this when paused
    std::atomic<bool> resume{false};  // the test sets this to resume
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

}  // namespace sluice::async

#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
