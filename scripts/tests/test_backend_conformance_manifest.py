# test_backend_conformance_manifest.py
#
# Phase C1 — pure-data self-tests for the backend conformance manifest and the
# aggregate gate's per-backend RESULT ATTRIBUTION logic.
#
# Two test concerns, both pure-data (no xmake, no subprocess, no binary):
#
#   1. Manifest-INTERNAL invariants (closed profiles/layers/statuses, unique
#      evidence ids, mandatory-section coverage, helper well-definedness). These
#      do NOT check whether a target exists in the build graph (that is the
#      aggregate GATE's preflight concern).
#
#   2. The aggregate gate's per-backend attribution logic
#      (scripts/verify-backend-conformance.py): prove that one backend's failure
#      cannot contaminate another backend's verdict. These exercise the gate's
#      PURE classification helpers (parse_meta, canonical backend-key mapping,
#      metadata validation, per-backend verdict computation) against fabricated
#      RunResult/meta inputs — never mocking away the subprocess result-attribution
#      logic itself, because that logic is exactly what is under test.
#
# Run with:
#   python3 -m unittest discover -v scripts/tests        # CI / hardening runner
#   python3 scripts/tests/test_backend_conformance_manifest.py
# Both invocations exit 0 on success, non-zero on any failure.
#
# IMPORTANT: this module MUST NOT execute sys.exit() at module top level. An
# earlier version called sys.exit(0) at import time, which `unittest discover`
# surfaced as a SystemExit -> _FailedTest -> ERROR -> FAILED (errors=1),
# breaking CI. See docs/architecture/phase-c1-conformance-gate.md.
"""Pure-data invariant + result-attribution tests for Phase C1."""

import importlib.util
import os
import re
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import backend_conformance_manifest as M  # noqa: E402

# The aggregate gate lives in a HYPHEN-named script
# (scripts/verify-backend-conformance.py) which is NOT importable as a normal
# module. Load it explicitly by path so the per-backend attribution logic is
# exercised directly (the same Python file the gate runs in production).
_GATE_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "verify-backend-conformance.py")
# Repository root (this file lives in <repo>/scripts/tests/).
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location("verify_backend_conformance", _GATE_PATH)
G = importlib.util.module_from_spec(_spec)
sys.modules["verify_backend_conformance"] = G
_spec.loader.exec_module(G)  # type: ignore[union-attr]


# ---------------------------------------------------------------------------
# Manifest-internal invariant tests
# ---------------------------------------------------------------------------

class ManifestClosedSetsTest(unittest.TestCase):
    """The manifest's closed sets never grow accidentally."""

    def test_profiles_is_the_closed_c1_set(self):
        self.assertEqual(set(M.PROFILES),
                         {"ReferenceProfile", "BlockingIoProfile",
                          "KernelIoProfile"})

    def test_layers_is_the_closed_c1_set(self):
        self.assertEqual(set(M.LAYERS),
                         {"shared", "lifecycle", "authority",
                          "backend_specific", "external_admission"})

    def test_status_constants_are_the_closed_set(self):
        self.assertEqual({M.STATUS_IMPLEMENTED, M.STATUS_NOT_APPLICABLE,
                          M.STATUS_NOT_IMPLEMENTED},
                         {"implemented", "not_applicable", "not_implemented"})

    def test_mandatory_layers_subset_of_layers(self):
        self.assertTrue(set(M.MANDATORY_LAYERS_PER_BACKEND) <= set(M.LAYERS))


class BackendRegistryTest(unittest.TestCase):
    """Backend registry: closed, unique, valid profiles, no nameless backends."""

    def test_backend_names_unique(self):
        names = [b.name for b in M.BACKENDS]
        self.assertEqual(len(names), len(set(names)),
                         f"backend names must be unique: {names}")

    def test_every_backend_references_a_known_profile(self):
        for b in M.BACKENDS:
            self.assertIn(b.profile, M.PROFILES,
                          f"backend {b.name} references unknown profile "
                          f"{b.profile}")

    def test_registered_backends_are_exactly_fake_threadpool_uring(self):
        # The closed profile mapping: no unregistered/nameless backend may
        # silently enter the result set.
        self.assertEqual([b.name for b in M.BACKENDS],
                         ["Fake", "ThreadPool", "Uring"])

    def test_profile_to_backend_mapping_is_closed(self):
        # ReferenceProfile -> Fake, BlockingIoProfile -> ThreadPool,
        # KernelIoProfile -> Uring.
        by_profile = {b.profile: b.name for b in M.BACKENDS}
        self.assertEqual(by_profile,
                         {"ReferenceProfile": "Fake",
                          "BlockingIoProfile": "ThreadPool",
                          "KernelIoProfile": "Uring"})


class EvidenceRecordsTest(unittest.TestCase):
    """Evidence records: unique ids, valid layers/statuses, known backends."""

    def test_evidence_ids_unique(self):
        ids = [e.evidence_id for e in M.EVIDENCE]
        self.assertEqual(len(ids), len(set(ids)),
                         f"evidence_id must be unique: {ids}")

    def test_every_evidence_layer_is_in_closed_set(self):
        for e in M.EVIDENCE:
            self.assertIn(e.layer, set(M.LAYERS),
                          f"{e.evidence_id}: bad layer {e.layer!r}")

    def test_every_evidence_status_is_in_closed_set(self):
        for e in M.EVIDENCE:
            self.assertIn(e.status,
                          {"implemented", "not_applicable",
                           "not_implemented"},
                          f"{e.evidence_id}: bad status {e.status!r}")

    def test_every_evidence_references_known_backend(self):
        known = {b.name for b in M.BACKENDS}
        for e in M.EVIDENCE:
            for b in e.backends:
                self.assertIn(b, known,
                              f"{e.evidence_id}: unknown backend {b!r}")

    def test_not_applicable_requires_a_reason(self):
        for e in M.EVIDENCE:
            if e.status == M.STATUS_NOT_APPLICABLE:
                self.assertTrue(e.reason,
                                f"{e.evidence_id}: not_applicable needs reason")

    def test_every_evidence_has_a_nonempty_target(self):
        for e in M.EVIDENCE:
            self.assertTrue(e.target, f"{e.evidence_id}: empty target")

    def test_required_evidence_modes_are_closed_and_unique(self):
        for e in M.EVIDENCE:
            self.assertEqual(len(e.required_modes), len(set(e.required_modes)),
                             f"{e.evidence_id}: duplicate required mode")
            self.assertTrue(set(e.required_modes) <= set(M.EVIDENCE_MODES),
                            f"{e.evidence_id}: unknown required mode")

    def test_pinned_evidence_cases_are_nonempty_unique_strings(self):
        # When an evidence record pins a `cases` set, that tuple is G2's
        # VERIFICATION authority (the gate's _drive() proves the binary ran
        # exactly these cases). The authority itself must therefore be
        # well-formed: non-empty, no duplicates, no empty/non-string element.
        # A malformed pin such as cases=("foo","foo") would otherwise let the
        # gate under-verify a target that only registered one distinct case.
        for e in M.EVIDENCE:
            if e.cases is None:
                continue
            self.assertIsInstance(e.cases, tuple,
                                  f"{e.evidence_id}: cases must be a tuple")
            self.assertGreater(len(e.cases), 0,
                               f"{e.evidence_id}: pinned cases must be non-empty")
            self.assertEqual(len(e.cases), len(set(e.cases)),
                             f"{e.evidence_id}: pinned cases must be unique: "
                             f"{list(e.cases)}")
            for case in e.cases:
                self.assertIsInstance(case, str,
                                      f"{e.evidence_id}: case name must be str")
                self.assertTrue(case,
                                f"{e.evidence_id}: case name must be non-empty")

    def test_not_implemented_never_counts_as_pass(self):
        # Behavioral: drive the gate with a not_implemented evidence record
        # present and assert the record is INCOMPLETE, which is never in the
        # SATISFACTORY set — a known gap can never satisfy a mandatory slot.
        gap = M.Evidence(
            evidence_id="phase_d_gap",
            target="phase_d_target",
            layer="backend_specific",
            backends=("Uring",),
            status=M.STATUS_NOT_IMPLEMENTED,
        )
        import contextlib
        import io
        buf = io.StringIO()
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, gap)), \
             mock.patch.object(G, "xmake_target_exists", return_value=False), \
             mock.patch.object(G, "run_shell_script", return_value=(0, "")), \
             contextlib.redirect_stdout(buf):
            g = G.Gate(args=None)
            g.run()
            self.assertEqual(g.results["phase_d_gap"].state, G.INCOMPLETE)
        self.assertNotIn(G.INCOMPLETE, G.SATISFACTORY)


class MandatoryCoverageTest(unittest.TestCase):
    """Every registered backend has evidence in each mandatory layer."""

    def test_every_backend_covers_all_mandatory_layers(self):
        for b in M.BACKENDS:
            covered = M.mandatory_layers_covered(b.name)
            missing = set(M.MANDATORY_LAYERS_PER_BACKEND) - covered
            self.assertFalse(
                missing,
                f"backend {b.name} missing mandatory-layer evidence: "
                f"{sorted(missing)}")

    def test_every_backend_has_applicable_shared_evidence(self):
        for b in M.BACKENDS:
            shared_applies = any(
                e.layer == "shared"
                and (not e.backends or b.name in e.backends)
                for e in M.evidence_for_backend(b.name))
            self.assertTrue(
                shared_applies,
                f"backend {b.name} has no applicable shared-suite evidence")


class HelpersTest(unittest.TestCase):
    """Pure-data helpers are well-defined."""

    def test_evidence_by_id_resolves_known(self):
        self.assertIsNotNone(M.evidence_by_id("shared_suite"))

    def test_evidence_by_id_returns_none_for_unknown(self):
        self.assertIsNone(M.evidence_by_id("does_not_exist_zzz"))

    def test_backend_by_name_resolves_known(self):
        self.assertIsNotNone(M.backend_by_name("Fake"))

    def test_backend_by_name_returns_none_for_unknown(self):
        self.assertIsNone(M.backend_by_name("Nope"))

    def test_script_targets_well_formed(self):
        targets = M.script_targets()
        self.assertGreaterEqual(len(targets), 1)
        for s in targets:
            self.assertTrue(s.startswith("__script__:"),
                            f"malformed script target: {s!r}")


# ---------------------------------------------------------------------------
# Aggregate-gate per-backend RESULT-ATTRIBUTION tests (the C1 corrective).
#
# These prove the defect from the task §4 matrix is closed: one backend's
# failure cannot contaminate another backend's verdict. They build a Gate with
# fabricated RunResults (no subprocess) and assert each backend's verdict is
# computed INDEPENDENTLY from its OWN run state, the stable [conformance-meta]
# lines, and the manifest — exactly the logic the gate runs in production.
# ---------------------------------------------------------------------------

REGISTERED = [b.name for b in M.BACKENDS]  # ["Fake", "ThreadPool", "Uring"]


def _stub_gate(shared_per_backend, *, shared_rc=None,
               extra_results=None, meta_override=None):
    """Build a Gate whose results table is pre-seeded per backend.

    `shared_per_backend` maps the shared-suite state per registered backend
    name, e.g. {"Fake": "PASS", "ThreadPool": "RUN_FAIL", ...}. The gate's
    per-backend verdict logic is then driven against this fabricated state
    without any subprocess.

    All non-shared IMPLEMENTED evidence records (lifecycle, backend_specific,
    authority, external_admission) default to PASS so that the ONLY variable
    under test is the shared-suite state — the layer the contamination defect
    lived in. `extra_results` overrides individual non-shared results.
    `meta_override` replaces the parsed [conformance-meta] table.
    """
    g = G.Gate(args=None)
    g.meta = meta_override if meta_override is not None else {
        "Fake": {"profile": "ReferenceProfile", "mode": "deterministic"},
        "ThreadPool": {"profile": "BlockingIoProfile", "mode": "real"},
        "Uring": {"profile": "KernelIoProfile", "mode": "stub"},
    }
    # Seed every non-shared IMPLEMENTED evidence to PASS by default.
    g.results = {}
    for ev in M.EVIDENCE:
        if ev.evidence_id == "shared_suite":
            continue
        if ev.status == M.STATUS_IMPLEMENTED:
            g.results[ev.evidence_id] = G.RunResult(
                ev.evidence_id, ev.target, G.PASS, detail="stub PASS")
        elif ev.status == M.STATUS_NOT_APPLICABLE:
            g.results[ev.evidence_id] = G.RunResult(
                ev.evidence_id, ev.target, G.NOT_APPLICABLE,
                detail=ev.reason or "not applicable")
    if extra_results:
        for k, v in extra_results.items():
            g.results[k] = v
    # Per-backend shared-suite state (the layer under test).
    g.shared_by_backend = {}
    for name, state in (shared_per_backend or {}).items():
        g.shared_by_backend[name] = G.RunResult(
            f"shared_suite:{name}", "backend_conformance_test", state,
            detail=f"stub {state}")
    # Phase C2a: per-backend shared CAPACITY-suite state. Stub KernelIo is
    # build/API evidence only, so its real-capacity result defaults to
    # INCOMPLETE; every other capacity-capable mode defaults to PASS.
    g.capacity_by_backend = {}
    for b in M.BACKENDS:
        if b.capacity_driver_case:
            mode = g.meta.get(b.name, {}).get("mode", "unknown")
            state = (G.INCOMPLETE if b.profile == "KernelIoProfile" and mode != "real"
                     else G.PASS)
            g.capacity_by_backend[b.name] = G.RunResult(
                f"shared_capacity_suite:{b.name}", "backend_conformance_test",
                state, detail=f"stub capacity {state}")
    # Phase C2e: per-backend shared CLOSE/DRAIN-suite state. Every backend
    # with a close_admission seam (Fake, ThreadPool, and — Phase D4 — Uring)
    # defaults to PASS so the ONLY variable under test is the shared-suite
    # state, EXCEPT a KernelIo close/drain run whose mode is not real: that is
    # build/API evidence only and defaults to INCOMPLETE (the D4 lift's mode
    # attribution, mirroring the capacity seeding below).
    g.close_drain_by_backend = {}
    for b in M.BACKENDS:
        if b.close_drain_driver_case:
            mode = g.meta.get(b.name, {}).get("mode", "unknown")
            state = (G.INCOMPLETE if b.profile == "KernelIoProfile" and mode != "real"
                     else G.PASS)
            g.close_drain_by_backend[b.name] = G.RunResult(
                f"c2e_shared_close_drain_suite:{b.name}",
                "backend_conformance_test", state,
                detail=f"stub close/drain {state}")
    return g


def _stubbed_gate_with_recorded_runs():
    """Build a Gate with the shared-suite drive methods mocked, run it, and
    return (gate, driven, shared_runs, capacity_runs, close_drain_runs).

    `_drive` / `_run_shared_suite` / `_run_capacity_suite` /
    `_run_close_drain_suite` are replaced by recorders that also seed benign
    per-backend PASS results so `_report()` stays valid. Gate.run()'s report
    output is suppressed with contextlib.redirect_stdout (same pattern as the
    nearby test_not_implemented_never_counts_as_pass), so the test runner does
    not print the full gate report.

    Shared by SharedCapacitySuiteDriveExclusionTest and
    CapacityResultAuthorityTest (PR #69 regression D/E).
    """
    import contextlib
    import io

    g = G.Gate(args=None)
    driven: list[str] = []
    shared_runs: list[str] = []
    capacity_runs: list[str] = []
    close_drain_runs: list[str] = []

    def fake_drive(ev):
        driven.append(ev.evidence_id)
        return G.RunResult(ev.evidence_id, ev.target, G.PASS,
                           detail="stub drive")

    def fake_shared(ev):
        shared_runs.append(ev.evidence_id)
        # Seed a benign PASS for every backend so _report() is valid.
        for b in M.BACKENDS:
            if b.driver_case:
                g.shared_by_backend[b.name] = G.RunResult(
                    f"{ev.evidence_id}:{b.name}", ev.target, G.PASS,
                    detail="stub shared")

    def fake_capacity(ev):
        capacity_runs.append(ev.evidence_id)
        for b in M.BACKENDS:
            if b.capacity_driver_case:
                g.capacity_by_backend[b.name] = G.RunResult(
                    f"{ev.evidence_id}:{b.name}", ev.target, G.PASS,
                    detail="stub capacity")

    def fake_close_drain(ev):
        close_drain_runs.append(ev.evidence_id)
        for b in M.BACKENDS:
            if b.close_drain_driver_case:
                g.close_drain_by_backend[b.name] = G.RunResult(
                    f"{ev.evidence_id}:{b.name}", ev.target, G.PASS,
                    detail="stub close/drain")

    with mock.patch.object(g, "_drive", side_effect=fake_drive), \
         mock.patch.object(g, "_run_shared_suite", side_effect=fake_shared), \
         mock.patch.object(g, "_run_capacity_suite",
                           side_effect=fake_capacity), \
         mock.patch.object(g, "_run_close_drain_suite",
                           side_effect=fake_close_drain), \
         contextlib.redirect_stdout(io.StringIO()):
        g.run()
    return g, driven, shared_runs, capacity_runs, close_drain_runs


