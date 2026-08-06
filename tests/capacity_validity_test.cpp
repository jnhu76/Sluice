// Capacity-case validity fixture (issue #68 C2a, CORRECTION 6).
//
// Proves the shared C2a capacity cases are EFFECTIVE: a deliberately
// nonconforming backend must make run_capacity_cases() return the SPECIFIC
// failing case name (never just a whole-suite bool). Each violation targets
// exactly one case; the cases before it still pass, so the reported failure
// name pins WHICH assertion caught the defect.
//
// The backend is intentionally MINIMAL: it only needs to misbehave on capacity
// accounting, not implement the full async surface. It is:
//   * guarded by SLUICE_ASYNC_INTERNAL_TESTING (test-only, never built into
//     production sluice_async or the public extension surface);
//   * NOT registered in the conformance manifest (scripts/backend_conformance_
//     manifest.py) — it never affects any normal backend verdict;
//   * driven ONLY through run_capacity_cases() with the same
//     make_backend_with_capacity seam the real backends use.
//
// Violations and the exact case each one pins:
//   over_accept         accepts the (N+1)th op          -> capacity_rejects_with_idle_completion
//   bind_rejected       claims the rejected Completion  -> capacity_rejects_with_idle_completion
//   late_bind_only      rejected op bound to outstanding -> capacity_rejection_never_completes
//                       later by progress (never published)
//   late_complete       completes a rejected op on poll -> capacity_rejection_never_completes
//   misclassify_invalid non-idle submit -> would_block  -> capacity_stats_are_exact
//   inflate_outstanding outstanding() over-reports +1   -> capacity_accepts_exact_limit
//   no_recycle          capacity never recycles         -> capacity_recycles_after_reset
//
// The None violation is a sanity control: a conforming minimal backend must
// pass all five capacity cases.

#include "backend_conformance.hpp"
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

namespace {

using namespace sluice::async;
using sluice::IoError;
using sluice::make_unexpected;
using sluice::Result;

// The deliberately nonconforming capacity behavior to inject.
enum class CapacityViolation {
    none,                // conforming control (must pass all cases)
    over_accept,         // capacity full still accepts+binds (N+1)th, returns
                         // success — c3 must be caught by cleanup, not left
                         // for a destructor fail-fast
    bind_rejected,       // capacity reject still claims the Completion and
                         // returns would_block — WITHOUT recording any cancel
                         // intent (no self-cleanup); only the fixture's
                         // cleanup can terminalize c3
    late_bind_only,      // capacity reject returns would_block with the
                         // Completion still idle; backend progress LATER
                         // binds it to outstanding (never publishing). The
                         // fixture must have tracked it at submit time, or
                         // cleanup cannot find it (timeout/abort)
    late_complete,       // rejected op is later completed by backend progress
    misclassify_invalid, // non-idle Completion submit -> would_block (not
                         // invalid_state): the queue_full/invalid_state split
    inflate_outstanding, // outstanding() over-reports by 1 while live
    no_recycle,          // capacity never recycles after reap (never releases)
};

// A MINIMAL AsyncBackend that misbehaves ONLY on capacity accounting. It
// tracks accepted Completions itself (no RequestArena — this is a validity
// fixture, not a lifecycle-conforming backend). Size ops (read/write) are
// supported; void ops return invalid_argument (the capacity cases only submit
// reads). Claim/publish of a rejected Completion uses the protected
// two-stage binding WITHOUT installing an arena release capability, so the
// Completion is a "probe-driven" completion: reset() skips the arena release
// and is safe (include/sluice/async/completion.hpp reset()).
class NonConformingCapacityBackend : public AsyncBackend {
  public:
    explicit NonConformingCapacityBackend(std::size_t capacity,
                                          CapacityViolation violation)
        : capacity_(capacity), violation_(violation) {}

