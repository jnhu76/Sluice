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
    over_accept,         // capacity full still returns success (fake accept)
    bind_rejected,       // capacity reject still claims the Completion
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

    // Reap: complete every op with a recorded cancel intent, then (late_complete
    // only) complete every op that was rejected but must "late-complete" on
    // backend progress. Publishes through the protected reap authority.
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
                // Fake success: return success WITHOUT tracking (the op is
                // neither bound nor outstanding — a silently accepted N+1).
                return {};
            case CapacityViolation::bind_rejected:
                // Reject for capacity but STILL claim the Completion: it
                // becomes outstanding (a rejected op must stay idle). Also
                // record the cancel intent so cleanup/reap can resolve it
                // (otherwise it would stay outstanding forever).
                if (begin_binding(c)) {
                    commit_binding(c);
                    bogus_.push_back(&c);
                    canceled_.push_back(&c);
                }
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
    // Accepting the (N+1)th op must fail capacity_rejects_with_idle_completion
    // (the would_block + idle assertions), not a later case.
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

SLUICE_MAIN()

#else  // !SLUICE_ASYNC_INTERNAL_TESTING

// The validity fixture only exists under SLUICE_ASYNC_INTERNAL_TESTING. This
// build never runs it (the target is registered only in the internal-testing
// group), so a stray production build cannot execute it.
int main() { return 0; }

#endif