class MetaParsingTest(unittest.TestCase):
    """[conformance-meta] parsing is the stable machine interface."""

    def test_parses_well_formed_meta(self):
        out = ("[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=deterministic\n"
               "[conformance-meta] backend=ThreadPool profile=BlockingIoProfile "
               "mode=real\n"
               "[conformance-meta] backend=Uring(stub) profile=KernelIoProfile "
               "mode=stub\n")
        meta = G.parse_meta_lines(out)
        self.assertEqual(meta["Fake"]["mode"], "deterministic")
        self.assertEqual(meta["ThreadPool"]["profile"], "BlockingIoProfile")
        self.assertEqual(meta["Uring(stub)"]["mode"], "stub")

    def test_ignores_non_meta_lines(self):
        out = ("[run] conformance_fake\n"
               "[conformance] skip Fake :: foo (non-real_mode)\n"
               "some human text\n")
        self.assertEqual(G.parse_meta_lines(out), {})

    def test_empty_output_yields_empty_meta(self):
        self.assertEqual(G.parse_meta_lines(""), {})

    def test_canonical_backend_key_maps_stub_variant(self):
        # "Uring(stub)" maps to the registered "Uring"; exact names map 1:1.
        self.assertEqual(
            G.canonical_backend_key("Uring(stub)", REGISTERED), "Uring")
        self.assertEqual(
            G.canonical_backend_key("Fake", REGISTERED), "Fake")
        self.assertIsNone(
            G.canonical_backend_key("Mystery", REGISTERED))


class EvidenceModeParsingTest(unittest.TestCase):
    """Real-only evidence has a separate fail-closed mode declaration."""

    def test_parses_exact_evidence_mode_line(self):
        out = ("noise\n[evidence-meta] evidence=uring_c2d_failure_injection "
               "mode=real\n")
        self.assertEqual(G.parse_evidence_meta_lines(out),
                         [("uring_c2d_failure_injection", "real")])

    def test_malformed_evidence_mode_line_is_not_parsed(self):
        self.assertEqual(G.parse_evidence_meta_lines(
            "[evidence-meta] evidence=uring_c2d_failure_injection\n"), [])

    def _d2_cases(self):
        # Single source of truth: read the pinned set from the manifest, never
        # duplicate the case names here. This couples the test's notion of the
        # required corpus to the manifest authority.
        return M.evidence_by_id("uring_c2d_failure_injection").cases

    def _d2_output(self, cases=None, mode=None, meta_lines=None):
        """Fabricate a target's stdout with the given [run] case-set and meta.

        ``cases`` defaults to the manifest's authoritative pinned set, so the
        positive path (exact 10-case set) is self-maintaining. ``mode`` emits a
        single matching [evidence-meta] line (None disables it). ``meta_lines``
        overrides the meta entirely (for duplicate / missing-meta mutants).
        """
        if cases is None:
            cases = self._d2_cases()
        run_lines = "".join(f"[run] {c}\n" for c in cases)
        if meta_lines is not None:
            return run_lines + meta_lines
        if mode is not None:
            return (run_lines + f"[evidence-meta] evidence="
                    f"uring_c2d_failure_injection mode={mode}\n")
        return run_lines

    def _drive_c2d(self, output, rc=0):
        ev = M.evidence_by_id("uring_c2d_failure_injection")
        gate = G.Gate(args=mock.Mock(no_build=True))
        with mock.patch.object(G, "xmake_target_exists", return_value=True), \
             mock.patch.object(G, "xmake_run_target", return_value=(rc, output)):
            return gate._drive(ev)

    def test_real_mode_is_pass(self):
        # Positive pin: the full pinned case-set runs and emits one real-mode
        # [evidence-meta] line. This exercises the complete PASS chain through
        # _drive: exit 0 -> exact case-set -> exact metadata.
        result = self._drive_c2d(self._d2_output(mode="real"))
        self.assertEqual(result.state, G.PASS)

    def test_stub_mode_is_incomplete_not_pass(self):
        # The case-set passes (full pinned set ran); the metadata then reports
        # mode=stub, which is not in required_modes -> INCOMPLETE.
        result = self._drive_c2d(self._d2_output(mode="stub"))
        self.assertEqual(result.state, G.INCOMPLETE)

    def test_missing_or_duplicate_mode_is_incomplete(self):
        # Full pinned case-set ran, but zero / duplicate [evidence-meta] lines.
        missing = self._drive_c2d(self._d2_output(meta_lines=""))
        duplicate = self._drive_c2d(self._d2_output(meta_lines=(
            "[evidence-meta] evidence=uring_c2d_failure_injection mode=real\n"
            "[evidence-meta] evidence=uring_c2d_failure_injection mode=real\n")))
        self.assertEqual(missing.state, G.INCOMPLETE)
        self.assertEqual(duplicate.state, G.INCOMPLETE)

    def test_nonzero_real_target_is_run_fail(self):
        # rc != 0 short-circuits to RUN_FAIL before any case-set or meta check.
        result = self._drive_c2d(self._d2_output(mode="real"), rc=1)
        self.assertEqual(result.state, G.RUN_FAIL)


class EvidenceCaseSetPinTest(unittest.TestCase):
    """The aggregate gate proves an ordinary evidence target's pinned case-set.

    Closes Issue #81 P1 G2. G1 (closed in the prior commit) owns the child
    environment so a hostile parent filter cannot shrink the executed set. G2
    is a DISTINCT invariant: even when the child runs unfiltered (G1 satisfied)
    the gate must still prove the binary executed the manifest's pinned
    required case-set — every case exactly once, nothing extra. Without this
    check a mutant that deletes nine load-bearing cases (leaving only the
    metadata-emitting case) exits 0, emits one [evidence-meta] line, and is
    misclassified PASS.

    Every mutation here traverses Gate._drive() end-to-end (not the helper in
    isolation), so a future regression that drops the classify_case_set call
    from _drive cannot slip through as a green pure-function test.
    """

    def setUp(self):
        self.ev = M.evidence_by_id("uring_c2d_failure_injection")
        self.required = list(self.ev.cases)
        # Sanity: the manifest actually pins a non-trivial set for this record.
        self.assertGreater(len(self.required), 1,
                           "precondition: D2 record pins a real case-set")

    def _drive(self, output, rc=0):
        gate = G.Gate(args=mock.Mock(no_build=True))
        with mock.patch.object(G, "xmake_target_exists", return_value=True), \
             mock.patch.object(G, "xmake_run_target", return_value=(rc, output)):
            return gate._drive(self.ev)

    def _out(self, cases, mode="real"):
        run = "".join(f"[run] {c}\n" for c in cases)
        return (run + f"[evidence-meta] evidence=uring_c2d_failure_injection "
                f"mode={mode}\n")

    def test_positive_exact_pinned_set_is_pass(self):
        # Exact 10-case set + real metadata -> PASS through the full chain.
        result = self._drive(self._out(self.required, mode="real"))
        self.assertEqual(result.state, G.PASS, result.detail)

    def test_g2a_remove_one_load_bearing_case_is_incomplete(self):
        # G2-A: delete exactly one load-bearing case. The binary no longer
        # proves the contract, yet without the case-set check its exit 0 + one
        # evidence-meta line would be misclassified PASS.
        mutant = list(self.required)
        # Drop a non-metadata load-bearing case (the second entry).
        removed = mutant.pop(1)
        result = self._drive(self._out(mutant, mode="real"))
        self.assertEqual(result.state, G.INCOMPLETE)
        self.assertIn(removed, result.detail,
                      "detail must name the missing case")

    def test_g2b_metadata_case_only_is_incomplete(self):
        # G2-B: the load-bearing detector. Only the metadata-emitting case
        # survives (e.g. a source-level deletion of the other nine cases).
        result = self._drive(self._out([self.required[0]], mode="real"))
        self.assertEqual(result.state, G.INCOMPLETE)
        # The detail must report every other required case as missing.
        for case in self.required[1:]:
            self.assertIn(case, result.detail,
                          f"detail must name missing case {case!r}")

    def test_g2c_unexpected_extra_case_is_incomplete(self):
        # G2-C: the pinned set ran, plus an unpinned/foreign case. The binary
        # executed work the manifest did not authorize.
        mutant = list(self.required) + ["a_case_the_manifest_did_not_pin"]
        result = self._drive(self._out(mutant, mode="real"))
        self.assertEqual(result.state, G.INCOMPLETE)
        self.assertIn("a_case_the_manifest_did_not_pin", result.detail,
                      "detail must name the unexpected case")

    def test_g2d_duplicate_keeps_missing_and_duplicate_in_detail(self):
        # G2-D: 9 distinct required cases run + one required case is repeated
        # (= 10 selected), so exactly one required case never ran. This mutant
        # has BOTH a missing case AND a duplicate. The diagnostic must report
        # BOTH honestly rather than forcing a single category.
        duplicated = self.required[1]
        never_ran = self.required[-1]
        mutant = [c for c in self.required if c != never_ran] + [duplicated]
        self.assertEqual(len(mutant), len(self.required))  # same length
        result = self._drive(self._out(mutant, mode="real"))
        self.assertEqual(result.state, G.INCOMPLETE)
        self.assertIn(never_ran, result.detail,
                      "detail must name the missing case")
        self.assertIn(duplicated, result.detail,
                      "detail must name the duplicated case")

    def test_classify_case_set_helper_is_order_insensitive(self):
        # The required set is a SET equivalence, not a registration-order
        # claim: any permutation of the exact set is PASS at the helper level.
        import random
        rng = random.Random(0)
        permuted = list(self.required)
        rng.shuffle(permuted)
        self.assertNotEqual(permuted, self.required)  # actually permuted
        state, _ = G.classify_case_set(permuted, self.ev.cases)
        self.assertEqual(state, G.PASS)

    def test_manifest_pin_matches_cpp_source_registration(self):
        # DRIFT detector (auxiliary, not authoritative): the manifest's pinned
        # set must match the case names actually registered as
        # SLUICE_TEST_CASE macros in the C++ target source. The anchor only
        # matches a real macro invocation at the start of a logical line, so a
        # comment like `// SLUICE_TEST_CASE(foo)` is NOT counted. The
        # authoritative check remains the runtime [run] set observed by _drive
        # (above); this test catches manifest/source drift before the gate runs.
        source_path = os.path.join(REPO_ROOT, "tests",
                                   "uring_d2_failure_noalloc_test.cpp")
        with open(source_path, "r", encoding="utf-8") as f:
            source = f.read()
        found = re.findall(
            r"^[ \t]*SLUICE_TEST_CASE\(([_A-Za-z][_A-Za-z0-9]*)\)",
            source, flags=re.MULTILINE)
        self.assertEqual(
            set(found), set(self.ev.cases),
            f"manifest pin {sorted(self.ev.cases)} does not match the case "
            f"names registered in {source_path}: {sorted(found)}")


class D3DriftDetectorTest(unittest.TestCase):
    """D3 (PR #80 G2 discipline): every dedicated D3 evidence target's pinned
    manifest case-set must equal its actual SLUICE_TEST_CASE registrations.

    One record per new dedicated target (uring_backend_c2b_identity_test /
    uring_backend_c2c_waiter_borrow_test), anchored macro regex, exact set
    equality. The runtime [run]-set check in _drive remains authoritative.
    """

    def _assert_source_matches_pin(self, evidence_id, source_name):
        ev = M.evidence_by_id(evidence_id)
        self.assertIsNotNone(ev.cases, f"{evidence_id} must pin a case-set")
        source_path = os.path.join(REPO_ROOT, "tests", source_name)
        with open(source_path, "r", encoding="utf-8") as f:
            source = f.read()
        found = re.findall(
            r"^[ \t]*SLUICE_TEST_CASE\(([_A-Za-z][_A-Za-z0-9]*)\)",
            source, flags=re.MULTILINE)
        self.assertEqual(
            set(found), set(ev.cases),
            f"manifest pin {sorted(ev.cases)} does not match the case names "
            f"registered in {source_path}: {sorted(found)}")
        # NOTE: no duplicate-registration assertion — these dual-mode targets
        # legitimately register every real case TWICE (the real body and the
        # stub-mode empty body), so the set-equality above is the meaningful
        # drift check.

    def test_c2b_pin_matches_source(self):
        self._assert_source_matches_pin(
            "uring_c2b_identity_integration", "uring_backend_c2b_identity_test.cpp")

    def test_c2c_pin_matches_source(self):
        self._assert_source_matches_pin(
            "uring_c2c_borrow_waiter_integration", "uring_backend_c2c_waiter_borrow_test.cpp")

    def test_c2e_pin_matches_source(self):
        self._assert_source_matches_pin(
            "uring_c2e_close_drain", "uring_backend_c2e_close_drain_test.cpp")