    Result<void> submit_read(ReadOp, Completion<std::size_t>& c) override {
        return submit_size(c);
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>& c) override {
        return submit_size(c);
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }

    // Reap: complete every op with a recorded cancel intent, then (for the
    // late_* violations) act on every op that was rejected but must misbehave
    // on backend progress. Publishes through the protected reap authority.
    std::size_t poll() override {
        std::size_t n = 0;
        for (auto* c : canceled_) {
            if (c->outstanding()) {
                publish(*c, make_unexpected<std::size_t>(
                                IoError{IoError::Code::canceled}));
                ++n;
            }
        }
        canceled_.clear();
        if (violation_ == CapacityViolation::late_complete) {
            for (auto* c : rejected_) {
                if (c->idle() && begin_binding(*c)) {
                    commit_binding(*c);
                    publish(*c, make_unexpected<std::size_t>(
                                    IoError{IoError::Code::canceled}));
                    ++n;
                }
            }
            rejected_.clear();
        } else if (violation_ == CapacityViolation::late_bind_only) {
            // Bind the rejected op to outstanding (idle -> binding ->
            // outstanding) WITHOUT publishing a terminal: the op becomes
            // observable via outstanding() but never completes on its own.
            // Only cleanup's cancel -> reap can resolve it — proving the
            // fixture tracked the still-idle-at-submit Completion.
            for (auto* c : rejected_) {
                if (c->idle() && begin_binding(*c)) {
                    commit_binding(*c);
                    bogus_.push_back(c);
                }
            }
            rejected_.clear();
        }
        // Reap ends the backend's borrow of every published Completion: drop
        // the now-ready ops from the tracking lists so outstanding() (which the
        // AsyncIoContext destructor calls during fixture teardown) never
        // dereferences a Completion after the case function has destroyed it.
        // This mirrors a real arena backend, whose outstanding() counts live
        // slots without holding Completion pointers.
        remove_ready(accepted_);
        remove_ready(bogus_);
        return n;
    }
    Result<std::size_t> wait_one() override { return poll(); }

    void cancel(Completion<std::size_t>& c) override { canceled_.push_back(&c); }
    void cancel(Completion<void>&) override {}

    std::size_t outstanding() const noexcept override {
        std::size_t n = 0;
        for (auto* c : accepted_) {
            if (c->outstanding()) ++n;
        }
        for (auto* c : bogus_) {
            if (c->outstanding()) ++n;
        }
        if (violation_ == CapacityViolation::inflate_outstanding && n > 0) {
            ++n;  // over-report the high-water signal while anything is live
        }
        return n;
    }

