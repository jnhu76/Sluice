// detail::submit_transaction — the ONE pre-accept submission ladder shared
// by every in-repo explicit-I/O backend.
//
// Authority (AGENTS.md §3.2; docs/architecture/async-request-lifecycle.md):
// this function owns ONLY the correctness-critical pre-accept ladder —
//
//   stage0_precheck -> reserve -> validate -> prepare -> write_scratch
//   -> install_publication_binding -> begin_binding (CAS) -> commit
//   -> [pause seam] -> install_binding + commit_binding (the accept LP)
//
// plus the rollback ladder:
//
//   reserve fail          -> return error            (no slot held)
//   injected/validate fail-> rollback_reserved_or_prepared; return error
//   prepare fail          -> rollback_reserved_or_prepared; return error
//   binding-install fail  -> rollback_reserved_or_prepared; return error
//   begin_binding loser   -> rollback_reserved_or_prepared; return error
//                            (own slot only — the winner's slot and the
//                            shared Completion are never touched)
//   injected/commit fail  -> rollback_binding FIRST (binding -> idle), THEN
//                            rollback_reserved_or_prepared; return error
//   — after commit_binding NO failure representation exists: the function
//     contains no rejection return past the LP; an accidental throw
//     hits the noexcept boundary and terminates (fail-fast), never a
//     rejection-after-accept.
//
// It acquires NO backend / admission / Scheduler / wake-domain lock
// directly, performs NO wake, allocates NOTHING, and touches NO
// Scheduler / wait-source / backend-queue state. It invokes RequestArena
// leaf operations (reserve, prepare, install_publication_binding, commit,
// rollback_reserved_or_prepared), which acquire the arena leaf mutex
// exactly as before centralization — no lock-domain change. The caller
// (the backend) holds its own admission discipline around the whole call
// — context access_mtx_ serialization (Sync), admission_mtx_
// (Fake/ThreadPool), or dispatch_mtx_ with the in-lock Stage-0 gate
// (Uring) — and performs stage-4 enqueue itself after the call returns
// (backend execution ownership, unchanged).
//
// Policy contract (per backend; nested in the backend class so the binding
// trio reaches the protected AsyncBackend statics). Surface budget
// (mechanically countable; any addition requires re-review):
//
//   4 argument/data adapters    (kind, borrow, requested_bytes, publish_thunk)
//   4 Completion-binding adapters (begin_binding, install_binding,
//                                  commit_binding, rollback_binding)
//   4 backend-divergence hooks  (stage0_precheck, validate, write_scratch,
//                                pause_before_commit_binding)
//   1 test-only injection hook  (injected_precommit_stage_failure;
//                                SLUICE_ASYNC_INTERNAL_TESTING only)
//
//   using completion_type = Completion<std::size_t> | Completion<void>;
//   // --- data accessors (pure; mirror arena/Completion call arguments) ---
//   detail::OperationKind kind() const noexcept;
//   detail::BorrowMetadata borrow(const Op& op) const noexcept;
//   std::uint64_t requested_bytes(const Op& op) const noexcept;
//   void (*publish_thunk() const noexcept)(void*, const detail::TerminalResult&) noexcept;
//   // --- binding trio (backend protected statics; never fail) ---
//   bool begin_binding(completion_type& c) noexcept;
//   void install_binding(completion_type& c, detail::RequestArena*,
//                        detail::SlotHandle) noexcept;
//   void commit_binding(completion_type& c) noexcept;
//   void rollback_binding(completion_type& c) noexcept;  // before-accept form
//   // --- production hooks ---
//   Result<void> stage0_precheck() noexcept;   // Uring: ring/poison/admission, verbatim
//   Result<void> validate(const Op& op) noexcept;
//   void write_scratch(detail::SlotHandle h, const Op& op) noexcept;
//   void pause_before_commit_binding() noexcept;  // test seam, guarded body
//   // --- test-guarded injection seam (member exists only under
//   //     SLUICE_ASYNC_INTERNAL_TESTING; call sites below compiled out of
//   //     production builds — no branch, no local, no symbol) ---
//   std::optional<IoError> injected_precommit_stage_failure(
//       detail::SubmitStage stage) noexcept;
#pragma once

#include <sluice/async/detail/request_arena.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <optional>
#include <utility>