class D4DriftDetectorTest(unittest.TestCase):
    """D4 (PR #84 repair, P1-B/P0-C): every D4 multi-case Uring evidence target
    used in the KernelIo lift must have an exact pinned runtime case-set, and
    the manifest pin must equal the actual SLUICE_TEST_CASE registrations in
    the source (both builds register the same names — real bodies plus the
    stub-mode empty bodies — so set-equality is the meaningful drift check).
    """

    def _assert_source_matches_pin(self, evidence_id, source_name):
        ev = M.evidence_by_id(evidence_id)
        self.assertIsNotNone(ev.cases, f"{evidence_id} must pin a case-set")
        source_path = os.path.join(REPO_ROOT, "tests", source_name)
        with open(source_path, "r", encoding="utf-8") as f:
            source = f.read()
        found = re.findall(
            r"^[ \t]*SLUICE_TEST_CASE\(([_A-Za-z][_A-Za-z0-9]*)\)",
            source, flags=re.MULTILINE)
        self.assertEqual(
            set(found), set(ev.cases),
            f"manifest pin {sorted(ev.cases)} does not match the case names "
            f"registered in {source_path}: {sorted(found)}")

    def test_uring_backend_contract_pin_matches_source(self):
        self._assert_source_matches_pin(
            "uring_backend_contract", "uring_backend_test.cpp")

    def test_uring_quiescent_destruction_pin_matches_source(self):
        self._assert_source_matches_pin(
            "uring_c2e_quiescent_destruction", "uring_backend_c2e_death_test.cpp")

    def test_uring_close_drain_pin_matches_source(self):
        # P1-B drift closure: EVERY pinned multi-case D4 Uring record must
        # match its source registrations (both builds register the same names).
        # The uring_c2e_submit_races_close_linearization case was added after
        # the pin was first written; without this test the strict set-
        # equivalence gate classifies the record INCOMPLETE (unexpected case)
        # while the binary itself passes — a source<->manifest drift that the
        # mechanical check turns into a RED self-test instead of a late
        # aggregate-gate surprise.
        self._assert_source_matches_pin(
            "uring_c2e_close_drain", "uring_backend_c2e_close_drain_test.cpp")

    def test_quiescent_destruction_record_is_mandatory_and_real(self):
        # P0-C: the destruction evidence is a MANDATORY real-mode lifecycle
        # record (a mechanical gate input), not prose in another record's notes.
        ev = M.evidence_by_id("uring_c2e_quiescent_destruction")
        self.assertIsNotNone(ev)
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertEqual(ev.backends, ("Uring",))
        self.assertEqual(ev.required_modes, ("real",))
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        self.assertEqual(ev.target, "uring_backend_c2e_death_test")
        # The old unrelated stub-only skip case must NOT be part of the corpus.
        self.assertNotIn("uring_c2e_death_skip_non_posix_or_stub", ev.cases)
        # The corpus is the exact pinned set (evidence-mode + 8 semantic + the
        # D4-RM11 destructor-order probe).
        self.assertEqual(len(ev.cases), 10)
        self.assertEqual(ev.cases[0], "uring_d4_c2e_death_evidence_mode")

    def test_uring_backend_contract_record_is_pinned(self):
        # P1-B: the backend-contract record must be fully pinned (no unpinned
        # multi-case mandatory real-mode record in the lift).
        ev = M.evidence_by_id("uring_backend_contract")
        self.assertIsNotNone(ev)
        self.assertIsNotNone(ev.cases)
        self.assertGreaterEqual(len(ev.cases), 10)

    def test_wait_source_include_guarded_for_stub_public_header(self):
        # P1-D (D4-RM6 detector): the Uring STUB/OFF public header must not
        # pull Linux/POSIX-only wait-source headers (<poll.h>, <sys/eventfd.h>,
        # <unistd.h>). The include of uring_wait_source.hpp must sit INSIDE an
        # #if defined(SLUICE_HAS_LIBURING) guard. A mutant that restores the
        # unconditional include breaks the stub/off build on non-Linux POSIX
        # (macOS has no sys/eventfd.h) and this mechanical source check.
        p = os.path.join(REPO_ROOT, "include", "sluice", "async",
                         "uring_backend.hpp")
        with open(p, "r", encoding="utf-8") as f:
            src = f.read()
        lines = src.splitlines()
        idx = next(i for i, l in enumerate(lines)
                   if "uring_wait_source.hpp" in l)
        # Walk upward to the nearest enclosing #if directive.
        guard = None
        for j in range(idx - 1, -1, -1):
            if lines[j].lstrip().startswith("#if"):
                guard = lines[j]
                break
        self.assertIsNotNone(guard, "wait-source include has no enclosing #if")
        self.assertIn("SLUICE_HAS_LIBURING", guard,
                      "wait-source include must be inside the "
                      "SLUICE_HAS_LIBURING guard (stub public header portability; "
                      f"nearest guard is {guard!r})")

    def test_close_admission_uses_dispatch_mtx(self):
        # P0 (D4-RM8 revisited — honest close-vs-acceptance LP evidence): the
        # submit admission transaction (Stage 0..commit_binding) and
        # close_admission()'s admission-close write use the SAME dispatch_mtx_.
        # This source-level invariant is the deterministic authority for the
        # submit-vs-close linearization — a runtime "closer blocked on mutex"
        # observation cannot be made without scheduler timing (the test itself
        # serializes close after the submit LP). A mutant that removes
        # dispatch_mtx_ from close_admission (so arena_.close_admission() and
        # admission_closed_=true are written unlocked) turns this self-test RED
        # deterministically for EVERY build, independent of race timing. The
        # parser locates the close_admission() body and asserts both writes sit
        # inside a std::lock_guard<std::mutex> lk(dispatch_mtx_) critical
        # section in that function.
        p = os.path.join(REPO_ROOT, "src", "async", "uring_backend.cpp")
        with open(p, "r", encoding="utf-8") as f:
            src = f.read()
        # close_admission() has two definitions: a stub (no-liburing) body and
        # the real liburing body. Enumerate every definition's balanced-brace
        # body and locate the REAL one — the body that calls
        # arena_.close_admission() (the stub does not).
        bodies = []
        for m in re.finditer(
                r"void\s+UringAsyncBackend::close_admission\s*\(\s*\)\s*\{",
                src):
            start = src.index("{", m.start())
            depth = 0
            end = None
            for i in range(start, len(src)):
                if src[i] == "{":
                    depth += 1
                elif src[i] == "}":
                    depth -= 1
                    if depth == 0:
                        end = i + 1
                        break
            self.assertIsNotNone(end, "close_admission() body is unbalanced")
            bodies.append(src[start:end])
        self.assertTrue(bodies, "close_admission() definition not found")
        body = next((b for b in bodies if "arena_.close_admission()" in b),
                    None)
        self.assertIsNotNone(
            body,
            "no close_admission() body calls arena_.close_admission() "
            "(real-liburing definition missing?)")
        self.assertIn("admission_closed_ = true",
                      body,
                      "close_admission() must set admission_closed_ = true")
        self.assertIn("dispatch_mtx_",
                      body,
                      "close_admission() must acquire dispatch_mtx_")
        # Both writes must sit INSIDE a lock_guard(dispatch_mtx_) critical
        # section (not merely in the same function before/after a lock). The
        # lock_guard is declared at the top of its block, so the block's open
        # brace is the LAST '{' at or before the lock_guard text. Find that
        # open brace, then its matching close brace, and assert both writes
        # are within that critical section.
        lg_idx = body.find("std::lock_guard<std::mutex> lk(dispatch_mtx_)")
        self.assertGreater(lg_idx, -1,
                           "close_admission() has no lock_guard(dispatch_mtx_) "
                           "critical section (D4-RM8 mutant: shared lock removed)")
        cs_start = body.rfind("{", 0, lg_idx)
        self.assertGreater(cs_start, -1,
                           "lock_guard(dispatch_mtx_) has no enclosing block")
        depth = 0
        cs_end = None
        for i in range(cs_start, len(body)):
            if body[i] == "{":
                depth += 1
            elif body[i] == "}":
                depth -= 1
                if depth == 0:
                    cs_end = i
                    break
        self.assertIsNotNone(cs_end,
                             "close_admission() lock_guard block is unbalanced")
        cs = body[cs_start:cs_end + 1]
        self.assertIn("arena_.close_admission()", cs,
                      "arena_.close_admission() must be INSIDE the "
                      "dispatch_mtx_ critical section (D4-RM8 mutant)")
        self.assertIn("admission_closed_ = true", cs,
                      "admission_closed_ = true must be INSIDE the "
                      "dispatch_mtx_ critical section (D4-RM8 mutant)")


class D4EvidenceModeDriveTest(unittest.TestCase):
    """P1-A (PR #84 repair — the D3 R1/R3 defect reapplied to D4): the C2e
    evidence-mode metadata MUST exist in BOTH build modes. Before the repair
    the metadata case was compiled out in stub builds, so a stub run printed
    the full pinned [run] set MINUS one case and ZERO [evidence-meta] lines —
    the gate classified it INCOMPLETE for the WRONG reason (a missing required
    case), not the intended "disallowed mode".

    These tests drive the uring_c2e_close_drain evidence record end-to-end
    through Gate._drive() with fabricated target stdout, proving:
      * real mode (full pinned 16-case set + one mode=real meta)  -> PASS;
      * stub mode (full pinned set + one mode=stub meta)  -> INCOMPLETE by
        required_modes (NOT by a missing case);
      * full pinned set but ZERO [evidence-meta] lines     -> INCOMPLETE
        (missing metadata);
      * stub run missing exactly the evidence-mode case   -> INCOMPLETE
        (case-set mismatch) — the re-compile-out regression is still caught
        independently.
    """

    @staticmethod
    def _ev():
        ev = M.evidence_by_id("uring_c2e_close_drain")
        assert ev is not None and ev.cases, "c2e record must pin a case-set"
        return ev

    def _out(self, cases, mode=None, meta_lines=None):
        run = "".join(f"[run] {c}\n" for c in cases)
        if meta_lines is not None:
            return run + meta_lines
        if mode is not None:
            return run + f"[evidence-meta] evidence=uring_c2e_close_drain mode={mode}\n"
        return run

    def _drive(self, output, rc=0):
        gate = G.Gate(args=mock.Mock(no_build=True))
        with mock.patch.object(G, "xmake_target_exists", return_value=True), \
             mock.patch.object(G, "xmake_run_target", return_value=(rc, output)):
            return gate._drive(self._ev())

    def test_c2e_real_full_set_is_pass(self):
        ev = self._ev()
        result = self._drive(self._out(list(ev.cases), mode="real"))
        self.assertEqual(result.state, G.PASS, result.detail)

    def test_c2e_stub_full_set_incomplete_by_mode_not_case(self):
        ev = self._ev()
        result = self._drive(self._out(list(ev.cases), mode="stub"))
        self.assertEqual(result.state, G.INCOMPLETE, result.detail)
        self.assertIn("mode", result.detail)
        self.assertIn("required", result.detail)
        self.assertNotIn("case-set mismatch", result.detail)
        self.assertNotIn("missing=", result.detail)

    def test_c2e_full_set_missing_meta_incomplete(self):
        ev = self._ev()
        result = self._drive(self._out(list(ev.cases)))
        self.assertEqual(result.state, G.INCOMPLETE, result.detail)
        self.assertIn("evidence-meta", result.detail)

    def test_c2e_stub_missing_evidence_mode_case_is_case_set_incomplete(self):
        # The original D3 R1/R3 defect shape: the stub build compiled the
        # evidence-mode case out (truncated set, zero meta). The gate must
        # classify it as a CASE-SET mismatch, not as a pass.
        ev = self._ev()
        evidence_case = ev.cases[0]  # uring_d4_c2e_evidence_mode is pinned first
        truncated = [c for c in ev.cases if c != evidence_case]
        result = self._drive(self._out(truncated))
        self.assertEqual(result.state, G.INCOMPLETE, result.detail)
        self.assertIn(evidence_case, result.detail,
                      "detail must name the missing evidence-mode case")
        self.assertIn("case-set mismatch", result.detail)

    def test_c2e_death_evidence_mode_registered_in_both_builds(self):
        # P0-C: the death target's evidence-mode case must be registered
        # unconditionally in the source (the internal #if/#else picks the
        # emitted mode), mirroring the C2e close/drain target.
        source_path = os.path.join(REPO_ROOT, "tests",
                                   "uring_backend_c2e_death_test.cpp")
        with open(source_path, "r", encoding="utf-8") as f:
            source = f.read()
        # The case registration must sit OUTSIDE the liburing guard: find the
        # SLUICE_TEST_CASE line and prove the nearest enclosing #if/#else
        # structure around it contains an internal #if defined(SLUICE_HAS_LIBURING).
        lines = source.splitlines()
        idx = next(i for i, l in enumerate(lines)
                   if l.startswith("SLUICE_TEST_CASE(uring_d4_c2e_death_evidence_mode)"))
        # Inside the case body there must be a real/stub mode split.
        body = "\n".join(lines[idx:idx + 12])
        self.assertIn("mode=real", body)
        self.assertIn("mode=stub", body)
        self.assertIn("SLUICE_HAS_LIBURING", body)


class D3EvidenceModeDriveTest(unittest.TestCase):
    """D3 (PR #83 review R1/R3): the evidence-mode metadata MUST exist in BOTH
    build modes. Before the repair the evidence-mode case was compiled out in
    stub builds, so a stub run printed the full pinned [run] set MINUS one
    (10 for C2b, 13 for C2c) and ZERO [evidence-meta] lines — the gate
    classified it INCOMPLETE for the WRONG reason (a missing required case),
    not the intended "disallowed mode".

    These tests drive the evidence records end-to-end through Gate._drive()
    with fabricated target stdout, proving for BOTH D3 records:
      * real mode (full pinned set + one mode=real meta)  -> PASS;
      * stub mode (full pinned set + one mode=stub meta)  -> INCOMPLETE by
        required_modes (NOT by a missing case);
      * full pinned set but ZERO [evidence-meta] lines     -> INCOMPLETE
        (missing metadata), proving the G2 "exactly one [evidence-meta]
        line per gate-driven run" invariant is enforced at the gate;
      * stub run missing exactly the evidence-mode case   -> INCOMPLETE
        (case-set mismatch), proving a future regression that re-compiles
        the evidence-mode case out is still caught independently.

    Runtime observation is the compiled-binary execution authority; the
    source<->pin drift detector above is an auxiliary registration-name
    detector that cannot see preprocessor guards (the very hole this repair
    closed).
    """

    @staticmethod
    def _ev(evidence_id):
        ev = M.evidence_by_id(evidence_id)
        assert ev is not None and ev.cases, evidence_id
        return ev

    def _out(self, ev, cases, mode=None, meta_lines=None):
        run = "".join(f"[run] {c}\n" for c in cases)
        if meta_lines is not None:
            return run + meta_lines
        if mode is not None:
            return run + f"[evidence-meta] evidence={ev.evidence_id} mode={mode}\n"
        return run

    def _drive(self, ev, output, rc=0):
        gate = G.Gate(args=mock.Mock(no_build=True))
        with mock.patch.object(G, "xmake_target_exists", return_value=True), \
             mock.patch.object(G, "xmake_run_target", return_value=(rc, output)):
            return gate._drive(ev)

    def _assert_both_modes(self, evidence_id):
        ev = self._ev(evidence_id)
        pinned = list(ev.cases)

        # Real mode: full pinned set + mode=real -> PASS.
        real = self._drive(ev, self._out(ev, pinned, mode="real"))
        self.assertEqual(real.state, G.PASS, real.detail)

        # Stub mode: full pinned set + mode=stub -> INCOMPLETE solely because
        # mode=stub is not in required_modes=("real",), NOT a missing case.
        # The detail must be mode-centric ("mode=... not in required"), NOT a
        # case-set mismatch ("missing=...").
        stub = self._drive(ev, self._out(ev, pinned, mode="stub"))
        self.assertEqual(stub.state, G.INCOMPLETE, stub.detail)
        self.assertIn("mode", stub.detail)
        self.assertIn("required", stub.detail)
        self.assertNotIn("case-set mismatch", stub.detail)
        self.assertNotIn("missing=", stub.detail)

        # Missing [evidence-meta] entirely (full pinned set) -> INCOMPLETE by
        # metadata count, NOT by case-set (the G2 one-meta-per-run invariant).
        no_meta = self._drive(ev, self._out(ev, pinned))
        self.assertEqual(no_meta.state, G.INCOMPLETE, no_meta.detail)
        self.assertIn("evidence-meta", no_meta.detail)

        # Stub regression guard: a stub build that RE-COMPILES the
        # evidence-mode case out (the original bug) would run the full pinned
        # set MINUS the evidence-mode case with zero meta. The gate MUST
        # catch that as a case-set mismatch, independently of the meta check.
        evidence_case = pinned[0]  # the evidence-mode case is pinned first
        truncated = [c for c in pinned if c != evidence_case]
        regressed = self._drive(ev, self._out(ev, truncated))
        self.assertEqual(regressed.state, G.INCOMPLETE, regressed.detail)
        self.assertIn(evidence_case, regressed.detail,
                      "detail must name the missing evidence-mode case")

    def test_c2b_evidence_mode_present_in_both_modes(self):
        self._assert_both_modes("uring_c2b_identity_integration")

    def test_c2c_evidence_mode_present_in_both_modes(self):
        self._assert_both_modes("uring_c2c_borrow_waiter_integration")


class FailClosedMetadataTest(unittest.TestCase):
    """Missing/malformed metadata fails closed (NOT_RUN / unknown mode)."""

    def test_missing_meta_for_backend_is_unknown_mode(self):
        # No meta at all -> mode 'unknown' -> KernelIo NOT CONFORMING; for the
        # reference/blocking profiles the gate treats absence as not-seen.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
                       meta_override={})
        # KernelIo with unknown mode must be INCOMPLETE (never ELIGIBLE).
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)

    def test_unknown_mode_for_kernel_is_not_conforming(self):
        g = _stub_gate(
            {"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
            meta_override={"Uring": {"profile": "KernelIoProfile",
                                     "mode": "unknown"}})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)