  private:
    static void remove_ready(std::vector<Completion<std::size_t>*>& v) noexcept {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](Completion<std::size_t>* c) {
                                   return !c->outstanding();
                               }),
                v.end());
    }

    // Whether capacity is (from this backend's point of view) full. no_recycle
    // treats every ever-accepted op as occupying a slot forever (the reap does
    // not release capacity); every other violation counts live (outstanding)
    // ops, so reap releases capacity normally. inflate_outstanding only affects
    // the REPORTED outstanding() (the max_outstanding high-water signal), never
    // the internal admission check.
    bool at_capacity() const noexcept {
        if (violation_ == CapacityViolation::no_recycle) {
            // total_accepted_ never decreases: capacity is never recycled, even
            // after reap + reset (the case-E violation).
            return total_accepted_ >= capacity_;
        }
        std::size_t n = 0;
        for (auto* c : accepted_) {
            if (c->outstanding()) ++n;
        }
        for (auto* c : bogus_) {
            if (c->outstanding()) ++n;
        }
        return n >= capacity_;
    }

    Result<void> submit_size(Completion<std::size_t>& c) {
        // Caller lifecycle violation: non-idle Completion.
        if (!c.idle()) {
            if (violation_ == CapacityViolation::misclassify_invalid) {
                // Wrong classification: capacity vocabulary instead of the
                // invalid_state vocabulary (ADR Decision 6 split).
                return make_unexpected<void>(IoError{IoError::Code::would_block});
            }
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (at_capacity()) {
            switch (violation_) {
            case CapacityViolation::over_accept:
                // Over-accept by ACTUALLY binding the (N+1)th Completion: it
                // becomes genuinely outstanding and observable via
                // outstanding(). A previous version returned success WITHOUT
                // binding, which could not exercise the cleanup gap (the
                // Completion stayed idle). With a real claim the validity case
                // proves the fixture's submit_and_track() tracks a claimed-but-
                // should-be-rejected Completion so cleanup cancel/reap/reset
                // terminalizes it — instead of a destructor fail-fast masking
                // the capacity assertion. Record a cancel intent too so a
                // poll()/wait_one() reap resolves it; cleanup drives that path.
                if (begin_binding(c)) {
                    commit_binding(c);
                    bogus_.push_back(&c);
                    canceled_.push_back(&c);
                }
                return {};
            case CapacityViolation::bind_rejected:
                // Reject for capacity but STILL claim the Completion: it
                // becomes outstanding (a rejected op must stay idle). Deliberately
                // NO cancel intent is recorded here: the violation branch must
                // NOT self-clean (that would let the test pass even if the
                // fixture never tracked c3). Only cleanup's ctx.cancel(c3) ->
                // backend.cancel() -> canceled_ path can resolve it.
                if (begin_binding(c)) {
                    commit_binding(c);
                    bogus_.push_back(&c);
                }
                return make_unexpected<void>(IoError{IoError::Code::would_block});
            case CapacityViolation::late_bind_only:
                // Reject (Completion stays idle at submit time), remember the
                // op; backend progress later binds it to outstanding without
                // publishing (see poll()). A fixture that only tracked at
                // submit-success or at non-idle-after-submit would miss it.
                rejected_.push_back(&c);
                return make_unexpected<void>(IoError{IoError::Code::would_block});
            case CapacityViolation::late_complete:
                // Reject but remember the op; backend progress later completes
                // it (a rejected op must NEVER produce a completion).
                rejected_.push_back(&c);
                return make_unexpected<void>(IoError{IoError::Code::would_block});
            default:
                return make_unexpected<void>(IoError{IoError::Code::would_block});
            }
        }
        // Normal accept: claim the Completion (idle -> binding -> outstanding)
        // so the op is genuinely outstanding and observable via
        // outstanding(). No arena release capability is installed — this is a
        // probe-driven Completion, so reset() skips the arena release and is
        // safe. A lost claim CAS (impossible in this single-threaded fixture)
        // fails the submit synchronously.
        if (!begin_binding(c)) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        commit_binding(c);
        accepted_.push_back(&c);
        ++total_accepted_;
        return {};
    }

    std::size_t capacity_;
    CapacityViolation violation_;
    std::size_t total_accepted_ = 0;  // ever-accepted count (no_recycle uses it)
    std::vector<Completion<std::size_t>*> accepted_;  // normal accepts
    std::vector<Completion<std::size_t>*> bogus_;     // bound-but-rejected
    std::vector<Completion<std::size_t>*> rejected_;  // rejected, late-complete
    std::vector<Completion<std::size_t>*> canceled_;  // cancel intent
};

// Factory for the nonconforming backend. real_mode=false (no kernel I/O; the
// capacity cases only need the admission path), profile/mode mirror the Fake
// classification (deterministic reference) — the validity target is NOT part
// of the conformance manifest, so these fields only matter for driver output.
sluice_test::conformance::BackendFactory make_nonconforming_factory(
    CapacityViolation v) {
    using sluice_test::conformance::BackendFactory;
    BackendFactory f;
    f.name = "NonConformingCapacity";
    f.make_backend = [] {
        return std::make_unique<NonConformingCapacityBackend>(
            64, CapacityViolation::none);
    };
    f.make_backend_with_capacity = [v](std::size_t cap) {
        return std::make_unique<NonConformingCapacityBackend>(cap, v);
    };
    f.make_temp_fd = nullptr;
    f.real_mode = false;
    f.profile = "ReferenceProfile";
    f.mode = "deterministic";
    return f;
}

}  // namespace

