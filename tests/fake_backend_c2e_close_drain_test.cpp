// Phase C2e — FakeAsyncBackend admission-transaction deterministic test
// (Issue #68 rows 15-16; ADR Decision 15 + §"Commit / accept" :453-462).
//
// The backend admission transaction domain serializes close_admission()
// against an in-flight submit's acceptance protocol. The fake's
// SLUICE_ASYNC_INTERNAL_TESTING SubmitPauseGate pauses the submit path AFTER
// the slot commit (Step 4: prepared -> pending) and BEFORE the `binding ->
// outstanding` release-store (ADR Step 5 — the commit/accept linearization
// point), INSIDE the admission transaction. close_admission() must BLOCK on
// the transaction: after close returns, no new acceptance LP may occur
// (Decision 15). The resumed submit completes its LP (submit wins), close
// returns after, the accepted request completes normally, and a new submit
// after close rejects invalid_state. This is the deterministic detector for
// the "close drops the admission transaction" mutation on the Fake driver
// (mutant M11-fake): without the transaction, close returns while the submit
// is paused before its LP — a new acceptance LP after close.
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);
// C2e (B1): failure-protection bound for the deterministic negative probe in
// fake_c2e_close_waits_for_inflight_acceptance_lp. The probe observes "the
// closer's read must not complete while the submit is paused": under the
// admission-transaction mutation the closer's close returns in microseconds,
// so this window is >=1000x the defect latency; the ordering proof itself is
// structural (the admission lock + the pause gate), not this window.
constexpr auto kCloseProbeTimeout = std::chrono::seconds(2);

// Bounded wait on an atomic flag. Returns true when the flag became true.
bool wait_flag(std::atomic<bool>& flag, std::chrono::steady_clock::time_point deadline) {
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

SLUICE_MAIN()

SLUICE_TEST_CASE(fake_c2e_close_waits_for_inflight_acceptance_lp) {
    FakeAsyncBackend backend{/*request_capacity=*/1};
    FakeAsyncBackend::SubmitPauseGate gate;
    backend.set_submit_pause_after_commit(&gate);

    std::byte buf[1]{};
    Completion<std::size_t> c;
    Completion<std::size_t> c2;
    std::atomic<bool> submit_done{false};
    std::atomic<bool> submit_ok{false};
    std::atomic<bool> close_done{false};

    std::thread submitter([&] {
        auto r = backend.submit_read(ReadOp{0, buf, 1, 0}, c);
        submit_ok.store(r.has_value(), std::memory_order_release);
        submit_done.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    const char* fail_msg = nullptr;
    if (!wait_flag(gate.paused, deadline)) {
        fail_msg = "submit gate did not pause in time";
    } else if (submit_done.load(std::memory_order_acquire)) {
        fail_msg = "submitter must still be paused";
    } else if (c.outstanding()) {
        fail_msg = "the Completion must still be `binding` at the pause "
                   "(the LP is the binding -> outstanding release-store)";
    }

    // close_admission() must wait for the in-flight acceptance protocol.
    std::atomic<bool> close_saw_outstanding{false};
    std::thread closer;
    if (fail_msg == nullptr) {
        closer = std::thread([&] {
            backend.close_admission();
            // Decision-15 observable AT the close return: a submit that
            // entered the protocol before close has already passed its
            // acceptance LP (`binding -> outstanding` release-store, ADR
            // Step 5). Under the fix the admission-lock handoff makes the
            // submit's LP release-store visible to this read (mutex acquire
            // after the submit's mutex release); under the mutation the read
            // completes while the submitter is still paused and sees
            // `binding` — the violation this case must catch.
            close_saw_outstanding.store(c.outstanding(), std::memory_order_release);
            close_done.store(true, std::memory_order_release);
        });
        // Deterministic negative probe (the 2 s window is failure protection
        // only — see kCloseProbeTimeout): while the submit is paused before
        // its LP, the closer's read must NOT complete — a completed read means
        // close returned before the LP (the Decision-15 violation; mutant
        // M11-fake detector). The submitter cannot advance until the test
        // resumes it (it spins on the gate), and under the fix the closer is
        // blocked on the admission lock the paused submitter holds, so the
        // probe can only fire under the mutation.
        const auto probe_deadline = std::chrono::steady_clock::now() + kCloseProbeTimeout;
        while (std::chrono::steady_clock::now() < probe_deadline) {
            if (close_done.load(std::memory_order_acquire)) {
                fail_msg = "close_admission returned before the in-flight "
                           "acceptance LP (admission transaction violated)";
                break;
            }
            std::this_thread::yield();
        }
    }

    // Resume: the submit completes its LP (binding -> outstanding), submit
    // returns success; then close acquires the admission lock and returns.
    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        submitter.join();
        if (!submit_ok.load(std::memory_order_acquire)) {
            fail_msg = "submit must have succeeded (the in-flight LP wins)";
        } else if (!wait_flag(close_done, deadline)) {
            fail_msg = "close_admission must return after the in-flight submit's LP";
        } else if (!close_saw_outstanding.load(std::memory_order_acquire)) {
            fail_msg = "close_admission returned before the in-flight acceptance "
                       "LP (admission transaction violated)";
        } else if (backend.outstanding() != 1) {
            fail_msg = "the accepted request must be outstanding";
        }
    }

    // The accepted request completes normally after close (the fake completes
    // it; reap publishes the real result).
    if (fail_msg == nullptr) {
        backend.complete_oldest_with_bytes(1);
        if (backend.poll() != 1) {
            fail_msg = "reap must publish the completed request";
        } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
            fail_msg = "the result must be the completed 1-byte read verbatim";
        }
    }

    // A NEW submit after close returned rejects synchronously (Decision 15).
    if (fail_msg == nullptr) {
        auto r2 = backend.submit_read(ReadOp{0, buf, 1, 0}, c2);
        if (r2.has_value()) {
            fail_msg = "a submit after close returned must be rejected";
        } else if (r2.error().code != IoError::Code::invalid_state) {
            fail_msg = "the post-close rejection must be invalid_state";
        } else if (!c2.idle() || c2.outstanding() || c2.ready()) {
            fail_msg = "the rejected Completion must be idle (zero residue)";
        }
    }

    // cleanup (both paths): resume + join the submitter FIRST (the closer may
    // be blocked on the admission lock behind the paused submit), then join
    // the closer; drain + reset so the arena destructor sees quiescence.
    gate.resume.store(true, std::memory_order_release);
    if (submitter.joinable()) submitter.join();
    if (closer.joinable()) closer.join();
    if (c.ready()) c.reset();
    // Fail-path only: if the post-close submit was (incorrectly) accepted,
    // complete + reap + reset it so the failure stays the case, not a
    // destructor fail-fast.
    backend.complete_oldest_with_bytes(1);
    (void)backend.poll();
    if (c2.ready()) c2.reset();
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}