class KernelIoAggregateFailClosedTest(unittest.TestCase):
    """P0-A (D4 repair): the AGGREGATE gate fails closed on a REAL KernelIo
    INCOMPLETE / NOT CONFORMING, tolerates ONLY a stub-mode INCOMPLETE, and
    fails closed on an unknown/missing mode.

    Every scenario (GATE-L1..L7) runs the real aggregate/report path —
    Gate._report() over a fabricated per-backend run state — and asserts the
    exit code the gate returns, NOT merely _backend_verdict() in isolation.
    The _stub_gate() fabricator seeds every non-shared IMPLEMENTED evidence
    PASS and per-backend shared/capacity/close-drain PASS (with the KernelIo
    mode-attribution downgrade), so the only variable under test is the
    evidence/verdict/mode combination each scenario names.
    """

    @staticmethod
    def _report_rc(g):
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            return g._report()

    @staticmethod
    def _real_gate(extra_results=None):
        """A fully-PASSing REAL KernelIo gate (mode=real, all evidence PASS)."""
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
                       meta_override={
                           "Fake": {"profile": "ReferenceProfile",
                                    "mode": "deterministic"},
                           "ThreadPool": {"profile": "BlockingIoProfile",
                                          "mode": "real"},
                           "Uring": {"profile": "KernelIoProfile",
                                     "mode": "real"},
                       })
        for k, v in (extra_results or {}).items():
            g.results[k] = v
        return g

    def test_gate_l1_real_all_mandatory_pass_returns_zero(self):
        # GATE-L1: real mode + every mandatory evidence PASS -> KernelIo
        # ELIGIBLE and the aggregate exits 0.
        g = self._real_gate()
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.ELIGIBLE, reasons)
        self.assertEqual(self._report_rc(g), 0)

    def test_gate_l2_real_close_drain_incomplete_returns_nonzero(self):
        # GATE-L2: real mode + uring_c2e_close_drain INCOMPLETE (e.g. a
        # missing pinned case) -> KernelIo INCOMPLETE and the aggregate exits
        # non-zero. Before the P0-A repair the KernelIo exemption let this
        # return 0 (the false-green this repair closes).
        ev = M.evidence_by_id("uring_c2e_close_drain")
        g = self._real_gate(extra_results={
            ev.evidence_id: G.RunResult(
                ev.evidence_id, ev.target, G.INCOMPLETE,
                detail="evidence case-set mismatch: "
                       "missing=uring_c2e_multiple_parked_waiters_all_wake, "
                       "unexpected=[], duplicate=[]")})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertNotEqual(self._report_rc(g), 0)

    def test_gate_l3_real_backend_contract_incomplete_returns_nonzero(self):
        # GATE-L3: real mode + uring_backend_contract INCOMPLETE -> KernelIo
        # INCOMPLETE and the aggregate exits non-zero.
        ev = M.evidence_by_id("uring_backend_contract")
        g = self._real_gate(extra_results={
            ev.evidence_id: G.RunResult(
                ev.evidence_id, ev.target, G.INCOMPLETE,
                detail="evidence case-set mismatch: "
                       "missing=uring_stats_increment_on_real_path, "
                       "unexpected=[], duplicate=[]")})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertNotEqual(self._report_rc(g), 0)

    def test_gate_l4_real_required_c2b_case_missing_returns_nonzero(self):
        # GATE-L4: real mode + a required pinned C2b case missing. The G2
        # case-set machinery itself classifies the truncated run INCOMPLETE
        # (driven end-to-end through _drive, not a hand-built state), and the
        # aggregate then fails. (C2c/C2e share the identical mechanism; the
        # representative C2b drives the path.)
        ev = M.evidence_by_id("uring_c2b_identity_integration")
        self.assertIsNotNone(ev)
        truncated = [c for c in ev.cases if c != ev.cases[1]]
        out = "".join(f"[run] {c}\n" for c in truncated)
        out += f"[evidence-meta] evidence={ev.evidence_id} mode=real\n"
        gate = G.Gate(args=mock.Mock(no_build=True))
        with mock.patch.object(G, "xmake_target_exists", return_value=True), \
             mock.patch.object(G, "xmake_run_target", return_value=(0, out)):
            result = gate._drive(ev)
        self.assertEqual(result.state, G.INCOMPLETE)
        self.assertIn("case-set mismatch", result.detail)
        g = self._real_gate(extra_results={ev.evidence_id: result})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertNotEqual(self._report_rc(g), 0)

    def test_gate_l5_real_wrong_missing_evidence_meta_returns_nonzero(self):
        # GATE-L5: real mode + a mandatory detector emits zero/wrong
        # [evidence-meta] lines. _drive() classifies the run INCOMPLETE
        # (metadata-count failure), and the aggregate exits non-zero.
        ev = M.evidence_by_id("uring_c2e_close_drain")
        out = "".join(f"[run] {c}\n" for c in ev.cases)  # no meta line at all
        gate = G.Gate(args=mock.Mock(no_build=True))
        with mock.patch.object(G, "xmake_target_exists", return_value=True), \
             mock.patch.object(G, "xmake_run_target", return_value=(0, out)):
            result = gate._drive(ev)
        self.assertEqual(result.state, G.INCOMPLETE)
        self.assertIn("evidence-meta", result.detail)
        g = self._real_gate(extra_results={ev.evidence_id: result})
        self.assertNotEqual(self._report_rc(g), 0)

    def test_gate_l6_stub_incomplete_returns_zero_never_eligible(self):
        # GATE-L6: stub mode + real-only evidence INCOMPLETE -> the aggregate
        # may PASS (build/API-only honesty), KernelIo is INCOMPLETE and can
        # never be ELIGIBLE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertNotEqual(verdict, G.ELIGIBLE)
        self.assertEqual(self._report_rc(g), 0)

    def test_gate_l7_unknown_kernel_mode_fails_closed(self):
        # GATE-L7: an unknown/missing KernelIo mode fails closed — the
        # aggregate exits non-zero because only a STUB-mode INCOMPLETE is
        # tolerable, and 'unknown' / missing is neither 'stub' nor 'real'.
        g = _stub_gate(
            {"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
            meta_override={"Uring": {"profile": "KernelIoProfile",
                                     "mode": "unknown"}})
        self.assertNotEqual(self._report_rc(g), 0)
        g2 = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
                        meta_override={})
        self.assertNotEqual(self._report_rc(g2), 0)

    def test_gate_stub_not_conforming_still_fails(self):
        # STUB KernelIo NOT CONFORMING (a mandatory evidence proved a
        # violation — a genuinely broken stub build) must fail the aggregate
        # too; only stub INCOMPLETE is tolerated.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "RUN_FAIL"})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertNotEqual(self._report_rc(g), 0)

    def test_gate_real_not_conforming_fails(self):
        # REAL KernelIo NOT CONFORMING -> aggregate FAIL.
        g = self._real_gate(extra_results={
            "uring_c2e_close_drain": G.RunResult(
                "uring_c2e_close_drain", "uring_backend_c2e_close_drain_test",
                G.RUN_FAIL, detail="exit 1")})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertNotEqual(self._report_rc(g), 0)


class ResultAttributionIsolationTest(unittest.TestCase):
    """The core corrective: a backend failure never contaminates the others."""

    def test_threadpool_failure_does_not_make_fake_run_fail(self):
        # Case 1 of task §4: ThreadPool shared-suite fails; Fake reports from
        # ITS OWN evidence (ELIGIBLE); Uring reports its Phase-D gap.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "RUN_FAIL",
                        "Uring": "PASS"})
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        v_uring, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(v_fake, G.ELIGIBLE,
                         "Fake must report ELIGIBLE from its own evidence")
        self.assertEqual(v_tp, G.NOT_CONFORMING,
                         "ThreadPool must report its own failure")
        self.assertEqual(v_uring, G.INCOMPLETE,
                         "stub-mode Uring must be INCOMPLETE (mode attribution)")
        # Crucially: Fake is NOT dragged to RUN_FAIL by ThreadPool's failure.
        self.assertNotEqual(v_fake, G.NOT_CONFORMING)

    def test_fake_failure_does_not_contaminate_threadpool(self):
        # Case 2 of task §4.
        g = _stub_gate({"Fake": "RUN_FAIL", "ThreadPool": "PASS",
                        "Uring": "PASS"})
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.NOT_CONFORMING)
        self.assertEqual(v_tp, G.ELIGIBLE,
                         "ThreadPool must stay ELIGIBLE when Fake fails")

    def test_uring_driver_failure_does_not_contaminate_others(self):
        # Case 3 of task §4: even if the Uring driver subprocess crashes, Fake
        # and ThreadPool report from their own evidence.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                        "Uring": "RUN_FAIL"})
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.ELIGIBLE)
        self.assertEqual(v_tp, G.ELIGIBLE)

    def test_all_pass_fake_and_tp_eligible_uring_stub_incomplete(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                        "Uring": "PASS"})
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("Fake"))[0], G.ELIGIBLE)
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("ThreadPool"))[0], G.ELIGIBLE)
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("Uring"))[0], G.INCOMPLETE)


class ClosedProfileMappingTest(unittest.TestCase):
    """Case 9/10 of task §4: closed profile manifest, no nameless backends."""

    def test_no_unregistered_backend_can_be_eligible(self):
        # Behavioral: an unregistered backend name cannot produce an ELIGIBLE
        # verdict even when every applicable evidence record passes. (The
        # registry CONTENT itself is asserted in BackendRegistryTest.)
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        ghost = M.BackendEntry("Ghost", "ReferenceProfile", "conformance_ghost")
        verdict, _ = g._backend_verdict(ghost)
        self.assertNotEqual(verdict, G.ELIGIBLE)

    def test_kernel_profile_stub_or_undriven_never_eligible(self):
        # After the D4 lift the KernelIo verdict comes from the ordinary
        # machinery with mode attribution: a stub/undriven shared-suite run is
        # INCOMPLETE (never ELIGIBLE); a RUN_FAIL is NOT CONFORMING.
        for state in ("PASS", "NOT_RUN"):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": state})
            verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
            self.assertEqual(verdict, G.INCOMPLETE,
                             f"Uring must be INCOMPLETE even if shared={state}")
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "RUN_FAIL"})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)