// ---- validity: each violation makes the SPECIFIC case fail -----------------
// run_capacity_cases returns the FIRST failing case name. Each violation below
// must leave the earlier cases green and pin exactly the case that asserts the
// violated property. A whole-suite bool could not prove this; the case name can.

SLUICE_TEST_CASE(capacity_validity_over_accept) {
    // Over-accept by ACTUALLY binding the (N+1)th Completion (it becomes
    // outstanding) must fail capacity_rejects_with_idle_completion (the
    // would_block + idle assertions), not a later case. Crucially, because the
    // backend bound c3 before returning success, submit_and_track's non-idle
    // branch must register c3 so cleanup cancel/reap/reset terminalizes it —
    // proving a bound over-accept does NOT rely on a destructor fail-fast.
    const auto f = make_nonconforming_factory(CapacityViolation::over_accept);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_rejects_with_idle_completion",
                     "over_accept must fail capacity_rejects_with_idle_completion, got: " +
                         failed);
}

SLUICE_TEST_CASE(capacity_validity_bind_rejected) {
    // Claiming a rejected Completion (it becomes outstanding/not-idle) must
    // fail capacity_rejects_with_idle_completion (the c3.idle() assertions).
    const auto f = make_nonconforming_factory(CapacityViolation::bind_rejected);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_rejects_with_idle_completion",
                     "bind_rejected must fail capacity_rejects_with_idle_completion, got: " +
                         failed);
}

SLUICE_TEST_CASE(capacity_validity_late_complete) {
    // A rejected op that later completes on backend progress must fail
    // capacity_rejection_never_completes (the idle-throughout assertions).
    const auto f = make_nonconforming_factory(CapacityViolation::late_complete);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_rejection_never_completes",
                     "late_complete must fail capacity_rejection_never_completes, got: " +
                         failed);
}

SLUICE_TEST_CASE(capacity_validity_late_bind_only) {
    // A rejected op that stays idle AT SUBMIT TIME but is later bound to
    // outstanding by backend progress (never published) must fail
    // capacity_rejection_never_completes with a STABLE case name — NOT a
    // cleanup timeout/abort or a destructor fail-fast. This pins the
    // pre-submit tracking of submit_and_track: a fixture that only tracked
    // submit-success or non-idle-after-submit would not find this Completion
    // during cleanup and would abort on the deadline.
    const auto f = make_nonconforming_factory(CapacityViolation::late_bind_only);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_rejection_never_completes",
                     "late_bind_only must fail capacity_rejection_never_completes, got: " +
                         failed);
}

SLUICE_TEST_CASE(capacity_validity_misclassify_invalid) {
    // Conflating invalid_state with would_block (queue_full) must fail
    // capacity_stats_are_exact (the exact counter-split assertions).
    const auto f =
        make_nonconforming_factory(CapacityViolation::misclassify_invalid);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_stats_are_exact",
                     "misclassify_invalid must fail capacity_stats_are_exact, got: " +
                         failed);
}

SLUICE_TEST_CASE(capacity_validity_inflate_outstanding) {
    // max_outstanding reaching N+1 (over-reported outstanding) must fail
    // capacity_accepts_exact_limit (the outstanding/max_outstanding assertions).
    const auto f =
        make_nonconforming_factory(CapacityViolation::inflate_outstanding);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_accepts_exact_limit",
                     "inflate_outstanding must fail capacity_accepts_exact_limit, got: " +
                         failed);
}