namespace sluice::async::detail {

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// Test-guarded stage vocabulary for the C2d pre-commit failure-injection
// seam (ThreadPoolBackend's harness; the backends alias it). Production
// builds never compile this enum, the policy members, or the guarded call
// sites below.
enum class SubmitStage { reserve, prepare, commit };
#endif

// The shared ladder. Returns the accepted slot handle on success (the caller
// performs stage-4 enqueue with it) or the synchronous rejection verbatim.
template <class Policy>
Result<SlotHandle> submit_transaction(RequestArena& arena,
                                      typename Policy::completion_type& c,
                                      const typename Policy::op_type& op,
                                      Policy& policy) noexcept {
    // Stage 0: backend ring/poison/admission gate, under the CALLER's
    // admission discipline (the hook may assume it). Uring returns its
    // ring, poison, and admission errors verbatim BEFORE reserve
    // (hook-internal precedence: ring -> poison -> admission); the
    // reference and ThreadPool policies are trivial ok. The shared
    // function never assumes a healthy backend: health is a policy
    // question.
    if (auto pre = policy.stage0_precheck(); !pre.has_value()) {
        return make_unexpected<SlotHandle>(pre.error());
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // C2d (ADR Gate 4): injected reserve failure — before any slot exists,
    // so no rollback (zero residue by construction).
    if (auto inj = policy.injected_precommit_stage_failure(SubmitStage::reserve);
        inj.has_value()) {
        return make_unexpected<SlotHandle>(*inj);
    }
#endif
    // Stage 1: reserve. Arena full -> would_block; admission closed ->
    // invalid_state (ADR Decision 6/13: capacity pressure is NEVER
    // invalid_state).
    auto rh = arena.reserve();
    if (!rh.has_value()) {
        return make_unexpected<SlotHandle>(rh.error());
    }
    SlotHandle h = rh.value();
    // Stage 1.5: descriptor validation INSIDE the admission transaction,
    // AFTER reserve — admission/capacity take precedence over a malformed
    // descriptor (ADR Decision 5 stage order). Reference backends defer
    // validation (DIV-14): their validate hook is trivial ok.
    if (auto v = policy.validate(op); !v.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(v.error());
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // C2d: injected prepare failure — AFTER a successful reserve; the
    // candidate slot rolls back through the same pre-commit rollback.
    if (auto inj = policy.injected_precommit_stage_failure(SubmitStage::prepare);
        inj.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(*inj);
    }
#endif
    // Stage 2: prepare (op kind + fd/buffer borrow metadata; borrowing
    // begins at commit, not here).
    if (auto ph = arena.prepare(h, policy.kind(), policy.borrow(op));
        !ph.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(ph.error());
    }
    // Backend scratch: the fixed prepared-op record (construction-bounded
    // array; dispatch reads it only after a current-generation running
    // transition). No-op for the reference backends.
    policy.write_scratch(h, op);
    // Stage 2.5: install the slot's Completion publication binding (the
    // slot is the identity carrier; reap publishes through it inside the
    // leaf domain). A later CAS loss rolls the binding back with the slot.
    if (auto bh = arena.install_publication_binding(
            h, &c, policy.requested_bytes(op), policy.publish_thunk());
        !bh.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(bh.error());
    }
    // Stage 3a: Completion CAS idle -> binding elects ONE submitter. A
    // loser rolls back ONLY this submit's own slot + binding; it never
    // touches the winner's slot or the shared Completion's binding state.
    if (!policy.begin_binding(c)) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(IoError{IoError::Code::invalid_state});
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // C2d: injected commit-boundary failure — the binding CAS already won
    // (Completion in `binding`), so this is the executable instance of the
    // commit-failure rollback: binding -> idle FIRST, then the slot
    // rollback (publication binding cleared, generation++, capacity
    // recyclable, accepted-outstanding untouched).
    if (auto inj = policy.injected_precommit_stage_failure(SubmitStage::commit);
        inj.has_value()) {
        policy.rollback_binding(c);
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(*inj);
    }
#endif
    // Stage 3b: commit (prepared -> pending, enqueue pin live, accepted++,
    // borrow begins — the submit-success LP's slot half).
    if (auto ch = arena.commit(h); !ch.has_value()) {
        policy.rollback_binding(c);
        (void)arena.rollback_reserved_or_prepared(h);
        // Observable error mapping: commit failure on the submission path
        // is mapped to invalid_state, matching the pre-centralization
        // per-backend behavior (accepted constraint: error precedence /
        // observable behavior identical per backend).
        (void)ch; // arena error consumed — invalid_state is the contract
        return make_unexpected<SlotHandle>(IoError{IoError::Code::invalid_state});
    }
    // Deterministic causal seam: between the arena commit and the
    // binding->outstanding release-store. Bodies are guarded inside the
    // policies; production policies compile an empty inline.
    policy.pause_before_commit_binding();
    // Stage 3c: install the slot-release capability, then publish
    // outstanding — the `binding -> outstanding` release-store is the
    // commit/accept linearization point (ADR Step 5). AFTER this nothing
    // may fail: the only remaining statement is the success return.
    policy.install_binding(c, &arena, h);
    policy.commit_binding(c);
    return h;
}

} // namespace sluice::async::detail