class KernelIoFailClosedOverrideTest(unittest.TestCase):
    """P1-B (D4-lift semantics): required_modes alone is NOT KernelIo
    per-backend override authority.

    required_modes declares WHICH mode a target must execute in; it does not
    declare that this evidence belongs to a given backend. After the D4 lift:

      * a backend-agnostic record (backends == ()) is SHARED-CONTRACT
        evidence — mode-independent, it reports its own run state (Uring
        carries the RequestArena contract through its D1 migration);
      * ONLY a real-mode record explicitly tagged to exactly this backend
        (ev.backends == (backend_name,)) may report its own PASS as the
        backend's phase record;
      * a MULTI-BACKEND-tagged lifecycle record (e.g. backends == ("Fake",
        "Uring")) can never lift Uring's per-backend obligation.
    """

    def test_backend_agnostic_contract_record_reports_its_own_state(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        generic = M.Evidence(
            evidence_id="generic_lifecycle_contract",
            target="some_lifecycle_target",
            layer="lifecycle",
            backends=(),                    # backend-agnostic contract record
        )
        g.results[generic.evidence_id] = G.RunResult(
            generic.evidence_id, generic.target, G.PASS, detail="stub PASS")
        state = g._backend_run_state(generic, "Uring", "KernelIoProfile")
        self.assertEqual(state, G.PASS,
                         "backend-agnostic contract records report their own "
                         "state (shared contract, mode-independent)")

    def test_multi_backend_tagged_cannot_lift_uring_per_backend_obligation(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        multi = M.Evidence(
            evidence_id="multi_backend_lifecycle",
            target="some_lifecycle_target",
            layer="lifecycle",
            backends=("Fake", "Uring"),     # NOT exactly this backend
            required_modes=("real",),
        )
        g.results[multi.evidence_id] = G.RunResult(
            multi.evidence_id, multi.target, G.PASS, detail="real PASS")
        state = g._backend_run_state(multi, "Uring", "KernelIoProfile")
        self.assertEqual(state, G.INCOMPLETE,
                         "multi-backend-tagged records must NOT report PASS "
                         "for Uring (P1-B)")

    def test_multi_backend_tagged_required_modes_cannot_lift_uring(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        shared_real = M.Evidence(
            evidence_id="multi_backend_required_real",
            target="some_lifecycle_target",
            layer="lifecycle",
            backends=("Uring", "ThreadPool"),
            required_modes=("real",),
        )
        g.results[shared_real.evidence_id] = G.RunResult(
            shared_real.evidence_id, shared_real.target, G.PASS,
            detail="stub PASS")
        state = g._backend_run_state(shared_real, "Uring", "KernelIoProfile")
        self.assertEqual(state, G.INCOMPLETE,
                         "a multi-backend tagged record cannot lift the "
                         "KernelIo fail-closed default either")

    def test_exactly_tagged_real_record_still_reports_its_own_pass(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        ev = M.evidence_by_id("uring_c2d_failure_injection")
        self.assertIsNotNone(ev)
        # The manifest must keep the real-mode phase record tagged to exactly
        # Uring; otherwise this test's premise (and the C2d PASS) breaks.
        self.assertEqual(ev.backends, ("Uring",))
        self.assertEqual(g._backend_run_state(ev, "Uring", "KernelIoProfile"),
                         G.PASS)


class SubprocessOutcomeMappingTest(unittest.TestCase):
    """Cases 8 of task §4: subprocess non-zero / timeout map deterministically."""

    def test_nonzero_subprocess_is_run_fail(self):
        self.assertEqual(G._state_for_rc(0), G.PASS)
        self.assertEqual(G._state_for_rc(1), G.RUN_FAIL)
        self.assertEqual(G._state_for_rc(2), G.RUN_FAIL)

    def test_timeout_subprocess_is_run_fail(self):
        self.assertEqual(G._state_for_rc(124), G.RUN_FAIL)


# ---------------------------------------------------------------------------
# Issue #81 P1 — isolate conformance evidence from ambient test filters.
#
# The conformance gate MUST own the execution environment of its evidence
# targets. Before the fix, xmake_run_target() passed env=None to subprocess.run
# for ordinary (non-shared) evidence, which makes the child INHERIT the parent
# os.environ verbatim. An ambient SLUICE_TEST_FILTER (e.g. a developer shell
# exporting SLUICE_TEST_FILTER=uring_d2_evidence_mode) would then silently
# reduce a 10-case D2 target to its single metadata case -> one evidence-meta
# line -> exit 0 -> _drive() classifies PASS. That is a false-conformance
# path. These tests capture the EXACT env dict handed to subprocess.run and
# prove: (1) ordinary evidence strips the ambient filter; (2) the shared
# per-backend suite sets the EXACT driver case on top of the cleaned env so a
# hostile parent value cannot leak; (3) the no-filter normal case is unchanged;
# (4) the sanitizer is narrow (PATH and unrelated foreign vars survive).
# ---------------------------------------------------------------------------

class EvidenceEnvIsolationTest(unittest.TestCase):
    """xmake_run_target gives every evidence subprocess an explicit, owned env."""

    def setUp(self):
        # Snapshot and clear any real ambient filter so each test controls its
        # own starting parent environment deterministically.
        self._saved_filter = os.environ.pop("SLUICE_TEST_FILTER", None)
        # A foreign, benign variable used to prove the sanitizer is narrow.
        self._foreign_name = "SLUICE_PROBE_KEEP_G1"
        self._foreign_val = "sentinel-g1"
        os.environ[self._foreign_name] = self._foreign_val
        # Record subprocess.run calls (do not actually exec xmake).
        self._real_run = subprocess.run
        self.captured_env = {}
        self.captured_cmd = {}
        outer = self

        def fake_run(cmd, *args, **kwargs):
            outer.captured_env["last"] = kwargs.get("env")
            outer.captured_cmd["last"] = cmd

            class _R:
                returncode = 0
                stdout = ""
                stderr = ""

            return _R()

        subprocess.run = fake_run

    def tearDown(self):
        subprocess.run = self._real_run
        os.environ.pop(self._foreign_name, None)
        if self._saved_filter is not None:
            os.environ["SLUICE_TEST_FILTER"] = self._saved_filter

    def test_ordinary_evidence_always_gets_explicit_env(self):
        # Regression for the P1 root cause: env=None meant 'inherit os.environ'.
        G.xmake_run_target("uring_d2_failure_noalloc_test")
        env = self.captured_env["last"]
        self.assertIsNotNone(
            env, "ordinary evidence must receive an EXPLICIT env dict, not "
            "env=None (which inherits the parent's ambient test filters)")

    def test_ambient_filter_stripped_for_ordinary_evidence(self):
        # The load-bearing counterexample from Issue #81 P1: a hostile parent
        # exports SLUICE_TEST_FILTER=uring_d2_evidence_mode; the 10-case D2
        # target must NOT inherit it, or it runs only its metadata case.
        os.environ["SLUICE_TEST_FILTER"] = "uring_d2_evidence_mode"
        G.xmake_run_target("uring_d2_failure_noalloc_test")
        env = self.captured_env["last"]
        self.assertNotIn(
            "SLUICE_TEST_FILTER", env,
            "ambient SLUICE_TEST_FILTER leaked into ordinary evidence env")

    def test_shared_suite_owns_exact_filter_no_parent_leak(self):
        # Shared per-backend runs set the EXACT driver case on top of a cleaned
        # env. A hostile parent value must not leak through.
        os.environ["SLUICE_TEST_FILTER"] = "some_unrelated_case"
        G.xmake_run_target("backend_conformance_test",
                           env_filter="conformance_uring")
        env = self.captured_env["last"]
        self.assertIsNotNone(env)
        self.assertEqual(
            env.get("SLUICE_TEST_FILTER"), "conformance_uring",
            "shared suite must set the EXACT driver case; parent value leaked")
        self.assertNotEqual(
            env.get("SLUICE_TEST_FILTER"), "some_unrelated_case")

    def test_no_ambient_filter_ordinary_behavior_unchanged(self):
        # Without an ambient filter, ordinary evidence behavior is unchanged:
        # explicit env, no filter key present, exit-0 path still classified PASS.
        self.assertNotIn("SLUICE_TEST_FILTER", os.environ)
        G.xmake_run_target("uring_d2_failure_noalloc_test")
        env = self.captured_env["last"]
        self.assertIsNotNone(env)
        self.assertNotIn("SLUICE_TEST_FILTER", env)
        # The classifier still maps an exit-0 ordinary run to PASS.
        self.assertEqual(G._state_for_rc(0), G.PASS)

    def test_sanitizer_is_narrow_foreign_env_remains(self):
        # Prove the sanitizer is narrow: PATH and an unrelated synthetic
        # variable must survive, so evidence subprocesses do not run under an
        # accidentally-empty/minimal environment.
        os.environ["SLUICE_TEST_FILTER"] = "uring_d2_evidence_mode"
        G.xmake_run_target("uring_d2_failure_noalloc_test")
        env = self.captured_env["last"]
        self.assertIn("PATH", env, "PATH must be preserved by clean_test_env")
        self.assertEqual(
            env.get(self._foreign_name), self._foreign_val,
            f"unrelated variable {self._foreign_name} must survive the narrow "
            f"sanitizer (only test-selection variables are stripped)")

    def test_selection_var_set_is_exactly_sluice_test_filter(self):
        # Guard against silently widening the sanitizer. tests/harness.hpp is
        # the authority for case selection and honors exactly SLUICE_TEST_FILTER.
        self.assertEqual(
            set(G.TEST_SELECTION_ENV_VARS), {"SLUICE_TEST_FILTER"},
            "TEST_SELECTION_ENV_VARS widened; only SLUICE_TEST_FILTER alters "
            "registered case selection (see tests/harness.hpp)")


class ProfileModesTest(unittest.TestCase):
    """Closed mode vocabulary per profile (P1 meta validation input)."""

    def test_profile_modes_keys_are_exactly_profiles(self):
        self.assertEqual(set(M.PROFILE_MODES), set(M.PROFILES))

    def test_modes_are_the_closed_vocabulary(self):
        all_modes = set().union(*(set(v) for v in M.PROFILE_MODES.values()))
        self.assertEqual(all_modes, {"deterministic", "real", "stub"})

    def test_reference_profile_allows_deterministic_only(self):
        self.assertEqual(set(M.PROFILE_MODES["ReferenceProfile"]),
                         {"deterministic"})

    def test_blocking_io_profile_allows_real_only(self):
        self.assertEqual(set(M.PROFILE_MODES["BlockingIoProfile"]), {"real"})

    def test_kernel_io_profile_allows_real_or_stub(self):
        self.assertEqual(set(M.PROFILE_MODES["KernelIoProfile"]),
                         {"real", "stub"})

    def test_every_backend_has_nonempty_driver_case(self):
        for b in M.BACKENDS:
            self.assertTrue(b.driver_case,
                            f"{b.name}: driver_case must be set")


# ---------------------------------------------------------------------------
# P1 corrective: the isolated shared-suite run is fail-closed.
#
# A per-backend shared run counts as PASS only when it provably executed
# exactly the manifest driver_case and emitted exactly one [conformance-meta]
# line declaring the manifest profile and a profile-allowed mode. Anything
# else (zero/extra selected cases, missing/duplicate/foreign meta, wrong
# backend/profile/mode) is INCOMPLETE — never PASS. These tests drive the
# gate's pure classification helper with fabricated subprocess outputs.
# ---------------------------------------------------------------------------

class SharedRunFailClosedTest(unittest.TestCase):
    """_classify_shared_run: PASS only with provable, exactly-one coverage."""

    GOOD_FAKE = (
        "[run] conformance_fake\n"
        "[conformance-meta] backend=Fake profile=ReferenceProfile "
        "mode=deterministic\n")

    def _classify(self, backend="Fake", rc=0, out=""):
        g = G.Gate(args=None)
        return g._classify_shared_run(M.backend_by_name(backend), rc, out)

    def test_valid_run_is_pass(self):
        state, detail = self._classify(out=self.GOOD_FAKE)
        self.assertEqual(state, G.PASS, detail)

    def test_nonzero_rc_is_run_fail(self):
        state, detail = self._classify(rc=1, out=self.GOOD_FAKE)
        self.assertEqual(state, G.RUN_FAIL, detail)

    def test_driver_case_typo_selects_zero_cases_is_incomplete(self):
        # A typo'd / renamed driver_case runs zero cases: no [run] line, no
        # meta. Previously this produced "ALL TESTS PASSED" -> exit 0 -> PASS.
        state, detail = self._classify(out="")
        self.assertEqual(state, G.INCOMPLETE, detail)
        self.assertIn("selected", detail)

    def test_filter_matches_multiple_cases_is_incomplete(self):
        out = ("[run] conformance_fake\n[run] conformance_threadpool\n"
               "[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=deterministic\n")
        state, detail = self._classify(out=out)
        self.assertEqual(state, G.INCOMPLETE, detail)

    def test_missing_conformance_meta_is_incomplete(self):
        out = ("[run] conformance_fake\n"
               "[conformance] skip Fake :: foo (non-real_mode)\n")
        state, detail = self._classify(out=out)
        self.assertEqual(state, G.INCOMPLETE, detail)
        self.assertIn("meta", detail)

    def test_wrong_backend_meta_is_incomplete(self):
        out = ("[run] conformance_fake\n"
               "[conformance-meta] backend=ThreadPool "
               "profile=BlockingIoProfile mode=real\n")
        state, detail = self._classify(out=out)
        self.assertEqual(state, G.INCOMPLETE, detail)
        self.assertIn("backend", detail)

    def test_wrong_profile_meta_is_incomplete(self):
        out = ("[run] conformance_fake\n"
               "[conformance-meta] backend=Fake profile=BlockingIoProfile "
               "mode=deterministic\n")
        state, detail = self._classify(out=out)
        self.assertEqual(state, G.INCOMPLETE, detail)
        self.assertIn("profile", detail)

    def test_wrong_mode_meta_is_incomplete(self):
        out = ("[run] conformance_fake\n"
               "[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=real\n")
        state, detail = self._classify(out=out)
        self.assertEqual(state, G.INCOMPLETE, detail)
        self.assertIn("mode", detail)

    def test_duplicate_meta_lines_is_incomplete(self):
        out = ("[run] conformance_fake\n"
               "[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=deterministic\n"
               "[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=deterministic\n")
        state, detail = self._classify(out=out)
        self.assertEqual(state, G.INCOMPLETE, detail)
        self.assertIn("meta", detail)

    def test_foreign_backend_meta_alongside_own_is_incomplete(self):
        # Evaluating ONE filtered backend while the output carries meta for
        # every backend (Fake, ThreadPool, Uring) must be INCOMPLETE — the
        # run provably executed more than the expected backend's case.
        out = ("[run] conformance_fake\n"
               "[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=deterministic\n"
               "[conformance-meta] backend=ThreadPool "
               "profile=BlockingIoProfile mode=real\n"
               "[conformance-meta] backend=Uring(stub) profile=KernelIoProfile "
               "mode=stub\n")
        state, detail = self._classify(out=out)
        self.assertEqual(state, G.INCOMPLETE, detail)

    def test_uring_stub_meta_canonicalizes_to_registered_backend(self):
        out = ("[run] conformance_uring\n"
               "[conformance-meta] backend=Uring(stub) profile=KernelIoProfile "
               "mode=stub\n")
        state, detail = self._classify(backend="Uring", out=out)
        self.assertEqual(state, G.PASS, detail)

    def test_threadpool_real_meta_is_pass(self):
        out = ("[run] conformance_threadpool\n"
               "[conformance-meta] backend=ThreadPool "
               "profile=BlockingIoProfile mode=real\n")
        state, detail = self._classify(backend="ThreadPool", out=out)
        self.assertEqual(state, G.PASS, detail)

    def test_capacity_case_passes_with_capacity_driver_case(self):
        # Phase C2a: a capacity-suite run is classified against the CAPACITY
        # driver case (conformance_capacity_fake), not the base shared-suite
        # case (conformance_fake). Passing expected_case=conformance_capacity_fake
        # must make a run that selected exactly that case PASS.
        out = ("[run] conformance_capacity_fake\n"
               "[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=deterministic\n")
        g = G.Gate(args=None)
        state, detail = g._classify_shared_run(
            M.backend_by_name("Fake"), 0, out,
            expected_case="conformance_capacity_fake")
        self.assertEqual(state, G.PASS, detail)

    def test_capacity_case_fails_without_expected_case(self):
        # Regression guard: WITHOUT passing expected_case, the capacity run
        # selected conformance_capacity_fake but the classifier expects the base
        # driver_case (conformance_fake) — must be INCOMPLETE, never PASS. This
        # pins the "capacity runs are classified by their own case name" wiring.
        out = ("[run] conformance_capacity_fake\n"
               "[conformance-meta] backend=Fake profile=ReferenceProfile "
               "mode=deterministic\n")
        g = G.Gate(args=None)
        state, detail = g._classify_shared_run(
            M.backend_by_name("Fake"), 0, out)
        self.assertEqual(state, G.INCOMPLETE, detail)


# ---------------------------------------------------------------------------
# P2 corrective: verdicts distinguish proven violation from missing evidence.
#
# Priority: any applicable MANDATORY evidence RUN_FAIL => NOT_CONFORMING;
# else any MISSING_TARGET / BUILD_FAIL / NOT_RUN / INCOMPLETE => INCOMPLETE;
# else all mandatory evidence PASS / legal NOT_APPLICABLE and every mandatory
# layer actually covered => ELIGIBLE. Non-mandatory evidence is diagnostic
# only: it can neither satisfy a mandatory layer nor block ELIGIBLE.
# ---------------------------------------------------------------------------

class MandatoryVerdictPriorityTest(unittest.TestCase):
    """_backend_verdict: one PASS per layer is NOT enough."""

    def test_layer_one_pass_one_missing_target_is_incomplete(self):
        # Same mandatory layer: one target PASS, one MISSING_TARGET. The old
        # state machine saw PASS in the layer set and returned ELIGIBLE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["arena_capacity_generation_release"] = G.RunResult(
            "arena_capacity_generation_release", "request_arena_test",
            G.MISSING_TARGET,
            detail="xmake show -t reports not a valid target")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertTrue(
            any("arena_capacity_generation_release" in r for r in reasons))

    def test_run_fail_beats_incomplete(self):
        # A proven violation outranks missing evidence: NOT_CONFORMING.
        g = _stub_gate({"Fake": "RUN_FAIL", "ThreadPool": "PASS",
                        "Uring": "PASS"})
        g.results["arena_capacity_generation_release"] = G.RunResult(
            "arena_capacity_generation_release", "request_arena_test",
            G.MISSING_TARGET, detail="missing")
        verdict, _ = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(verdict, G.NOT_CONFORMING)

    def test_missing_target_in_other_layer_is_incomplete(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["threadpool_phase_e_contract"] = G.RunResult(
            "threadpool_phase_e_contract", "threadpool_backend_phase_e_test",
            G.NOT_RUN, detail="not evaluated")
        verdict, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(verdict, G.INCOMPLETE)

    def test_all_mandatory_pass_is_eligible(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, _ = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(verdict, G.ELIGIBLE)
        verdict, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(verdict, G.ELIGIBLE)

    def test_uncovered_mandatory_layer_is_incomplete(self):
        # A backend with no evidence in a mandatory layer must be INCOMPLETE
        # (insufficient evidence), NOT NOT_CONFORMING (proven violation) and
        # never ELIGIBLE. "Ghost" is not registered: only backend-agnostic
        # lifecycle evidence applies, so "shared" and "backend_specific"
        # layers have no records.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        ghost = M.BackendEntry("Ghost", "ReferenceProfile", "conformance_ghost")
        verdict, reasons = g._backend_verdict(ghost)
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertTrue(any("layer" in r for r in reasons))

    def test_non_mandatory_evidence_is_diagnostic_only(self):
        # A mandatory=False record must not satisfy a mandatory layer nor
        # block ELIGIBLE when it fails.
        diag = M.Evidence(
            evidence_id="diagnostic_probe",
            target="diagnostic_probe_test",
            layer="lifecycle",
            backends=("Fake",),
            mandatory=False,
            notes="diagnostic only",
        )
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, diag)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            g.results["diagnostic_probe"] = G.RunResult(
                "diagnostic_probe", "diagnostic_probe_test",
                G.RUN_FAIL, detail="diagnostic failure")
            verdict, reasons = g._backend_verdict(M.backend_by_name("Fake"))
            self.assertEqual(verdict, G.ELIGIBLE, reasons)
            # ...but a mandatory record failing in the same layer still
            # produces INCOMPLETE (the diagnostic cannot rescue it).
            g2 = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                             "Uring": "PASS"})
            g2.results["diagnostic_probe"] = G.RunResult(
                "diagnostic_probe", "diagnostic_probe_test",
                G.PASS, detail="diagnostic pass")
            g2.results["arena_capacity_generation_release"] = G.RunResult(
                "arena_capacity_generation_release", "request_arena_test",
                G.MISSING_TARGET, detail="missing")
            verdict, _ = g2._backend_verdict(M.backend_by_name("Fake"))
            self.assertEqual(verdict, G.INCOMPLETE)


# ---------------------------------------------------------------------------
# Phase C2a corrective: not_implemented mandatory evidence enters the verdict.
#
# The prior gate iterated only implemented evidence in _backend_verdict, so a
# not_implemented mandatory record landed in self.results as INCOMPLETE but was
# invisible to the per-backend evidence set and the verdict. The C2a fix splits
# the helper into implemented_evidence_for_backend / applicable_evidence_for_
# backend and has the verdict iterate APPLICABLE evidence so a known gap forces
# INCOMPLETE in the backend's OWN verdict (not just a global results dict).
# ---------------------------------------------------------------------------

class ApplicableEvidenceHelpersTest(unittest.TestCase):
    """implemented_evidence_for_backend vs applicable_evidence_for_backend."""

    def test_implemented_excludes_not_implemented_and_not_applicable(self):
        # After D3/D4 no Uring not_implemented record remains; use a synthetic
        # gap to pin the helper split (implemented vs applicable).
        synth = M.Evidence(
            evidence_id="synthetic_uring_gap", target="t", layer="lifecycle",
            backends=("Uring",), mandatory=True, status=M.STATUS_NOT_IMPLEMENTED)
        impl = M.implemented_evidence_for_backend("Uring")
        ids = {e.evidence_id for e in impl}
        self.assertNotIn("synthetic_uring_gap", ids)
        # D4-closed: every mandatory Uring lifecycle record is implemented.
        for eid in ("uring_c2b_identity_integration",
                    "uring_c2c_borrow_waiter_integration",
                    "uring_c2e_close_drain"):
            self.assertIn(eid, ids, f"{eid} must be implemented after D3/D4")

    def test_applicable_includes_uring_tagged_evidence(self):
        # The Uring-tagged records (backends=("Uring",)) are in Uring's
        # applicable set — the set the verdict iterates.
        appl = M.applicable_evidence_for_backend("Uring")
        ids = {e.evidence_id for e in appl}
        for eid in ("uring_c2b_identity_integration",
                    "uring_c2c_borrow_waiter_integration",
                    "uring_c2e_close_drain"):
            self.assertIn(eid, ids)

    def test_applicable_excludes_uring_tagged_for_other_backend(self):
        # The Uring-tagged records MUST NOT apply to Fake or ThreadPool.
        for name in ("Fake", "ThreadPool"):
            appl = M.applicable_evidence_for_backend(name)
            ids = {e.evidence_id for e in appl}
            for eid in ("uring_c2b_identity_integration",
                        "uring_c2c_borrow_waiter_integration",
                        "uring_c2e_close_drain"):
                self.assertNotIn(eid, ids,
                                 f"{name} must not see the Uring-tagged {eid}")

    def test_applicable_includes_backend_agnostic_records(self):
        # Backend-agnostic records (backends == ()) apply to every backend in
        # BOTH helpers.
        for name in ("Fake", "ThreadPool", "Uring"):
            appl = M.applicable_evidence_for_backend(name)
            ids = {e.evidence_id for e in appl}
            self.assertIn("arena_capacity_generation_release", ids,
                          f"{name} must see backend-agnostic arena evidence")

    def test_evidence_for_backend_alias_equals_implemented(self):
        # The public alias must equal implemented_evidence_for_backend exactly.
        self.assertEqual(
            M.evidence_for_backend("Fake"),
            M.implemented_evidence_for_backend("Fake"))


class NotImplementedEntersVerdictTest(unittest.TestCase):
    """A not_implemented MANDATORY record forces a backend verdict INCOMPLETE.

    This is the C2a-corrective test the prior suite lacked: it proves the gap
    record enters the VERDICT (not just the global results dict).
    """

    def test_not_implemented_mandatory_forces_incomplete_verdict(self):
        # Inject a not_implemented MANDATORY record tagged for Fake. Fake's
        # verdict MUST become INCOMPLETE — proving the record entered the
        # verdict, not just the global results dict. (Fake is otherwise ELIGIBLE
        # in the stub gate.)
        gap = M.Evidence(
            evidence_id="fake_capacity_gap_injected",
            target="backend_conformance_test", layer="shared",
            backends=("Fake",), status=M.STATUS_NOT_IMPLEMENTED, mandatory=True,
        )
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, gap)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            # Seed the gap's results entry the way the run loop does.
            g.results["fake_capacity_gap_injected"] = G.RunResult(
                "fake_capacity_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            verdict, reasons = g._backend_verdict(M.backend_by_name("Fake"))
            self.assertEqual(verdict, G.INCOMPLETE, reasons)
            self.assertTrue(
                any("fake_capacity_gap_injected" in r for r in reasons),
                f"gap record must appear in reasons: {reasons}")

    def test_not_implemented_does_not_block_other_backends(self):
        # A gap tagged for Fake MUST NOT affect ThreadPool's verdict.
        gap = M.Evidence(
            evidence_id="fake_capacity_gap_injected",
            target="backend_conformance_test", layer="shared",
            backends=("Fake",), status=M.STATUS_NOT_IMPLEMENTED, mandatory=True,
        )
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, gap)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            g.results["fake_capacity_gap_injected"] = G.RunResult(
                "fake_capacity_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
            self.assertEqual(v_tp, G.ELIGIBLE,
                             "ThreadPool must not be affected by Fake's gap")

    def test_uring_capacity_gap_record_is_reconciled_away(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertIsNone(M.evidence_by_id("uring_capacity_not_implemented"))
        self.assertFalse(any("uring_capacity_not_implemented" in r for r in reasons))

    def test_not_implemented_never_counts_as_pass_in_verdict(self):
        # A not_implemented record can never satisfy a mandatory slot. Even
        # though it seeds INCOMPLETE (which is not RUN_FAIL), the verdict logic
        # treats INCOMPLETE as insufficient -> INCOMPLETE — never ELIGIBLE and
        # never NOT_CONFORMING (no proven violation). Assert the exact verdict,
        # not just "not ELIGIBLE".
        gap = M.Evidence(
            evidence_id="tp_only_gap_injected",
            target="backend_conformance_test", layer="shared",
            backends=("ThreadPool",), status=M.STATUS_NOT_IMPLEMENTED,
            mandatory=True,
        )
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, gap)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            g.results["tp_only_gap_injected"] = G.RunResult(
                "tp_only_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            verdict, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
            self.assertEqual(verdict, G.INCOMPLETE)


# ---------------------------------------------------------------------------
# Phase C2a: shared_capacity_suite is driven per-backend in isolated
# subprocesses, and the verdict reads each backend's OWN capacity result.
# Uring is driven too after D1; stub mode is classified INCOMPLETE rather than
# turning its build/API branch into a real-capacity PASS.
# ---------------------------------------------------------------------------

class CapacitySuiteDriverCaseTest(unittest.TestCase):
    """Every migrated backend has one isolated capacity driver case."""

    def test_all_registered_backends_have_capacity_driver_case(self):
        for name in ("Fake", "ThreadPool", "Uring"):
            b = M.backend_by_name(name)
            self.assertTrue(b.capacity_driver_case,
                            f"{name} must have a capacity_driver_case")

    def test_capacity_driver_cases_are_unique(self):
        cases = [b.capacity_driver_case for b in M.BACKENDS
                 if b.capacity_driver_case]
        self.assertEqual(len(cases), len(set(cases)),
                         f"capacity driver cases must be unique: {cases}")


class CapacitySuiteAttributionTest(unittest.TestCase):
    """The capacity suite is driven per-backend; one failure doesn't propagate."""

    def _stub_capacity(self, cap_per_backend):
        """Build a gate with per-backend capacity-suite state."""
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.capacity_by_backend = {}
        for name, state in cap_per_backend.items():
            g.capacity_by_backend[name] = G.RunResult(
                f"shared_capacity_suite:{name}", "backend_conformance_test",
                state, detail=f"stub {state}")
        return g

    def test_threadpool_capacity_fail_does_not_make_fake_ineligible(self):
        g = self._stub_capacity({"Fake": "PASS", "ThreadPool": "RUN_FAIL"})
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, reasons = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.ELIGIBLE,
                         "Fake must stay ELIGIBLE from its own capacity evidence")
        self.assertEqual(v_tp, G.NOT_CONFORMING,
                         "ThreadPool must report its own capacity failure")
        self.assertTrue(any("shared_capacity_suite" in r for r in reasons))

    def test_fake_capacity_fail_does_not_make_threadpool_ineligible(self):
        g = self._stub_capacity({"Fake": "RUN_FAIL", "ThreadPool": "PASS"})
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.NOT_CONFORMING)
        self.assertEqual(v_tp, G.ELIGIBLE)

    def test_uring_stub_capacity_is_incomplete_not_pass(self):
        g = self._stub_capacity(
            {"Fake": "PASS", "ThreadPool": "PASS", "Uring": "INCOMPLETE"})
        state = g._backend_run_state(M.evidence_by_id("shared_capacity_suite"),
                                     "Uring", "KernelIoProfile")
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(state, G.INCOMPLETE)
        self.assertEqual(verdict, G.INCOMPLETE)

    def test_shared_capacity_suite_evidence_tagged_for_all_backends(self):
        appl_fake = M.applicable_evidence_for_backend("Fake")
        appl_tp = M.applicable_evidence_for_backend("ThreadPool")
        appl_uring = M.applicable_evidence_for_backend("Uring")
        ids_fake = {e.evidence_id for e in appl_fake}
        ids_tp = {e.evidence_id for e in appl_tp}
        ids_uring = {e.evidence_id for e in appl_uring}
        self.assertIn("shared_capacity_suite", ids_fake)
        self.assertIn("shared_capacity_suite", ids_tp)
        self.assertIn("shared_capacity_suite", ids_uring)


# ---------------------------------------------------------------------------
# PR #69 review-finding regression: shared_capacity_suite MUST NOT enter the
# generic `_drive()` loop (which runs the WHOLE backend_conformance_test
# unfiltered), and the per-backend capacity runs MUST go through
# `_run_capacity_suite()`. A previous version only excluded `shared_suite`,
# so the IMPLEMENTED `shared_capacity_suite` was driven once unfiltered via
# `_drive()` AND THEN again per-backend via `_run_capacity_suite()` — a
# double-drive that contradicts the per-backend-subprocess isolation design
# and leaves a `results["shared_capacity_suite"]` entry with no consumer.
# ---------------------------------------------------------------------------

class SharedCapacitySuiteDriveExclusionTest(unittest.TestCase):
    """_drive() never receives shared_suite or shared_capacity_suite; the
    per-backend runs happen through _run_shared_suite / _run_capacity_suite."""

    def _run_gate_observing_drives(self):
        """Run the stubbed gate and return the recorded evidence lists. The
        gate construction + run + stdout suppression live in the module-level
        _stubbed_gate_with_recorded_runs helper (shared with
        CapacityResultAuthorityTest)."""
        _, driven, shared_runs, capacity_runs, _ = \
            _stubbed_gate_with_recorded_runs()
        return driven, shared_runs, capacity_runs

    def test_drive_never_receives_shared_suite(self):
        driven, _, _ = self._run_gate_observing_drives()
        self.assertNotIn("shared_suite", driven,
                         f"shared_suite must not enter _drive(): {driven}")

    def test_drive_never_receives_shared_capacity_suite(self):
        # The core PR #69 finding: shared_capacity_suite is IMPLEMENTED, so
        # without the explicit exclusion it WAS driven once unfiltered via
        # _drive(). After the fix it must be driven ONLY per-backend.
        driven, _, _ = self._run_gate_observing_drives()
        self.assertNotIn("shared_capacity_suite", driven,
                         f"shared_capacity_suite must not enter _drive(): "
                         f"{driven}")

    def test_run_shared_suite_executed_once(self):
        _, shared_runs, _ = self._run_gate_observing_drives()
        self.assertEqual(shared_runs, ["shared_suite"],
                         f"_run_shared_suite must run exactly once: "
                         f"{shared_runs}")

    def test_run_capacity_suite_executed_once(self):
        # Regression D: the capacity suite's ONLY runtime execution path must
        # be _run_capacity_suite(). It must be called exactly once (it loops
        # over backends internally), not driven via _drive().
        _, _, capacity_runs = self._run_gate_observing_drives()
        self.assertEqual(capacity_runs, ["shared_capacity_suite"],
                         f"_run_capacity_suite must run exactly once: "
                         f"{capacity_runs}")

    def test_no_results_entry_for_shared_capacity_suite(self):
        # The generic _drive() loop would populate
        # results["shared_capacity_suite"]; the per-backend path populates
        # capacity_by_backend instead. After the fix the generic results dict
        # must NOT carry a shared_capacity_suite key.
        g, _, _, _, _ = _stubbed_gate_with_recorded_runs()
        self.assertNotIn(
            "shared_capacity_suite", g.results,
            "shared_capacity_suite must not get a generic results[] entry; "
            "its verdict is read from capacity_by_backend")


class CapacityResultAuthorityTest(unittest.TestCase):
    """Regression E: every capacity verdict reads that backend's OWN
    capacity result from capacity_by_backend; the generic
    results['shared_capacity_suite'] does not exist."""

    def test_fake_verdict_reads_its_own_capacity_state(self):
        # Fake capacity RUN_FAIL must make Fake NOT CONFORMING even when every
        # other evidence (including the shared base suite) PASSES.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.capacity_by_backend["Fake"] = G.RunResult(
            "shared_capacity_suite:Fake", "backend_conformance_test",
            G.RUN_FAIL, detail="stub capacity fail")
        v, reasons = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(v, G.NOT_CONFORMING)
        self.assertTrue(any("shared_capacity_suite" in r for r in reasons),
                        f"capacity failure must surface in reasons: {reasons}")

    def test_generic_results_capacity_key_absent_does_not_break_verdict(self):
        # After the fix the generic run() loop does NOT populate
        # results["shared_capacity_suite"] (it is driven per-backend via
        # _run_capacity_suite into capacity_by_backend). Drive a fresh Gate.run()
        # with _drive / _run_shared_suite / _run_capacity_suite mocked so no
        # subprocess runs, then assert the generic key is absent and the
        # Fake/ThreadPool verdicts are ELIGIBLE from their own per-backend
        # capacity results. Reuses the module-level
        # _stubbed_gate_with_recorded_runs (same stubs as
        # SharedCapacitySuiteDriveExclusionTest).
        g, _, _, _, _ = _stubbed_gate_with_recorded_runs()
        self.assertNotIn(
            "shared_capacity_suite", g.results,
            "shared_capacity_suite must not get a generic results[] entry; "
            "its verdict is read from capacity_by_backend")
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("Fake"))[0], G.ELIGIBLE)
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("ThreadPool"))[0], G.ELIGIBLE)

    def test_uring_capacity_state_comes_from_its_isolated_result(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        self.assertIn("Uring", g.capacity_by_backend)
        self.assertEqual(g.capacity_by_backend["Uring"].state, G.INCOMPLETE)
        state = g._backend_run_state(M.evidence_by_id("shared_capacity_suite"),
                                     "Uring", "KernelIoProfile")
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(state, G.INCOMPLETE)
        self.assertEqual(verdict, G.INCOMPLETE)


# ---------------------------------------------------------------------------
# Phase C2b corrective: generation / stale-key / cancel-winner / identity
# evidence enters the per-backend verdict.
#
# The C2b slice adds four evidence records: a backend-agnostic arena matrix,
# Fake-tagged and ThreadPool-tagged integration records (both mandatory +
# implemented), and a Uring-tagged not_implemented record. The tests below
# prove:
#   * each mandatory C2b record exists with the correct layer, backends,
#     status, and mandatory flag;
#   * Fake/ThreadPool C2b integration is mandatory and implemented;
#   * Uring's C2b gap is mandatory and not_implemented (never PASS);
#   * a C2b RUN_FAIL on one backend does not contaminate the other;
#   * the arena-level PASS cannot erase Uring's not_implemented C2b gap;
#   * a missing C2b target fails closed (MISSING_TARGET -> INCOMPLETE);
#   * the C2b records appear in the lifecycle layer for the tagged backend.
# ---------------------------------------------------------------------------

C2B_EVIDENCE_IDS = (
    "c2b_arena_state_identity_matrix",
    "c2b_fake_identity_integration",
    "c2b_threadpool_identity_integration",
    "uring_c2b_identity_integration",
)


class C2bEvidenceRecordTest(unittest.TestCase):
    """C2b evidence records exist with correct attributes."""

    def test_all_c2b_records_exist(self):
        for eid in C2B_EVIDENCE_IDS:
            self.assertIsNotNone(M.evidence_by_id(eid),
                                 f"C2b evidence '{eid}' must exist in manifest")

    def test_arena_matrix_is_backend_agnostic(self):
        ev = M.evidence_by_id("c2b_arena_state_identity_matrix")
        self.assertEqual(ev.backends, ())
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_fake_integration_is_mandatory_and_tagged(self):
        ev = M.evidence_by_id("c2b_fake_identity_integration")
        self.assertIn("Fake", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_threadpool_integration_is_mandatory_and_tagged(self):
        ev = M.evidence_by_id("c2b_threadpool_identity_integration")
        self.assertIn("ThreadPool", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_uring_c2b_integration_is_implemented_real_mode(self):
        # D3 closure: the Uring C2b record is now an implemented, real-mode-
        # only, case-pinned evidence record (stub execution can never PASS it).
        ev = M.evidence_by_id("uring_c2b_identity_integration")
        self.assertEqual(ev.backends, ("Uring",))
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        self.assertEqual(ev.required_modes, ("real",))
        self.assertTrue(ev.cases and len(ev.cases) > 1)

    def test_c2b_ids_unique_in_manifest(self):
        ids = [e.evidence_id for e in M.EVIDENCE]
        for eid in C2B_EVIDENCE_IDS:
            self.assertEqual(ids.count(eid), 1,
                             f"C2b evidence '{eid}' must appear exactly once")


class C2bVerdictIntegrationTest(unittest.TestCase):
    """C2b evidence enters the per-backend verdict correctly."""

    def test_fake_has_mandatory_c2b_lifecycle_evidence(self):
        appl = M.applicable_evidence_for_backend("Fake")
        c2b = [e for e in appl if e.evidence_id in C2B_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2b),
                        "Fake must have at least one mandatory implemented "
                        "C2b lifecycle record")

    def test_threadpool_has_mandatory_c2b_lifecycle_evidence(self):
        appl = M.applicable_evidence_for_backend("ThreadPool")
        c2b = [e for e in appl if e.evidence_id in C2B_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2b),
                        "ThreadPool must have at least one mandatory "
                        "implemented C2b lifecycle record")

    def test_uring_has_mandatory_implemented_c2b_record(self):
        appl = M.applicable_evidence_for_backend("Uring")
        c2b = [e for e in appl if e.evidence_id in C2B_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2b),
                        "Uring must have a mandatory implemented C2b record")

    def test_uring_stub_mode_never_eligible(self):
        # D4 lift: the fail-closed hard-code is gone; mode attribution now
        # drives the stub verdict — the stub gate's Uring meta is mode=stub,
        # so the real-mode obligations (capacity/close-drain suites) stay
        # INCOMPLETE and the verdict is INCOMPLETE, never ELIGIBLE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE,
                         f"stub-mode Uring must be INCOMPLETE, never ELIGIBLE: {reasons}")

    def test_fake_c2b_failure_does_not_affect_threadpool(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2b_fake_identity_integration"] = G.RunResult(
            "c2b_fake_identity_integration", "backend_scheme_b_race_test",
            G.RUN_FAIL, detail="stub C2b Fake failure")
        v_fake, reasons_fake = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.NOT_CONFORMING)
        self.assertEqual(v_tp, G.ELIGIBLE,
                         "ThreadPool must stay ELIGIBLE when Fake C2b fails")
        self.assertTrue(
            any("c2b_fake_identity_integration" in r for r in reasons_fake))

    def test_threadpool_c2b_failure_does_not_affect_fake(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2b_threadpool_identity_integration"] = G.RunResult(
            "c2b_threadpool_identity_integration",
            "threadpool_backend_scheme_b_race_test",
            G.RUN_FAIL, detail="stub C2b ThreadPool failure")
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, reasons_tp = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.ELIGIBLE,
                         "Fake must stay ELIGIBLE when ThreadPool C2b fails")
        self.assertEqual(v_tp, G.NOT_CONFORMING)
        self.assertTrue(
            any("c2b_threadpool_identity_integration" in r for r in reasons_tp))

    def test_arena_pass_cannot_erase_uring_c2e_gap(self):
        # The arena matrix record is backend-agnostic (backends=()); it applies
        # to Uring but cannot satisfy Uring's own tagged obligation because the
        # tagged not_implemented C2e record remains INCOMPLETE (D4 work).
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2b_arena_state_identity_matrix"] = G.RunResult(
            "c2b_arena_state_identity_matrix",
            "request_lifecycle_scheme_b_test",
            G.PASS, detail="stub arena PASS")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)

    def test_missing_c2b_target_fails_closed(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2b_fake_identity_integration"] = G.RunResult(
            "c2b_fake_identity_integration", "backend_scheme_b_race_test",
            G.MISSING_TARGET, detail="xmake show -t reports not valid")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertTrue(
            any("c2b_fake_identity_integration" in r for r in reasons))

    def test_c2b_evidence_in_lifecycle_layer(self):
        # Every C2b record must be in the lifecycle layer so it appears in the
        # per-layer report section.
        for eid in C2B_EVIDENCE_IDS:
            ev = M.evidence_by_id(eid)
            self.assertEqual(ev.layer, "lifecycle",
                             f"C2b evidence '{eid}' must be lifecycle layer")

    def test_c2b_not_implemented_never_satisfies_mandatory_slot(self):
        # A not_implemented MANDATORY record can never satisfy a mandatory slot
        # in the ORDINARY (non-KernelIo) verdict branch. Use a synthetic
        # ReferenceProfile backend ("Ghost", not registered) so the priority-2
        # branch of _backend_verdict is actually exercised: the not_implemented
        # record seeds an INCOMPLETE run state, which is insufficient evidence
        # -> INCOMPLETE (never ELIGIBLE, never NOT_CONFORMING). The Uring
        # KernelIo early-return cannot prove this branch, so it is not used here.
        gap = M.Evidence(
            evidence_id="ghost_c2b_gap_injected",
            target="backend_conformance_test", layer="lifecycle",
            backends=("Ghost",), status=M.STATUS_NOT_IMPLEMENTED, mandatory=True,
        )
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, gap)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            ghost = M.BackendEntry("Ghost", "ReferenceProfile",
                                   "conformance_ghost")
            # Seed the run state the gate loop would produce for the
            # not_implemented record.
            g.results["ghost_c2b_gap_injected"] = G.RunResult(
                "ghost_c2b_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            verdict, reasons = g._backend_verdict(ghost)
            self.assertEqual(verdict, G.INCOMPLETE, reasons)
            self.assertTrue(
                any("ghost_c2b_gap_injected" in r for r in reasons),
                f"gap record must appear in reasons: {reasons}")

    def test_uring_c2b_integration_record_properties(self):
        # Direct manifest-level assertions (no verdict loop involved): the
        # Uring C2b record is mandatory + implemented + real-mode-only with a
        # pinned case set; the REMAINING Uring not_implemented gap (C2e, D4)
        # still enters Uring's applicable evidence set.
        ev = M.evidence_by_id("uring_c2b_identity_integration")
        self.assertEqual(ev.backends, ("Uring",))
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        self.assertEqual(ev.required_modes, ("real",))
        self.assertTrue(ev.cases)
        appl = M.applicable_evidence_for_backend("Uring")
        self.assertIn("uring_c2e_close_drain",
                      {e.evidence_id for e in appl})


# ---------------------------------------------------------------------------
# Phase C2c corrective: waiter / borrow / delivery-lease evidence enters the
# per-backend verdict.
#
# The C2c slice adds four evidence records: a backend-agnostic arena matrix,
# Fake-tagged and ThreadPool-tagged integration records (both mandatory +
# implemented), and a Uring-tagged not_implemented record. The tests below
# prove:
#   * each mandatory C2c record exists with the correct layer, backends,
#     status, and mandatory flag;
#   * Fake/ThreadPool C2c integration is mandatory and implemented;
#   * Uring's C2c gap is mandatory and not_implemented (never PASS);
#   * a C2c RUN_FAIL on one backend does not contaminate the other;
#   * the arena-level PASS cannot erase Uring's not_implemented C2c gap;
#   * a missing C2c target fails closed (MISSING_TARGET -> INCOMPLETE);
#   * a not_implemented C2c gap cannot satisfy a mandatory slot in the
#     ORDINARY (non-KernelIo) verdict branch — proven with a synthetic
#     ReferenceProfile backend (the KernelIo early-return cannot prove it);
#   * the C2c records appear in the lifecycle layer for the tagged backend.
# ---------------------------------------------------------------------------

C2C_EVIDENCE_IDS = (
    "c2c_arena_borrow_waiter_lease_matrix",
    "c2c_fake_borrow_waiter_integration",
    "c2c_threadpool_borrow_waiter_integration",
    "uring_c2c_borrow_waiter_integration",
)


class C2cEvidenceRecordTest(unittest.TestCase):
    """C2c evidence records exist with correct attributes."""

    def test_all_c2c_records_exist(self):
        for eid in C2C_EVIDENCE_IDS:
            self.assertIsNotNone(M.evidence_by_id(eid),
                                 f"C2c evidence '{eid}' must exist in manifest")

    def test_arena_matrix_is_backend_agnostic(self):
        ev = M.evidence_by_id("c2c_arena_borrow_waiter_lease_matrix")
        self.assertEqual(ev.backends, ())
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_fake_integration_is_mandatory_and_tagged(self):
        ev = M.evidence_by_id("c2c_fake_borrow_waiter_integration")
        self.assertIn("Fake", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_threadpool_integration_is_mandatory_and_tagged(self):
        ev = M.evidence_by_id("c2c_threadpool_borrow_waiter_integration")
        self.assertIn("ThreadPool", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_uring_c2c_integration_is_implemented_real_mode(self):
        # D3 closure: the Uring C2c record is now an implemented, real-mode-
        # only, case-pinned evidence record (stub execution can never PASS it).
        ev = M.evidence_by_id("uring_c2c_borrow_waiter_integration")
        self.assertEqual(ev.backends, ("Uring",))
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        self.assertEqual(ev.required_modes, ("real",))
        self.assertTrue(ev.cases and len(ev.cases) > 1)

    def test_c2c_ids_unique_in_manifest(self):
        ids = [e.evidence_id for e in M.EVIDENCE]
        for eid in C2C_EVIDENCE_IDS:
            self.assertEqual(ids.count(eid), 1,
                             f"C2c evidence '{eid}' must appear exactly once")


class C2cVerdictIntegrationTest(unittest.TestCase):
    """C2c evidence enters the per-backend verdict correctly."""

    def test_fake_has_mandatory_c2c_lifecycle_evidence(self):
        appl = M.applicable_evidence_for_backend("Fake")
        c2c = [e for e in appl if e.evidence_id in C2C_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2c),
                        "Fake must have at least one mandatory implemented "
                        "C2c lifecycle record")

    def test_threadpool_has_mandatory_c2c_lifecycle_evidence(self):
        appl = M.applicable_evidence_for_backend("ThreadPool")
        c2c = [e for e in appl if e.evidence_id in C2C_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2c),
                        "ThreadPool must have at least one mandatory "
                        "implemented C2c lifecycle record")

    def test_uring_has_mandatory_implemented_c2c_record(self):
        appl = M.applicable_evidence_for_backend("Uring")
        c2c = [e for e in appl if e.evidence_id in C2C_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2c),
                        "Uring must have a mandatory implemented C2c record")

    def test_uring_stub_verdict_incomplete_in_c2c(self):
        # After D3/D4 the C2c record is implemented/real-mode; the stub-mode
        # KernelIo verdict is INCOMPLETE (mode attribution), never ELIGIBLE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)

    def test_fake_c2c_failure_does_not_affect_threadpool(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2c_fake_borrow_waiter_integration"] = G.RunResult(
            "c2c_fake_borrow_waiter_integration", "backend_c2c_waiter_borrow_test",
            G.RUN_FAIL, detail="stub C2c Fake failure")
        v_fake, reasons_fake = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.NOT_CONFORMING)
        self.assertEqual(v_tp, G.ELIGIBLE,
                         "ThreadPool must stay ELIGIBLE when Fake C2c fails")
        self.assertTrue(
            any("c2c_fake_borrow_waiter_integration" in r for r in reasons_fake))

    def test_threadpool_c2c_failure_does_not_affect_fake(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2c_threadpool_borrow_waiter_integration"] = G.RunResult(
            "c2c_threadpool_borrow_waiter_integration",
            "threadpool_backend_c2c_waiter_borrow_test",
            G.RUN_FAIL, detail="stub C2c ThreadPool failure")
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, reasons_tp = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.ELIGIBLE,
                         "Fake must stay ELIGIBLE when ThreadPool C2c fails")
        self.assertEqual(v_tp, G.NOT_CONFORMING)
        self.assertTrue(
            any("c2c_threadpool_borrow_waiter_integration" in r
                for r in reasons_tp))

    def test_arena_pass_cannot_erase_uring_c2c_gap(self):
        # The arena matrix record is backend-agnostic (backends=()); it applies
        # to Uring but cannot satisfy Uring's own tagged obligation because the
        # tagged not_implemented C2e record remains INCOMPLETE (D4 work).
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2c_arena_borrow_waiter_lease_matrix"] = G.RunResult(
            "c2c_arena_borrow_waiter_lease_matrix",
            "request_waiter_borrow_lease_test",
            G.PASS, detail="stub arena PASS")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)

    def test_missing_c2c_target_fails_closed(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2c_fake_borrow_waiter_integration"] = G.RunResult(
            "c2c_fake_borrow_waiter_integration", "backend_c2c_waiter_borrow_test",
            G.MISSING_TARGET, detail="xmake show -t reports not valid")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertTrue(
            any("c2c_fake_borrow_waiter_integration" in r for r in reasons))

    def test_c2c_evidence_in_lifecycle_layer(self):
        # Every C2c record must be in the lifecycle layer so it appears in the
        # per-layer report section.
        for eid in C2C_EVIDENCE_IDS:
            ev = M.evidence_by_id(eid)
            self.assertEqual(ev.layer, "lifecycle",
                             f"C2c evidence '{eid}' must be lifecycle layer")

    def test_c2c_not_implemented_never_satisfies_mandatory_slot(self):
        # A not_implemented MANDATORY record can never satisfy a mandatory slot
        # in the ORDINARY (non-KernelIo) verdict branch. Use a synthetic
        # ReferenceProfile backend ("Ghost", not registered) so the priority-2
        # branch of _backend_verdict is actually exercised: the not_implemented
        # record seeds an INCOMPLETE run state, which is insufficient evidence
        # -> INCOMPLETE (never ELIGIBLE, never NOT_CONFORMING). The Uring
        # KernelIo early-return cannot prove this branch (C2b lesson), so it
        # is not used here.
        gap = M.Evidence(
            evidence_id="ghost_c2c_gap_injected",
            target="backend_conformance_test", layer="lifecycle",
            backends=("Ghost",), status=M.STATUS_NOT_IMPLEMENTED, mandatory=True,
        )
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, gap)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            ghost = M.BackendEntry("Ghost", "ReferenceProfile",
                                   "conformance_ghost")
            # Seed the run state the gate loop would produce for the
            # not_implemented record.
            g.results["ghost_c2c_gap_injected"] = G.RunResult(
                "ghost_c2c_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            verdict, reasons = g._backend_verdict(ghost)
            self.assertEqual(verdict, G.INCOMPLETE, reasons)
            self.assertTrue(
                any("ghost_c2c_gap_injected" in r for r in reasons),
                f"gap record must appear in reasons: {reasons}")

    def test_uring_c2c_integration_record_properties(self):
        # Direct manifest-level assertions (no verdict loop involved): the
        # Uring C2c record is mandatory + implemented + real-mode-only with a
        # pinned case set; the REMAINING Uring not_implemented gap (C2e, D4)
        # still enters Uring's applicable evidence set.
        ev = M.evidence_by_id("uring_c2c_borrow_waiter_integration")
        self.assertEqual(ev.backends, ("Uring",))
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        self.assertEqual(ev.required_modes, ("real",))
        self.assertTrue(ev.cases)
        appl = M.applicable_evidence_for_backend("Uring")
        self.assertIn("uring_c2e_close_drain",
                      {e.evidence_id for e in appl})


C2D_EVIDENCE_IDS = (
    "c2d_threadpool_failure_injection",
    "c2d_fake_failure_injection_terminal",
    "uring_c2d_failure_injection",
)


class C2dEvidenceRecordTest(unittest.TestCase):
    """C2d evidence records exist with correct attributes (Issue #68 rows 9-10)."""

    def test_all_c2d_records_exist(self):
        for eid in C2D_EVIDENCE_IDS:
            self.assertIsNotNone(M.evidence_by_id(eid),
                                 f"C2d evidence '{eid}' must exist in manifest")

    def test_threadpool_injection_is_mandatory_and_tagged(self):
        ev = M.evidence_by_id("c2d_threadpool_failure_injection")
        self.assertIn("ThreadPool", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_fake_injection_terminal_is_mandatory_and_tagged(self):
        ev = M.evidence_by_id("c2d_fake_failure_injection_terminal")
        self.assertIn("Fake", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_uring_c2d_is_implemented_and_requires_real_mode(self):
        ev = M.evidence_by_id("uring_c2d_failure_injection")
        self.assertIn("Uring", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        self.assertEqual(ev.target, "uring_d2_failure_noalloc_test")
        self.assertEqual(ev.required_modes, ("real",))

    def test_c2d_ids_unique_in_manifest(self):
        ids = [e.evidence_id for e in M.EVIDENCE]
        for eid in C2D_EVIDENCE_IDS:
            self.assertEqual(ids.count(eid), 1,
                             f"C2d evidence '{eid}' must appear exactly once")


class C2dVerdictIntegrationTest(unittest.TestCase):
    """C2d evidence enters the per-backend verdict correctly."""

    def test_fake_has_mandatory_c2d_lifecycle_evidence(self):
        appl = M.applicable_evidence_for_backend("Fake")
        c2d = [e for e in appl if e.evidence_id in C2D_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2d),
                        "Fake must have at least one mandatory implemented "
                        "C2d lifecycle record")

    def test_threadpool_has_mandatory_c2d_lifecycle_evidence(self):
        appl = M.applicable_evidence_for_backend("ThreadPool")
        c2d = [e for e in appl if e.evidence_id in C2D_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2d),
                        "ThreadPool must have at least one mandatory "
                        "implemented C2d lifecycle record")

    def test_uring_has_mandatory_implemented_c2d_record(self):
        appl = M.applicable_evidence_for_backend("Uring")
        c2d = [e for e in appl if e.evidence_id in C2D_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_IMPLEMENTED
                            for e in c2d),
                        "Uring must have mandatory implemented real-only C2d evidence")

    def test_uring_c2d_record_no_longer_surfaces_as_known_gap(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertFalse(any("uring_c2d_failure_injection" in r for r in reasons),
                         f"implemented C2d evidence is not a known gap: {reasons}")

    def test_fake_c2d_failure_does_not_affect_threadpool(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2d_fake_failure_injection_terminal"] = G.RunResult(
            "c2d_fake_failure_injection_terminal", "reference_backend_no_alloc_test",
            G.RUN_FAIL, detail="stub C2d Fake failure")
        v_fake, reasons_fake = g._backend_verdict(M.backend_by_name("Fake"))
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_fake, G.NOT_CONFORMING)
        self.assertEqual(v_tp, G.ELIGIBLE,
                         "ThreadPool must stay ELIGIBLE when Fake C2d fails")

    def test_threadpool_c2d_failure_does_not_affect_fake(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2d_threadpool_failure_injection"] = G.RunResult(
            "c2d_threadpool_failure_injection", "threadpool_backend_c2d_failure_test",
            G.RUN_FAIL, detail="stub C2d ThreadPool failure")
        v_tp, reasons_tp = g._backend_verdict(M.backend_by_name("ThreadPool"))
        v_fake, _ = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(v_tp, G.NOT_CONFORMING)
        self.assertEqual(v_fake, G.ELIGIBLE,
                         "Fake must stay ELIGIBLE when ThreadPool C2d fails")

    def test_stub_mode_cannot_satisfy_real_only_uring_c2d_record(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["uring_c2d_failure_injection"] = G.RunResult(
            "uring_c2d_failure_injection", "uring_d2_failure_noalloc_test",
            G.INCOMPLETE, detail="mode=stub not allowed")
        state = g._backend_run_state(
            M.evidence_by_id("uring_c2d_failure_injection"),
            "Uring", "KernelIoProfile")
        self.assertEqual(state, G.INCOMPLETE)

    def test_real_mode_satisfies_uring_c2d_record_without_lifting_kernelio(self):
        g = _stub_gate(
            {"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
            meta_override={
                "Fake": {"profile": "ReferenceProfile", "mode": "deterministic"},
                "ThreadPool": {"profile": "BlockingIoProfile", "mode": "real"},
                "Uring": {"profile": "KernelIoProfile", "mode": "real"},
            })
        g.results["uring_c2d_failure_injection"] = G.RunResult(
            "uring_c2d_failure_injection", "uring_d2_failure_noalloc_test",
            G.PASS, detail="mode=real")
        state = g._backend_run_state(
            M.evidence_by_id("uring_c2d_failure_injection"),
            "Uring", "KernelIoProfile")
        self.assertEqual(state, G.PASS)
        # D4 lift: with real-mode attribution for every mandatory record the
        # ordinary machinery makes KernelIo ELIGIBLE (see the C2e positive
        # test); the C2d record's own state is PASS either way.
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.ELIGIBLE)

    def test_missing_c2d_target_fails_closed(self):
        # A backend's mandatory implemented C2d record whose run state is not
        # PASS forces the verdict to INCOMPLETE (never skip-as-pass).
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2d_threadpool_failure_injection"] = G.RunResult(
            "c2d_threadpool_failure_injection", "threadpool_backend_c2d_failure_test",
            G.MISSING_TARGET, detail="xmake show -t reports not valid")
        v_tp, reasons_tp = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_tp, G.INCOMPLETE)
        self.assertTrue(
            any("c2d_threadpool_failure_injection" in r for r in reasons_tp))

    def test_c2d_evidence_in_lifecycle_layer(self):
        for eid in C2D_EVIDENCE_IDS:
            ev = M.evidence_by_id(eid)
            self.assertEqual(ev.layer, "lifecycle",
                             f"C2d evidence '{eid}' must live in the lifecycle layer")

    def test_c2d_not_implemented_never_satisfies_mandatory_slot(self):
        # A not_implemented MANDATORY record can never satisfy a mandatory slot
        # in the ORDINARY (non-KernelIo) verdict branch. Use a synthetic
        # ReferenceProfile backend ("Ghost", not registered) so the priority-2
        # branch of _backend_verdict is actually exercised: the not_implemented
        # record seeds an INCOMPLETE run state, which is insufficient evidence
        # -> INCOMPLETE (never ELIGIBLE, never NOT_CONFORMING). The Uring
        # KernelIo early-return cannot prove this branch (C2b/C2c lesson), so
        # it is not used here.
        gap = M.Evidence(
            evidence_id="ghost_c2d_gap_injected",
            target="backend_conformance_test", layer="lifecycle",
            backends=("Ghost",), status=M.STATUS_NOT_IMPLEMENTED, mandatory=True,
        )
        with mock.patch.object(M, "EVIDENCE", (*M.EVIDENCE, gap)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            ghost = M.BackendEntry("Ghost", "ReferenceProfile",
                                   "conformance_ghost")
            # Seed the run state the gate loop would produce for the
            # not_implemented record.
            g.results["ghost_c2d_gap_injected"] = G.RunResult(
                "ghost_c2d_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            verdict, reasons = g._backend_verdict(ghost)
            self.assertEqual(verdict, G.INCOMPLETE, reasons)
            self.assertTrue(
                any("ghost_c2d_gap_injected" in r for r in reasons),
                f"gap record must appear in reasons: {reasons}")

    def test_uring_c2d_evidence_is_mandatory_implemented_applicable(self):
        ev = M.evidence_by_id("uring_c2d_failure_injection")
        self.assertIn("Uring", ev.backends)
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        appl = M.applicable_evidence_for_backend("Uring")
        self.assertIn("uring_c2d_failure_injection",
                      {e.evidence_id for e in appl})


# ---------------------------------------------------------------------------
# Phase C2e — close / drain / reset / destruction (Issue #68 rows 15-16).
#
# The C2e slice adds four evidence records:
#   * c2e_shared_close_drain_suite — shared layer, Fake+ThreadPool, driven
#     per backend by the close_drain_driver_case (conformance_close_drain_fake /
#     conformance_close_drain_threadpool);
#   * c2e_threadpool_close_drain_race — lifecycle, ThreadPool (the
#     deterministic window/race target);
#   * c2e_fake_close_drain_death — lifecycle, Fake (the reference-path death
#     target);
#   * uring_c2e_close_drain — lifecycle, Uring, implemented, real-mode-only
#     (Phase D4; the dedicated deterministic target + the death matrix).
#
# The tests below prove (Issue #68 §"Manifest / conformance gate"):
#   * implemented C2e evidence is the ONLY thing that can make a backend's
#     C2e verdict PASS (a missing close/drain driver run is NOT_RUN ->
#     INCOMPLETE, never ELIGIBLE);
#   * Fake/ThreadPool verdicts read their OWN close_drain_by_backend result —
#     one backend's close/drain RUN_FAIL does not contaminate the other;
#   * a backend whose close/drain run is MISSING (harness error) is INCOMPLETE;
#   * Uring's C2e record is real-mode-only: a stub build can never satisfy it
#     (required_modes + the shared-suite real-mode downgrade), so a stub
#     KernelIo build stays NOT CONFORMING after the D4 lift;
#   * the gate drives _run_close_drain_suite exactly once and never routes the
#     shared close/drain suite through the generic _drive() loop.
# ---------------------------------------------------------------------------

C2E_EVIDENCE_IDS = (
    "c2e_shared_close_drain_suite",
    "c2e_threadpool_close_drain_race",
    "c2e_fake_close_drain_death",
    "uring_c2e_close_drain",
)


class C2eEvidenceRecordTest(unittest.TestCase):
    """C2e evidence records exist with correct attributes."""

    def test_all_c2e_records_exist(self):
        for eid in C2E_EVIDENCE_IDS:
            self.assertIsNotNone(M.evidence_by_id(eid),
                                 f"C2e evidence '{eid}' must exist in manifest")

    def test_shared_suite_is_mandatory_implemented_shared_layer(self):
        ev = M.evidence_by_id("c2e_shared_close_drain_suite")
        self.assertIn("Fake", ev.backends)
        self.assertIn("ThreadPool", ev.backends)
        self.assertIn("Uring", ev.backends)  # Phase D4: Uring gained the seam
        self.assertEqual(ev.layer, "shared")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_threadpool_race_is_mandatory_implemented_lifecycle(self):
        ev = M.evidence_by_id("c2e_threadpool_close_drain_race")
        self.assertIn("ThreadPool", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_fake_death_is_mandatory_implemented_lifecycle(self):
        ev = M.evidence_by_id("c2e_fake_close_drain_death")
        self.assertIn("Fake", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)

    def test_uring_close_drain_is_implemented_real_mode(self):
        # D4 closure: the Uring C2e record is now an implemented, real-mode-
        # only, case-pinned evidence record (stub execution can never PASS it).
        ev = M.evidence_by_id("uring_c2e_close_drain")
        self.assertEqual(ev.backends, ("Uring",))
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_IMPLEMENTED)
        self.assertEqual(ev.required_modes, ("real",))
        self.assertTrue(ev.cases and len(ev.cases) > 1)

    def test_c2e_ids_unique_in_manifest(self):
        ids = [e.evidence_id for e in M.EVIDENCE]
        for eid in C2E_EVIDENCE_IDS:
            self.assertEqual(ids.count(eid), 1,
                             f"C2e evidence '{eid}' must appear exactly once")

    def test_every_close_drain_driver_backend_has_the_shared_record(self):
        # Fail-closed wiring: any backend that declares a close_drain_driver_
        # case MUST have the shared close/drain record applicable to it (the
        # gate drives the suite per that backend).
        ev = M.evidence_by_id("c2e_shared_close_drain_suite")
        for b in M.BACKENDS:
            if b.close_drain_driver_case:
                self.assertIn(
                    b.name, ev.backends,
                    f"backend {b.name} declares a close_drain_driver_case but "
                    f"c2e_shared_close_drain_suite does not cover it")


class C2eVerdictIntegrationTest(unittest.TestCase):
    """C2e evidence enters the per-backend verdict correctly."""

    def test_fake_has_mandatory_c2e_evidence(self):
        appl = M.applicable_evidence_for_backend("Fake")
        c2e = [e for e in appl if e.evidence_id in C2E_EVIDENCE_IDS]
        self.assertTrue(
            any(e.mandatory and e.status == M.STATUS_IMPLEMENTED for e in c2e),
            "Fake must have at least one mandatory implemented C2e record")

    def test_threadpool_has_mandatory_c2e_evidence(self):
        appl = M.applicable_evidence_for_backend("ThreadPool")
        c2e = [e for e in appl if e.evidence_id in C2E_EVIDENCE_IDS]
        self.assertTrue(
            any(e.mandatory and e.status == M.STATUS_IMPLEMENTED for e in c2e),
            "ThreadPool must have at least one mandatory implemented C2e record")

    def test_uring_incomplete_when_close_drain_not_driven(self):
        # D4 lift: a registered close/drain driver case that the gate never
        # drove is NOT_RUN -> INCOMPLETE (harness error, never ELIGIBLE,
        # never skip-as-pass).
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.close_drain_by_backend.pop("Uring", None)  # harness never drove it
        state = g._backend_run_state(
            M.evidence_by_id("c2e_shared_close_drain_suite"),
            "Uring", "KernelIoProfile")
        self.assertEqual(state, G.NOT_RUN,
                         "undriven close/drain must be NOT_RUN")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertTrue(
            any("shared_close_drain" in r for r in reasons),
            f"undriven close/drain must surface in the verdict: {reasons}")

    def test_uring_eligible_when_all_real_evidence_pass(self):
        # D4 lift (positive): with REAL mode attribution and every mandatory
        # record PASS (shared + capacity + close/drain suites + the tagged
        # real-mode lifecycle records), the ordinary machinery makes KernelIo
        # ELIGIBLE — the fail-closed hard-code is gone.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
                       meta_override={
                           "Fake": {"profile": "ReferenceProfile", "mode": "deterministic"},
                           "ThreadPool": {"profile": "BlockingIoProfile", "mode": "real"},
                           "Uring": {"profile": "KernelIoProfile", "mode": "real"},
                       })
        # Real-mode close/drain + capacity results.
        g.close_drain_by_backend["Uring"] = G.RunResult(
            "c2e_shared_close_drain_suite:Uring", "backend_conformance_test",
            G.PASS, detail="real close/drain PASS")
        g.capacity_by_backend["Uring"] = G.RunResult(
            "shared_capacity_suite:Uring", "backend_conformance_test",
            G.PASS, detail="real capacity PASS")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.ELIGIBLE,
                         f"all-real evidence must be ELIGIBLE after the lift: {reasons}")

    def test_missing_close_drain_run_makes_backend_incomplete(self):
        # A backend that declares a close_drain_driver_case but whose
        # close_drain_by_backend entry is absent (the gate never drove it —
        # a harness error) must be INCOMPLETE, never ELIGIBLE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        del g.close_drain_by_backend["Fake"]
        verdict, reasons = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(verdict, G.INCOMPLETE)
        self.assertTrue(
            any("c2e_shared_close_drain_suite" in r for r in reasons),
            f"missing close/drain run must surface in reasons: {reasons}")

    def test_close_drain_run_fail_does_not_contaminate_other_backend(self):
        # Fake's close/drain RUN_FAIL must make Fake NOT CONFORMING while
        # ThreadPool (its own PASS) stays ELIGIBLE — per-backend attribution.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.close_drain_by_backend["Fake"] = G.RunResult(
            "c2e_shared_close_drain_suite:Fake", "backend_conformance_test",
            G.RUN_FAIL, detail="stub close/drain fail")
        v_fake, reasons = g._backend_verdict(M.backend_by_name("Fake"))
        self.assertEqual(v_fake, G.NOT_CONFORMING)
        self.assertTrue(
            any("c2e_shared_close_drain_suite" in r for r in reasons),
            f"close/drain failure must surface in Fake reasons: {reasons}")
        v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
        self.assertEqual(v_tp, G.ELIGIBLE,
                         "ThreadPool's own close/drain PASS must not be "
                         "contaminated by Fake's failure")

    def test_all_mandatory_close_drain_pass_is_eligible(self):
        # Implemented evidence PASS is what makes the C2e verdict green: with
        # every per-backend close/drain run PASS (the _stub_gate default), the
        # migrated backends are ELIGIBLE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("Fake"))[0], G.ELIGIBLE)
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("ThreadPool"))[0], G.ELIGIBLE)