SLUICE_TEST_CASE(capacity_validity_no_recycle) {
    // Capacity that never recycles after reap must fail
    // capacity_recycles_after_reset (the fresh-submit-after-reset assertion).
    const auto f = make_nonconforming_factory(CapacityViolation::no_recycle);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_recycles_after_reset",
                     "no_recycle must fail capacity_recycles_after_reset, got: " +
                         failed);
}

// ---- sanity control: a conforming minimal backend passes all five cases -----
SLUICE_TEST_CASE(capacity_validity_conforming_backend_passes) {
    const auto f = make_nonconforming_factory(CapacityViolation::none);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed.empty(),
                     "conforming minimal backend must pass all capacity cases, got: " +
                         failed);
}

// ---------------------------------------------------------------------------
// PR #69 review-finding regression evidence (Issue #68 Rev 3 cleanup principle).
//
// A/B/C pin the three properties the bare `case_bail`-only wrapper + the
// success-only submit_and_track() could not prove before this change:
//   A. bound over-accept cleanup: a backend that binds the (N+1)th Completion
//      and returns success must still be cleaned up (no destructor fail-fast);
//   B. bound-but-error cleanup: a backend that binds a Completion and returns
//      would_block must still be cleaned up;
//   C. catch-all exception cleanup: a body that throws a non-case_bail
//      exception while an op is outstanding must run cleanup and rethrow.
//
// They reuse the same CapacityFixture / run_capacity_case / nonconforming
// backend the validity cases use, so they exercise the REAL cleanup path. The
// process would crash (Completion/AsyncIoContext destructor fail-fast) if
// cleanup did not terminalize the bound-then-violated Completion.
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(capacity_regression_bound_over_accept_cleanup) {
    // Regression A: over_accept ACTUALLY binds the (N+1)th Completion (c3
    // becomes outstanding) and returns SUCCESS. The case fails
    // capacity_rejects_with_idle_completion (the idle assertion); REACHING
    // the end of the case (no process death) means cleanup terminalized the
    // bound c3 instead of letting the AsyncIoContext destructor fail-fast
    // mask the assertion. Note: over_accept returns success, so ANY
    // success-tracking helper would register c3 — the error-return equivalent
    // is pinned by regression B (bind_rejected, which returns would_block
    // with no backend self-cleanup).
    const auto f = make_nonconforming_factory(CapacityViolation::over_accept);
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    SLUICE_CHECK_MSG(failed == "capacity_rejects_with_idle_completion",
                     "regression A: over_accept must fail "
                     "capacity_rejects_with_idle_completion, got: " + failed);
}

SLUICE_TEST_CASE(capacity_regression_bound_but_error_cleanup) {
    // Regression B: bind_rejected binds c3 (outstanding) and returns
    // would_block. The violation branch deliberately records NO cancel intent
    // (no self-cleanup): the ONLY path that can terminalize c3 is the
    // fixture's cleanup, so submit_and_track MUST have tracked c3 before the
    // case inspects the result. Assert the tracking DIRECTLY (not only "the
    // process did not die"): c3 is in fx.tracked, then cleanup_or_abort
    // drives outstanding to 0 (cancel -> reap -> reset) with no destructor
    // fail-fast.
    using sluice_test::conformance::CapacityFixture;
    CapacityFixture fx(make_nonconforming_factory(CapacityViolation::bind_rejected)
                           .make_backend_with_capacity(2));
    Completion<std::size_t> c1, c2, c3;
    std::byte buf1[4]{}, buf2[4]{}, buf3[4]{};
    SLUICE_CHECK_MSG(
        fx.submit_and_track(c1, fx.make_read_op(-1, buf1, 4)).has_value(),
        "regression B: c1 accept must succeed");
    SLUICE_CHECK_MSG(
        fx.submit_and_track(c2, fx.make_read_op(-1, buf2, 4)).has_value(),
        "regression B: c2 accept must succeed");
    auto r3 = fx.submit_and_track(c3, fx.make_read_op(-1, buf3, 4));
    SLUICE_CHECK_MSG(!r3.has_value(),
                     "regression B: c3 must be rejected");
    SLUICE_CHECK_MSG(r3.error().code == IoError::Code::would_block,
                     "regression B: rejection must be would_block");
    // The backend ILLEGALLY bound c3 before returning the error: it is
    // non-idle, and the helper must have tracked it BEFORE the case inspected
    // the result (pre-submit registration).
    SLUICE_CHECK_MSG(!c3.idle(),
                     "regression B: broken backend must have bound c3");
    SLUICE_CHECK_MSG(
        std::find(fx.tracked.begin(), fx.tracked.end(), &c3) != fx.tracked.end(),
        "regression B: c3 must be in fx.tracked before the case inspects "
        "the result — cleanup cannot find a Completion the helper never "
        "registered");
    // Cleanup must cancel/reap/reset all three; reaching this line without a
    // destructor fail-fast proves the fixture's tracking + cleanup, not the
    // backend's self-cleanup.
    fx.cleanup_or_abort("NonConformingCapacity", "regression_b");
    SLUICE_CHECK_MSG(fx.ctx.outstanding() == 0,
                     "regression B: cleanup must drive outstanding to 0");
}

