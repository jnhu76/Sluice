# backend_conformance_manifest.py
#
# Phase C1 (test(async): establish the explicit-I/O backend conformance gate).
#
# Single source of truth for the explicit-I/O backend conformance gate. The
# aggregate gate (scripts/verify-backend-conformance.py) imports this module and
# drives every named target; the pure-data self-test
# (scripts/tests/test_backend_conformance_manifest.py) checks ONLY the
# manifest-internal invariants below (target-existence is the GATE's preflight
# concern, not the unit test's).
#
# Design (review fixes):
#   * Profiles are a CLOSED set. A profile MUST NOT name a backend that has no
#     evidence object in the registry. The C1 registry covers Fake, ThreadPool,
#     and Uring only; Sync/Synthetic backends are deliberately NOT registered
#     (the shared driver does not instantiate them), so they are never inferred
#     conforming.
#   * Layers are a CLOSED set.
#   * Evidence status is explicit: a not_applicable entry MUST carry a reason;
#     a not_implemented entry is NEVER counted as PASS.
#   * The external_admission layer describes extension ADMISSION, not
#     conformance — the MinimalExternalBackend is not run through the shared
#     observable-semantics suite.
#
# This file is pure data + helpers. No xmake parsing, no filesystem mutation,
# no third-party imports.
"""Explicit-I/O backend conformance manifest (Phase C1)."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional


# ---------------------------------------------------------------------------
# Closed sets
# ---------------------------------------------------------------------------

# A profile describes a backend FAMILY and the mandatory case-set its members
# must satisfy. Only backends actually registered in BACKENDS may claim a
# profile — no nameless backends.
PROFILES: tuple[str, ...] = (
    "ReferenceProfile",   # Fake — deterministic, no real kernel, no real cancel
                          # interruption.
    "BlockingIoProfile",  # ThreadPool — real blocking syscalls, bounded workers.
    "KernelIoProfile",    # Uring — real kernel queue (Phase D). Currently the
                          # stub build is NOT CONFORMING.
)

# Evidence layers. Layers exist so the gate reports WHAT kind of contract a
# piece of evidence covers; they are not a pass/fail rubric by themselves.
LAYERS: tuple[str, ...] = (
    "shared",               # Shared observable semantics (the 8-case suite).
    "lifecycle",            # RequestSlot/identity/reap/terminal contracts.
    "authority",            # Negative-compile publication/ownership boundary.
    "backend_specific",     # Backend-specific mechanism (TP workers, Uring SQE).
    "external_admission",   # Public-extension-surface admission (NOT conformance).
)

# Closed execution-mode vocabulary per profile. The shared driver's
# [conformance-meta] line declares (profile, mode); the aggregate gate rejects
# a meta line whose mode is outside this set for the declared profile, so a
# driver regression (e.g. claiming "real" for a stub build) fails the gate
# instead of being silently accepted.
PROFILE_MODES: dict[str, tuple[str, ...]] = {
    "ReferenceProfile": ("deterministic",),  # Fake — deterministic, no kernel.
    "BlockingIoProfile": ("real",),          # ThreadPool — real blocking syscalls.
    "KernelIoProfile": ("real", "stub"),     # Uring — real or stub build; the
                                             # profile is NOT CONFORMING until
                                             # Phase D either way.
}

# Mandatory section coverage: every backend MUST have at least one PASS
# evidence record in each of these layers to be ELIGIBLE. external_admission is
# a separate probe (not a per-backend section).
MANDATORY_LAYERS_PER_BACKEND: tuple[str, ...] = (
    "shared",
    "lifecycle",
    "backend_specific",
)

# Evidence statuses.
STATUS_IMPLEMENTED = "implemented"           # has a target; gate runs it.
STATUS_NOT_APPLICABLE = "not_applicable"     # legitimately skipped (needs reason).
STATUS_NOT_IMPLEMENTED = "not_implemented"   # known gap; NEVER counts as PASS.

# Real-only evidence declares its accepted execution mode explicitly. The
# aggregate gate parses a target-emitted [evidence-meta] line and classifies a
# disallowed mode as INCOMPLETE — a stub build can prove build/API honesty but
# can never satisfy a real KernelIo requirement.
EVIDENCE_MODES: tuple[str, ...] = ("deterministic", "real", "stub")


# ---------------------------------------------------------------------------
# Registry: which backends exist in C1
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class BackendEntry:
    name: str               # display name (matches driver meta backend=)
    profile: str            # one of PROFILES
    driver_case: str        # the SLUICE_TEST_CASE in backend_conformance_test
                            # that drives the shared suite for this backend; may
                            # be "" if the shared suite does not cover it.
    capacity_driver_case: str = ""  # Phase C2a: the SLUICE_TEST_CASE that drives
                                    # the shared capacity cases for this backend.
    close_drain_driver_case: str = ""  # Phase C2e: the SLUICE_TEST_CASE that
                                       # drives the shared close/drain cases for
                                       # this backend. "" when the backend has
                                       # no close_admission seam (Uring before
                                       # D4) — the gap is the
                                       # uring_c2e_close_drain_not_implemented
                                       # manifest record, never a skip-as-pass.


# The C1 backend registry. Sync/Synthetic are intentionally absent.
BACKENDS: tuple[BackendEntry, ...] = (
    BackendEntry("Fake", "ReferenceProfile", "conformance_fake",
                 capacity_driver_case="conformance_capacity_fake",
                 close_drain_driver_case="conformance_close_drain_fake"),
    BackendEntry("ThreadPool", "BlockingIoProfile", "conformance_threadpool",
                 capacity_driver_case="conformance_capacity_threadpool",
                 close_drain_driver_case="conformance_close_drain_threadpool"),
    BackendEntry("Uring", "KernelIoProfile", "conformance_uring",
                 capacity_driver_case="conformance_capacity_uring"),
)


# ---------------------------------------------------------------------------
# Evidence records
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Evidence:
    evidence_id: str                    # unique stable id
    target: str                         # xmake target name
    cases: Optional[tuple[str, ...]] = None   # Exact SLUICE_TEST_CASE set required
                                              # for this evidence record. When
                                              # present, the aggregate gate MUST
                                              # observe every case run exactly once
                                              # and no unpinned case (the record's
                                              # required runtime execution set);
                                              # absence means no per-case claim.
    layer: str = "shared"               # one of LAYERS
    backends: tuple[str, ...] = ()      # which display backends this covers;
                                        # () means backend-agnostic (e.g.
                                        # arena lifecycle, authority probes).
    mandatory: bool = True              # must PASS for an ELIGIBLE verdict.
    status: str = STATUS_IMPLEMENTED
    reason: str = ""                    # required iff status == not_applicable.
    notes: str = ""
    required_modes: tuple[str, ...] = () # empty: mode-independent target;
                                         # otherwise exactly one target-emitted
                                         # [evidence-meta] mode must be allowed.


# Helper aliases for readability.
_F = ("Fake",)
_TP = ("ThreadPool",)
_U = ("Uring",)
_REAL_BACKENDS = ("Fake", "ThreadPool", "Uring")


# The evidence table. Each record is one piece of evidence the gate drives.
# No assertion is made here that the target exists in the build graph — the
# gate's preflight (xmake show -t / xmake build) verifies that.
EVIDENCE: tuple[Evidence, ...] = (
    # -----------------------------------------------------------------------
    # Layer: shared observable semantics (the 8-case BackendFactory suite).
    # One target drives all three registered backends; the per-backend result
    # is parsed from the driver's [conformance-meta] + [conformance] FAIL lines.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="shared_suite",
        target="backend_conformance_test",
        layer="shared",
        backends=_REAL_BACKENDS,
        notes="8-case shared suite: submit->reap exactly-once, positional "
              "independence, EOF, short-completion retry, terminal exactly-once, "
              "cancel-defined-terminal, stats, clean shutdown.",
    ),

    # -----------------------------------------------------------------------
    # Phase C2a — shared capacity/admission/accounting conformance. The
    # capacity cases live in the SAME target as shared_suite
    # (backend_conformance_test) and are driven by run_capacity_cases(), built
    # via make_backend_with_capacity. They assert ONLY AsyncIoContext-observable
    # state. After the D1 merge, Uring's existing UringConfig/RequestArena path
    # passes the exact suite in real-liburing mode. The gate drives every backend
    # in an isolated subprocess; Uring stub mode is explicitly INCOMPLETE rather
    # than a skip-as-pass.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="shared_capacity_suite",
        target="backend_conformance_test",
        layer="shared",
        backends=("Fake", "ThreadPool", "Uring"),
        notes="C2a capacity/admission/rejection/accounting cases: accepts "
              "exact capacity, (N+1)th rejects with would_block (rejected "
              "Completion stays idle; no async from a reject), exact stats "
              "split (submitted_ops committed-only; queue_full_retries vs "
              "invalid_state_rejections), max_outstanding <= capacity, "
              "recycle after cancel->reap->reset. Shared-observable only. Uring "
              "execution evidence requires real mode; stub is build/API-only.",
    ),

    # -----------------------------------------------------------------------
    # Layer: lifecycle protocol evidence (aggregated existing targets).
    # These prove the RequestSlot/identity/reap/terminal contracts. Most are
    # backend-agnostic (arena-level); the ThreadPool ones are tagged so the
    # BlockingIo profile gets explicit lifecycle coverage.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="arena_capacity_generation_release",
        target="request_arena_test",
        layer="lifecycle",
        backends=(),  # arena-level; applies to all migrated backends.
        notes="RequestSlot arena capacity/reserve/release/generation "
              "contract. C2b: generation advances exactly +1 on BOTH "
              "release authorities (rollback + completed-binding); reap "
              "delivers ReadyEvent.key in backend-known terminal-winner "
              "order (not slot-index/submit order).",
    ),
    Evidence(
        evidence_id="arena_cancel_intent_best_effort",
        target="request_arena_cancel_intent_test",
        layer="lifecycle",
        backends=(),
        notes="ADR D11 best-effort cancel: running cancel records INTENT only.",
    ),
    Evidence(
        evidence_id="arena_lifecycle_death",
        target="request_arena_death_test",
        layer="lifecycle",
        backends=(),
        notes="RequestArena release/destroy/state-transition fail-fast.",
    ),
    Evidence(
        evidence_id="reference_arena_lifecycle",
        target="reference_backend_arena_lifecycle_test",
        layer="lifecycle",
        backends=_F,
        notes="Fake/Sync driven by arena + 5-stage admission (Fake registered).",
    ),
    Evidence(
        evidence_id="reference_no_alloc_terminal",
        target="reference_backend_no_alloc_test",
        layer="lifecycle",
        backends=_F,
        notes="Zero-allocation submit->reap->reset + transactional rejection.",
    ),
    Evidence(
        evidence_id="completion_authority_death",
        target="completion_authority_death_test",
        layer="lifecycle",
        backends=(),
        notes="Completion publication authority fail-fast.",
    ),
    Evidence(
        evidence_id="completion_binding_protocol",
        target="completion_binding_test",
        layer="lifecycle",
        backends=(),
        notes="Two-stage idle->binding->outstanding claim.",
    ),
    Evidence(
        evidence_id="request_lifecycle_scheme_b",
        target="request_lifecycle_scheme_b_test",
        layer="lifecycle",
        backends=(),
        notes="Scheme B: pending cancel wins, enqueue no-op, exactly-one winner.",
    ),
    Evidence(
        evidence_id="backend_scheme_b_race_fake",
        target="backend_scheme_b_race_test",
        layer="lifecycle",
        backends=_F,
        notes="Fake commit/enqueue barrier -> real Scheme-B race.",
    ),
    Evidence(
        evidence_id="threadpool_scheme_b_race",
        target="threadpool_backend_scheme_b_race_test",
        layer="lifecycle",
        backends=_TP,
        notes="Real-TP Scheme-B race + enqueued-cancel-wins.",
    ),
    Evidence(
        evidence_id="threadpool_death",
        target="threadpool_backend_death_test",
        layer="lifecycle",
        backends=_TP,
        notes="Non-quiescent destruction fail-fast (Debug AND Release): "
              "destroying with enqueued / running / backend-ready-unreaped / "
              "completion-ready-unreset / pending (C2e) work terminates; "
              "quiescent close_admission + drain + reset exits 0.",
    ),

    # -----------------------------------------------------------------------
    # Phase C2b — generation / stale-key / cancel-winner / identity-bearing reap.
    # Rows 3-8 of Issue #68. The arena-level records prove the RequestSlot
    # state/generation/stale-event contract (backend-agnostic); the Fake and
    # ThreadPool integration records prove each backend actually uses the
    # protocol (not a side-band path). Uring's gap is the not_implemented
    # record below (Phase D pending). The C2b arena cases live within the
    # existing arena lifecycle targets alongside the pre-C2b Scheme-B cases;
    # the gate drives the full target so both generations of cases run.
    # -----------------------------------------------------------------------
    # NOTE on attribution: the legal/illegal state-transition matrix,
    # stale-handle-leaves-live-occupant-untouched, and RequestKey context
    # provenance cases live in request_lifecycle_scheme_b_test; the
    # generation +1 on both release authorities and reap terminal-winner
    # order cases live in request_arena_test (covered by the
    # arena_capacity_generation_release record below). Each record's notes
    # name ONLY the cases its target executes.
    Evidence(
        evidence_id="c2b_arena_state_identity_matrix",
        target="request_lifecycle_scheme_b_test",
        layer="lifecycle",
        backends=(),
        notes="C2b rows 3-4 (request_lifecycle_scheme_b_test cases only): "
              "legal/illegal state-transition matrix, stale-handle "
              "leaves live occupant untouched, RequestKey context "
              "provenance. Generation +1 on both release authorities "
              "and reap terminal-winner order live in request_arena_test "
              "(see arena_capacity_generation_release).",
    ),
    Evidence(
        evidence_id="c2b_fake_identity_integration",
        target="backend_scheme_b_race_test",
        layer="lifecycle",
        backends=_F,
        mandatory=True,
        notes="C2b rows 5-8 Fake integration: canceled_ops tallied only "
              "on terminal_won, binding identity A->A B->B, publication "
              "boundary (poll gates ready), stale-generation cancel "
              "harmless after release+reuse.",
    ),
    Evidence(
        evidence_id="c2b_threadpool_identity_integration",
        target="threadpool_backend_scheme_b_race_test",
        layer="lifecycle",
        backends=_TP,
        mandatory=True,
        notes="C2b rows 5-8 ThreadPool integration: enqueued cancel wins "
              "(no syscall), running cancel intent only (real result "
              "verbatim), stale-generation harmless, publication boundary "
              "(reap gates ready), cancel-vs-worker exactly-one winner.",
    ),
    Evidence(
        evidence_id="uring_c2b_identity_not_implemented",
        target="backend_conformance_test",
        layer="lifecycle",
        backends=_U,
        mandatory=True,
        status=STATUS_NOT_IMPLEMENTED,
        notes="C2b rows 3-8 still require complete Uring identity/generation/"
              "cancel/reap integration evidence in D3. Recorded as a known "
              "gap; not_implemented never "
              "counts as PASS. (Target is the Uring-driving conformance "
              "binary; the gap is not executed.)",
    ),

    # -----------------------------------------------------------------------
    # Phase C2c — waiter / borrow / delivery-lease (Issue #68 rows 11-14).
    # The arena-level record proves the RequestSlot borrow/waiter/lease
    # protocol (backend-agnostic: the contract exists for every migrated
    # backend); the Fake and ThreadPool integration records prove each
    # backend's REAL submit path carries the same arena borrow lifecycle and
    # routes waiter registration through the REAL arena register_waiter /
    # cancel_waiter authorities (no side-band waiter map). Uring's gap is the
    # not_implemented record below (Phase D pending), entered via
    # applicable_evidence_for_backend() so Uring's OWN verdict surfaces it —
    # never skip-as-pass.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="c2c_arena_borrow_waiter_lease_matrix",
        target="request_waiter_borrow_lease_test",
        layer="lifecycle",
        backends=(),
        notes="C2c rows 11-14 (arena): borrow lifecycle matrix (prepare "
              "inactive -> commit active -> survives pending/enqueued/running/"
              "backend_ready and every cancel/wait-cancel path -> reap ends "
              "it before completion-ready; rollback never borrows; stale "
              "handle cannot touch a new occupant's borrow), single-waiter "
              "registration state matrix + no-overwrite cardinality (final "
              "delivery = first waiter), wait-cancel vs I/O-cancel "
              "independence, move-only lease transfer chains, by-value "
              "ReadyEvent across slot reuse, register-vs-reap and "
              "cancel_waiter-vs-reap races (exactly-one lease ownership).",
    ),
    Evidence(
        evidence_id="c2c_fake_borrow_waiter_integration",
        target="backend_c2c_waiter_borrow_test",
        layer="lifecycle",
        backends=_F,
        mandatory=True,
        notes="C2c rows 11-14 Fake integration: real submit path carries the "
              "exact borrow metadata active; waiter seam routes a real "
              "accepted Completion through the REAL arena register_waiter/"
              "cancel_waiter authorities; complete_*/cancel only produce "
              "backend_ready while the borrow stays active until poll() "
              "reaps; the production sink delivers the registered token+lease "
              "exactly once; wait-cancel keeps the I/O; I/O cancel keeps the "
              "waiter; stale waiter authority cannot touch a live N+1 "
              "occupant.",
    ),
    Evidence(
        evidence_id="c2c_threadpool_borrow_waiter_integration",
        target="threadpool_backend_c2c_waiter_borrow_test",
        layer="lifecycle",
        backends=_TP,
        mandatory=True,
        notes="C2c rows 11-14 ThreadPool integration (deterministic pause "
              "gates): running borrow active with the exact submitted "
              "fd/addr/len; a registered waiter survives enqueued->running->"
              "backend_ready; running cancel intent ends neither the borrow "
              "nor the waiter; the backend_ready-before-reap window still "
              "shows the borrow active (a worker finishing its syscall is NOT "
              "the borrow lifetime end); wait-cancel != I/O cancel (the real "
              "syscall still runs); enqueued I/O cancel keeps the waiter; a "
              "stale waiter authority is harmless against a live N+1 "
              "occupant.",
    ),
    Evidence(
        evidence_id="uring_c2c_borrow_waiter_not_implemented",
        target="backend_conformance_test",
        layer="lifecycle",
        backends=_U,
        mandatory=True,
        status=STATUS_NOT_IMPLEMENTED,
        notes="C2c rows 11-14 still require complete Uring borrow/waiter/lease "
              "lifecycle evidence in D3. "
              "Recorded as a known gap; not_implemented never counts as "
              "PASS. (Target is the Uring-driving conformance binary, "
              "matching uring_c2b_identity_not_implemented; the gap is not "
              "executed.)",
    ),

    # -----------------------------------------------------------------------
    # Phase C2d — failure injection / accepted-terminal under allocator
    # failure (Issue #68 rows 9-10). The ThreadPool record proves, on the REAL
    # blocking backend with deterministic guarded injection seams (compiled
    # out of production builds): ADR Gate-4 per-stage pre-commit injection at
    # reserve / prepare / commit-boundary — injected reserve (would_block)
    # leaves the Completion idle with zero residue, injected prepare rolls
    # back the candidate slot (capacity recyclable), and the injected
    # COMMIT-BOUNDARY failure executes the REAL rollback_binding_before_accept
    # + slot rollback (the only executable instance of that branch; the
    # Completion returns to fully reusable idle and the same Completion +
    # capacity are immediately reusable); transactional pre-commit rejection
    # (binding-CAS loss -> invalid_state, zero residue, capacity recyclable);
    # partial worker-startup failure (the constructor stops and joins the
    # already-started workers and rethrows synchronously — finding P1-04);
    # post-commit permanent dispatch failure (injected between enqueue and
    # dispatch push, inside work_mtx_, with no worker ever able to see the
    # handle): submit still succeeds, the request reaches exactly ONE defined
    # backend_error terminal, reap publishes exactly once, the borrow stays
    # active until reap, no worker/syscall executes, and the ring-full
    # fail-fast invariant path is untouched — for BOTH the size and void
    # operation paths; post-commit no-allocation under an always-throw
    # operator new (ADR Decision 14 / I9) on the real worker path and on the
    # injected failure path; and the dispatch-failure terminal vs cancel
    # exactly-one-winner invariant (no overwrite, no double publication, at
    # most one tally: canceled_ops == 1 iff cancel won — the injected
    # backend_error terminal contributes no tally, as completion_errors is
    # unwired for ThreadPool). The Fake record proves the
    # reference path reaches a defined error terminal under a FULL-window
    # always-throw operator new (submit -> complete_oldest_with_error ->
    # poll -> reset, zero allocations). Uring's gap is the not_implemented
    # record below (Phase D pending).
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="c2d_threadpool_failure_injection",
        target="threadpool_backend_c2d_failure_test",
        layer="lifecycle",
        backends=_TP,
        mandatory=True,
        notes="C2d rows 9-10 ThreadPool integration (deterministic injection "
              "seams, SLUICE_ASYNC_INTERNAL_TESTING-only): ADR Gate-4 per-stage "
              "pre-commit injection at reserve (injected would_block, "
              "Completion idle, zero residue), prepare (candidate slot "
              "rolled back, capacity recyclable), and the COMMIT-BOUNDARY "
              "(the binding CAS wins, then commit fails — the submit path "
              "executes the REAL rollback_binding_before_accept + slot "
              "rollback, the only executable instance of that branch; the "
              "Completion returns to fully reusable idle); real-backend "
              "binding-CAS-loss rejection is transactional (invalid_state, "
              "zero residue, capacity recyclable); partial worker-startup "
              "failure stops+joins started workers and rethrows "
              "synchronously; post-commit permanent dispatch failure "
              "(injected after enqueue, before dispatch push, inside "
              "work_mtx_, handle never visible to a worker) leaves submit "
              "successful, reaches exactly one defined backend_error "
              "terminal, publishes once via reap, keeps the borrow active "
              "until reap, executes no worker/syscall (size + void paths); "
              "post-commit no-allocation under always-throw operator new "
              "(real worker path and injected path); dispatch-failure vs "
              "cancel exactly-one-winner with at most one tally (canceled_ops "
              "== 1 iff cancel won — the injected backend_error terminal "
              "contributes no tally) and no worker execution in every "
              "interleaving.",
    ),
    Evidence(
        evidence_id="c2d_fake_failure_injection_terminal",
        target="reference_backend_no_alloc_test",
        layer="lifecycle",
        backends=_F,
        mandatory=True,
        notes="C2d rows 9-10 Fake reference path: a FULL-window always-throw "
              "operator new around submit -> complete_oldest_with_error "
              "(manual dispatch terminal) -> poll (reap) -> reset proves the "
              "accepted request reaches exactly one defined backend_error "
              "terminal with zero allocations (ADR Decision 14 / I9) — the "
              "defined-error terminal is not allocation-gated either. "
              "Complements the existing success/rejection no-allocation "
              "cases in the same target.",
    ),
    Evidence(
        evidence_id="uring_c2d_failure_injection",
        target="uring_d2_failure_noalloc_test",
        layer="lifecycle",
        backends=_U,
        mandatory=True,
        status=STATUS_IMPLEMENTED,
        # Pinned required runtime case-set (Issue #81 P1 G2). The C++ source
        # tests/uring_d2_failure_noalloc_test.cpp is the REGISTRATION authority
        # (these SLUICE_TEST_CASE names); this tuple is the VERIFICATION
        # authority the aggregate gate's _drive() enforces against the binary's
        # actual [run] output. Without this pin, a mutant that deletes nine
        # load-bearing cases — leaving only the metadata case — still exits 0,
        # emits exactly one [evidence-meta] line, and is misclassified PASS.
        # The gate does NOT parse C++; the source↔manifest drift detector in
        # test_backend_conformance_manifest.py keeps the two authorities aligned.
        cases=(
            "uring_d2_evidence_mode",
            "uring_d2_precommit_size_rejections_leave_zero_new_residue",
            "uring_d2_precommit_void_rejections_leave_zero_new_residue",
            "uring_d2_ordinary_size_path_is_allocation_free",
            "uring_d2_ordinary_void_path_is_allocation_free",
            "uring_d2_permanent_recovery_size_and_void_are_allocation_free",
            "uring_d2_poison_rejects_after_capacity_is_recycled",
            "uring_d2_pending_cancel_and_class_a_recovery_have_one_winner_each",
            "uring_d2_poison_wait_never_submits_quarantined_write",
            "uring_d2_repeated_cancel_control_is_bounded_and_allocation_free",
        ),
        required_modes=("real",),
        notes="Phase D2 real-liburing C2d evidence: natural pre-commit reserve/"
              "descriptor/binding rejection leaves Completion idle and no new "
              "arena/dispatch/router/transport-ledger/SQE residue; existing D1 "
              "tests retain requests across EINTR/EAGAIN/EBUSY, zero progress, "
              "and positive partial transport reports without submit-prefix "
              "RequestState authority; a deterministic injected negative submit "
              "result (scripted -EIO; only kRealSubmit enters liburing) drives "
              "the production P0-D recovery controller verbatim to retire only "
              "proven Class-A work while older Class-C work remains bound for "
              "its original CQE — this does NOT independently reproduce the "
              "real kernel negative-enter physical state (that claim belongs "
              "to the D1 liburing/kernel source proof); always-throw allocator "
              "probes cover ordinary size/void accepted paths, Class-A "
              "operation recovery, and Class-C plus Class-A cancel-control "
              "recovery; pending cancel and repeated running cancel under "
              "transient/poison pressure prove exactly one terminal "
              "publication and one bounded live control reference. Stub mode is "
              "build/API evidence only and is classified INCOMPLETE by required_modes.",
    ),

    # -----------------------------------------------------------------------
    # Phase C2e — close / drain / reset / destruction (Issue #68 rows 15-16).
    # The shared close/drain suite (run_close_drain_cases, driven per backend
    # by conformance_close_drain_fake / conformance_close_drain_threadpool)
    # asserts ONLY AsyncIoContext-observable state plus the driver-wired
    # close/slot_in_use closures: close rejects future submit with
    # invalid_state (Completion idle, zero residue), accepted-before-close
    # still reaches exactly one defined terminal with cancel/poll/reap legal,
    # drained != releasable (slot_in_use stays 1 until the ready Completion is
    # reset), and slot-release vs admission-close orthogonality (reuse after
    # close is rejected). The ThreadPool deterministic target proves the
    # per-window races (close while pending/enqueued/running, close || final
    # record_terminal in both orderings, one-shot parked-waiter wake with no
    # busy-spin, submit || close linearization); the Fake death target proves
    # the reference backend's non-quiescent destruction fail-fasts through the
    # arena destructor and its quiescent path exits 0. Row 16 (quiescent
    # destruction) also keeps the threadpool_death / arena_lifecycle_death
    # records; the C2e ThreadPool death matrix adds the `pending` state case.
    # Uring's gap is the not_implemented record below (Phase D pending),
    # entered via applicable_evidence_for_backend() so Uring's OWN verdict
    # surfaces it — never skip-as-pass.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="c2e_shared_close_drain_suite",
        target="backend_conformance_test",
        layer="shared",
        backends=("Fake", "ThreadPool"),
        mandatory=True,
        notes="C2e rows 15-16 shared close/drain cases: close rejects future "
              "submit with invalid_state (Completion idle, no borrow, no "
              "outstanding, no submitted_ops, no residue); accepted-before-"
              "close still reaches exactly ONE defined terminal with "
              "cancel/poll/reap legal after close; drained != releasable "
              "(accepted_outstanding == 0 and Completion ready but slot_in_use "
              "== 1 until the caller resets the ready Completion, then == 0); "
              "slot-release and admission-close are orthogonal (a released "
              "slot does not re-open admission — a fresh submit after "
              "close/drain/reset is still invalid_state). Shared-observable "
              "only + the driver-wired close/slot_in_use closures.",
    ),
    Evidence(
        evidence_id="c2e_threadpool_close_drain_race",
        target="threadpool_backend_c2e_close_drain_test",
        layer="lifecycle",
        backends=_TP,
        mandatory=True,
        notes="C2e rows 15-16 ThreadPool deterministic window evidence "
              "(SLUICE_ASYNC_INTERNAL_TESTING pause gates): close while the "
              "submit path is paused between commit and enqueue (`pending`) / "
              "while `enqueued` on the ring / while the worker is `running` "
              "the syscall — in every window the accepted request completes "
              "with its REAL result verbatim (close never retroactively "
              "rejects, cancels, or discards; void path too); void submit "
              "after close -> invalid_state with idle Completion; close then "
              "pending cancel still WINS canceled (Scheme B: no dispatch "
              "linkage, no syscall, canceled_ops == 1); close then running "
              "cancel records intent only (real result verbatim); close wakes "
              "a parked wait_one as a ONE-SHOT control wake (0, no fabricated "
              "completion) and a FUTURE wait_one parks normally again (no "
              "busy-spin); close || final record_terminal in BOTH orderings "
              "(close first: the interrupted wait_one returns 0 after its "
              "final reap and the NEXT wait_one reaps the final ready — the "
              "control interrupt never swallows it; terminal first: close "
              "does not affect an already-stored terminal); an invariant-only "
              "close-vs-workers race drain (every accepted request reaches "
              "exactly one verbatim terminal, accounting zero); submit || "
              "close concurrent linearization (every attempt is "
              "accepted-then-terminal or synchronously invalid_state-idle — "
              "never half-accepted).",
    ),
    Evidence(
        evidence_id="c2e_fake_close_drain_death",
        target="fake_backend_death_test",
        layer="lifecycle",
        backends=_F,
        mandatory=True,
        notes="C2e row 16 Fake reference-path death evidence through the "
              "CONCRETE FakeAsyncBackend type: destroying the backend with a "
              "bound unreaped request, or with a completion-ready-but-unreset "
              "Completion (drained != releasable), fail-fasts in BOTH Debug "
              "and Release (request_arena_destruction_fail_fast); the "
              "quiescent path close_admission + drain + reset + destroy exits "
              "0.",
    ),
    Evidence(
        evidence_id="uring_c2e_close_drain_not_implemented",
        target="backend_conformance_test",
        layer="lifecycle",
        backends=_U,
        mandatory=True,
        status=STATUS_NOT_IMPLEMENTED,
        notes="C2e rows 15-16 (close_admission / drain / quiescent "
              "destruction) remain D4 work for Uring. "
              "Recorded as a known gap; not_implemented never counts as PASS. "
              "(Target is the Uring-driving conformance binary, matching the "
              "other uring_*_not_implemented records; the gap is not "
              "executed.)",
    ),

    # -----------------------------------------------------------------------
    # Layer: backend-specific mechanism evidence.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="fake_deterministic_injection",
        target="fake_backend_test",
        layer="backend_specific",
        backends=_F,
        notes="Fake deterministic test-vehicle contract.",
    ),
    Evidence(
        evidence_id="threadpool_phase_e_contract",
        target="threadpool_backend_phase_e_test",
        layer="backend_specific",
        backends=_TP,
        notes="Phase E: capacity/would_block, fd validation, EBADF, no-lost-wake.",
    ),
    Evidence(
        evidence_id="threadpool_functional",
        target="threadpool_backend_test",
        layer="backend_specific",
        backends=_TP,
        notes="Real blocking I/O read/write/sync correctness.",
    ),
    Evidence(
        evidence_id="threadpool_reap_bounded",
        target="threadpool_backend_reap_test",
        layer="backend_specific",
        backends=_TP,
        notes="Persistent worker pool bounded under load.",
    ),
    Evidence(
        evidence_id="uring_backend_contract",
        target="uring_backend_test",
        layer="backend_specific",
        backends=_U,
        # In a stub build the uring real-path cases are skipped inside the
        # binary; the gate classifies the KernelIo profile as NOT CONFORMING
        # regardless, so this record's per-mode outcome is informational.
        notes="Stub + real io_uring contract (mode parsed from driver meta).",
    ),

    # -----------------------------------------------------------------------
    # Layer: authority boundary (negative compile). Existing gates prove the
    # Completion and RequestArena authority boundaries; the new probe closes
    # the narrower gap that AsyncBackend protected helpers are inaccessible to
    # non-derived ordinary code.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="authority_completion_negative_compile",
        target="__script__:verify-completion-authority-negative-compile.sh",
        layer="authority",
        backends=(),
        notes="Ordinary code cannot publish/claim via Completion privates.",
    ),
    Evidence(
        evidence_id="authority_request_arena_negative_compile",
        target="__script__:verify-request-arena-negative-compile.sh",
        layer="authority",
        backends=(),
        notes="Ordinary code cannot mutate RequestSlot private fields.",
    ),
    Evidence(
        evidence_id="authority_async_identity_negative_compile",
        target="__script__:verify-async-identity-negative-compile.sh",
        layer="authority",
        backends=(),
        notes="Async identity private-setter authority (E16-C2).",
    ),
    Evidence(
        evidence_id="authority_external_backend_negative_compile",
        target="__script__:verify-external-backend-authority-negative-compile.sh",
        layer="authority",
        backends=(),
        notes="NEW (C1): AsyncBackend protected publish/claim helpers are "
              "inaccessible to non-derived ordinary code; the probe's positive "
              "control proves derived-class access to both helpers still "
              "compiles (guards against protected->private regression).",
    ),

    # -----------------------------------------------------------------------
    # Layer: external_admission (NOT conformance). The MinimalExternalBackend
    # is built ONLY from public headers and proves the extension surface admits
    # a legitimate subclass; it is NOT run through the shared observable-
    # semantics suite.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="external_backend_admission",
        target="external_backend_admission_test",
        layer="external_admission",
        backends=(),
        notes="Public-surface subclass compiles + is owned by AsyncIoContext; "
              "derived access to the protected publication helpers is proven "
              "at compile level by the authority probe's positive control. "
              "NOT a conformance witness.",
    ),
)


# ---------------------------------------------------------------------------
# Helpers (used by both the gate and the self-test)
# ---------------------------------------------------------------------------

def evidence_by_id(eid: str) -> Optional[Evidence]:
    for e in EVIDENCE:
        if e.evidence_id == eid:
            return e
    return None


def evidence_for_backend(backend_name: str) -> tuple[Evidence, ...]:
    """All IMPLEMENTED evidence records covering a given backend display name.

    Backend-agnostic records (backends == ()) apply to every backend.

    NOTE (Phase C2a): this returns ONLY implemented records, so it is the right
    helper for target selection, command execution, and the "existence of
    records" mandatory-layer coverage check. For verdict / report / known-gap
    display use applicable_evidence_for_backend() instead, which also includes
    not_implemented and not_applicable records so a known gap surfaces in the
    backend's own verdict (INCOMPLETE), not just in a global results dict.
    """
    return implemented_evidence_for_backend(backend_name)


def implemented_evidence_for_backend(backend_name: str) -> tuple[Evidence, ...]:
    """Only IMPLEMENTED records covering this backend.

    Used for: target selection, command execution, runtime PASS/FAIL collection,
    and the mandatory implemented-coverage (mandatory_layers_covered) check.
    """
    return tuple(
        e for e in EVIDENCE
        if e.status == STATUS_IMPLEMENTED
        and (not e.backends or backend_name in e.backends)
    )


def applicable_evidence_for_backend(backend_name: str) -> tuple[Evidence, ...]:
    """IMPLEMENTED + not_implemented + not_applicable records covering this
    backend.

    Used by the verdict and the per-backend report so a known gap (e.g. Uring's
    Phase-D capacity gap) surfaces as INCOMPLETE in the backend's OWN verdict,
    not just in a global results dict. Backend-agnostic records (backends == ())
    apply to every backend.
    """
    return tuple(
        e for e in EVIDENCE
        if e.status in (STATUS_IMPLEMENTED, STATUS_NOT_IMPLEMENTED,
                        STATUS_NOT_APPLICABLE)
        and (not e.backends or backend_name in e.backends)
    )


def mandatory_layers_covered(backend_name: str) -> set[str]:
    """Which MANDATORY_LAYERS_PER_BACKEND have at least one evidence record
    (regardless of run outcome) for the backend. Existence of records, not
    pass/fail — the gate fills pass/fail at run time."""
    covered: set[str] = set()
    for e in evidence_for_backend(backend_name):
        if e.layer in MANDATORY_LAYERS_PER_BACKEND:
            covered.add(e.layer)
    return covered


def backend_by_name(name: str) -> Optional[BackendEntry]:
    for b in BACKENDS:
        if b.name == name:
            return b
    return None


def script_targets() -> tuple[str, ...]:
    """Authority evidence targets that are shell scripts, not xmake targets."""
    return tuple(
        e.target for e in EVIDENCE
        if e.target.startswith("__script__:")
    )