class C2eCloseDrainDriveExclusionTest(unittest.TestCase):
    """_drive() never receives the shared close/drain suite; the per-backend
    runs happen through _run_close_drain_suite (parity with the C2a capacity
    suite fix)."""

    def _run_gate_observing_drives(self):
        _, driven, _, _, close_drain_runs = \
            _stubbed_gate_with_recorded_runs()
        return driven, close_drain_runs

    def test_drive_never_receives_close_drain_suite(self):
        driven, _ = self._run_gate_observing_drives()
        self.assertNotIn("c2e_shared_close_drain_suite", driven,
                         f"c2e_shared_close_drain_suite must not enter "
                         f"_drive(): {driven}")

    def test_run_close_drain_suite_executed_once(self):
        _, close_drain_runs = self._run_gate_observing_drives()
        self.assertEqual(close_drain_runs, ["c2e_shared_close_drain_suite"],
                         f"_run_close_drain_suite must run exactly once: "
                         f"{close_drain_runs}")

    def test_no_generic_results_entry_for_close_drain_suite(self):
        g, _, _, _, _ = _stubbed_gate_with_recorded_runs()
        self.assertNotIn(
            "c2e_shared_close_drain_suite", g.results,
            "c2e_shared_close_drain_suite must not get a generic results[] "
            "entry; its verdict is read from close_drain_by_backend")


if __name__ == "__main__":
    # Standalone invocation mirrors `unittest discover`: exit non-zero on any
    # failure, zero on full pass. No top-level sys.exit during import.
    unittest.main(verbosity=2)