SLUICE_TEST_CASE(capacity_regression_catch_all_exception_cleanup) {
    // Regression C: a case body that throws a non-case_bail exception while an
    // op is accepted/outstanding must run cleanup (terminalize the op) and
    // rethrow. The old wrapper only caught case_bail, so an unexpected
    // exception (std::bad_alloc, std::runtime_error, ...) would unwind past
    // cleanup; the AsyncIoContext destructor would then fail-fast and mask the
    // original exception. We build a one-op fixture, accept an op, then run a
    // body that throws std::runtime_error; run_capacity_case must run cleanup
    // (outstanding -> 0) and rethrow. The process reaching the SLUICE_CHECK_MSG
    // after the catch proves cleanup ran; the caught runtime_error proves the
    // exception was not swallowed.
    using sluice_test::conformance::CapacityFixture;
    using sluice_test::conformance::run_capacity_case;
    CapacityFixture fx(make_nonconforming_factory(CapacityViolation::none)
                           .make_backend_with_capacity(1));
    Completion<std::size_t> c;
    std::byte buf[4]{};
    // Accept one op so it is genuinely outstanding during the throw.
    SLUICE_CHECK_MSG(
        fx.submit_and_track(c, fx.make_read_op(-1, buf, 4)).has_value(),
        "regression C: precondition accept must succeed");
    SLUICE_CHECK_MSG(fx.ctx.outstanding() == 1,
                     "regression C: one op outstanding before throw");

    bool rethrown = false;
    try {
        run_capacity_case(
            fx, "NonConformingCapacity", "regression_c", [] {
                throw std::runtime_error("regression-c-body");
            });
    } catch (const std::runtime_error& e) {
        rethrown = (std::string(e.what()) == "regression-c-body");
    }
    SLUICE_CHECK_MSG(rethrown,
                     "regression C: original std::runtime_error must rethrow");
    // cleanup ran: outstanding reached 0 (the accepted op was terminalized),
    // so the CapacityFixture destructor (and its AsyncIoContext) will not
    // fail-fast. The reset() below is belt-and-braces now that cleanup has
    // left the op ready.
    SLUICE_CHECK_MSG(fx.ctx.outstanding() == 0,
                     "regression C: cleanup must drive outstanding to 0");
    if (c.ready()) c.reset();
}

SLUICE_MAIN()

#else  // !SLUICE_ASYNC_INTERNAL_TESTING

// The validity fixture only exists under SLUICE_ASYNC_INTERNAL_TESTING. This
// build never runs it (the target is registered only in the internal-testing
// group), so a stray production build cannot execute it.
int main() { return 0; }

#endif
