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
                                    # "" when the backend has no capacity seam
                                    # (Uring before Phase D) — the gap is the
                                    # not_implemented manifest record, never a
                                    # skip-as-pass.


# The C1 backend registry. Sync/Synthetic are intentionally absent.
BACKENDS: tuple[BackendEntry, ...] = (
    BackendEntry("Fake", "ReferenceProfile", "conformance_fake",
                 capacity_driver_case="conformance_capacity_fake"),
    BackendEntry("ThreadPool", "BlockingIoProfile", "conformance_threadpool",
                 capacity_driver_case="conformance_capacity_threadpool"),
    BackendEntry("Uring", "KernelIoProfile", "conformance_uring"),
)


# ---------------------------------------------------------------------------
# Evidence records
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Evidence:
    evidence_id: str                    # unique stable id
    target: str                         # xmake target name
    cases: Optional[tuple[str, ...]] = None   # SLUICE_TEST_CASE names if the
                                              # target is filtered by case.
    layer: str = "shared"               # one of LAYERS
    backends: tuple[str, ...] = ()      # which display backends this covers;
                                        # () means backend-agnostic (e.g.
                                        # arena lifecycle, authority probes).
    mandatory: bool = True              # must PASS for an ELIGIBLE verdict.
    status: str = STATUS_IMPLEMENTED
    reason: str = ""                    # required iff status == not_applicable.
    notes: str = ""


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
    # Phase C2a — shared capacity/admission/accounting conformance gap.
    # The capacity cases (shared_capacity_suite, IMPLEMENTED for Fake/ThreadPool)
    # land in commit 3 alongside the driver case + run_capacity_cases(). Uring
    # has NOT migrated onto RequestArena (Phase D pending), so its capacity
    # coverage is recorded here as a known not_implemented gap: a
    # not_implemented MANDATORY record enters the verdict via
    # applicable_evidence_for_backend() and forces the backend to INCOMPLETE,
    # never skip-as-pass. This record reinforces (does not replace) the existing
    # KernelIoProfile-stays-NOT-CONFORMING rule.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="uring_capacity_not_implemented",
        target="backend_conformance_test",
        layer="shared",
        backends=_U,
        status=STATUS_NOT_IMPLEMENTED,
        mandatory=True,
        notes="C2a: Uring has no RequestArena before Phase D; capacity "
              "rejection/admission/accounting conformance is not implemented. "
              "Recorded as a known gap; the driver does not execute the "
              "capacity cases for Uring. Uring stays NOT CONFORMING.",
    ),

    # -----------------------------------------------------------------------
    # Phase C2a — shared capacity/admission/accounting conformance (implemented
    # for backends that have migrated onto RequestArena: Fake, ThreadPool). The
    # capacity cases live in the SAME target as shared_suite
    # (backend_conformance_test) and are driven by run_capacity_cases(), built
    # via make_backend_with_capacity. They assert ONLY AsyncIoContext-observable
    # state. The gate drives this per-backend in isolated subprocesses
    # (conformance_capacity_fake / conformance_capacity_threadpool). Uring's gap
    # is the not_implemented record above.
    # -----------------------------------------------------------------------
    Evidence(
        evidence_id="shared_capacity_suite",
        target="backend_conformance_test",
        layer="shared",
        backends=("Fake", "ThreadPool"),
        notes="C2a capacity/admission/rejection/accounting cases: accepts "
              "exact capacity, (N+1)th rejects with would_block (rejected "
              "Completion stays idle; no async from a reject), exact stats "
              "split (submitted_ops committed-only; queue_full_retries vs "
              "invalid_state_rejections), max_outstanding <= capacity, "
              "recycle after cancel->reap->reset. Shared-observable only.",
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
        notes="Non-quiescent destruction fail-fast.",
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
        notes="C2b rows 3-8 require RequestArena identity/generation/"
              "cancel/reap integration; Uring remains legacy until "
              "Phase D. Recorded as a known gap; not_implemented never "
              "counts as PASS. (Target is the Uring-driving conformance "
              "binary, matching uring_capacity_not_implemented; the gap "
              "is not executed.)",
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
        notes="C2c rows 11-14 require RequestArena borrow/waiter/lease "
              "lifecycle integration; Uring remains legacy until Phase D. "
              "Recorded as a known gap; not_implemented never counts as "
              "PASS. (Target is the Uring-driving conformance binary, "
              "matching uring_c2b_identity_not_implemented; the gap is not "
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
