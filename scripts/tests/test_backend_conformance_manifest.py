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
    # Phase C2e: per-backend shared CLOSE/DRAIN-suite state. Only backends
    # with a close_admission seam (Fake, ThreadPool) are driven; default them
    # to PASS so the ONLY variable under test is the shared-suite state. Uring
    # has no close/drain driver case, so it is not seeded here.
    g.close_drain_by_backend = {}
    for b in M.BACKENDS:
        if b.close_drain_driver_case:
            g.close_drain_by_backend[b.name] = G.RunResult(
                f"c2e_shared_close_drain_suite:{b.name}",
                "backend_conformance_test", G.PASS, detail="stub close/drain PASS")
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

    def _drive_c2d(self, output, rc=0):
        ev = M.evidence_by_id("uring_c2d_failure_injection")
        gate = G.Gate(args=mock.Mock(no_build=True))
        with mock.patch.object(G, "xmake_target_exists", return_value=True), \
             mock.patch.object(G, "xmake_run_target", return_value=(rc, output)):
            return gate._drive(ev)

    def test_real_mode_is_pass(self):
        result = self._drive_c2d(
            "[evidence-meta] evidence=uring_c2d_failure_injection mode=real\n")
        self.assertEqual(result.state, G.PASS)

    def test_stub_mode_is_incomplete_not_pass(self):
        result = self._drive_c2d(
            "[evidence-meta] evidence=uring_c2d_failure_injection mode=stub\n")
        self.assertEqual(result.state, G.INCOMPLETE)

    def test_missing_or_duplicate_mode_is_incomplete(self):
        missing = self._drive_c2d("ALL TESTS PASSED\n")
        duplicate = self._drive_c2d(
            "[evidence-meta] evidence=uring_c2d_failure_injection mode=real\n"
            "[evidence-meta] evidence=uring_c2d_failure_injection mode=real\n")
        self.assertEqual(missing.state, G.INCOMPLETE)
        self.assertEqual(duplicate.state, G.INCOMPLETE)

    def test_nonzero_real_target_is_run_fail(self):
        result = self._drive_c2d(
            "[evidence-meta] evidence=uring_c2d_failure_injection mode=real\n",
            rc=1)
        self.assertEqual(result.state, G.RUN_FAIL)


class FailClosedMetadataTest(unittest.TestCase):
    """Missing/malformed metadata fails closed (NOT_RUN / unknown mode)."""

    def test_missing_meta_for_backend_is_unknown_mode(self):
        # No meta at all -> mode 'unknown' -> KernelIo NOT CONFORMING; for the
        # reference/blocking profiles the gate treats absence as not-seen.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
                       meta_override={})
        # KernelIo with unknown mode must be NOT CONFORMING (never ELIGIBLE).
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)

    def test_unknown_mode_for_kernel_is_not_conforming(self):
        g = _stub_gate(
            {"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"},
            meta_override={"Uring": {"profile": "KernelIoProfile",
                                     "mode": "unknown"}})
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)


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
        self.assertEqual(v_uring, G.NOT_CONFORMING,
                         "Uring must report its Phase-D gap independently")
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

    def test_all_pass_fake_and_tp_eligible_uring_not_conforming(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                        "Uring": "PASS"})
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("Fake"))[0], G.ELIGIBLE)
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("ThreadPool"))[0], G.ELIGIBLE)
        self.assertEqual(
            g._backend_verdict(M.backend_by_name("Uring"))[0], G.NOT_CONFORMING)


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

    def test_kernel_profile_never_eligible_in_c1(self):
        # Regardless of run state, KernelIoProfile lifecycle/backend_specific
        # is INCOMPLETE (Phase D), so Uring is never ELIGIBLE in C1.
        for state in ("PASS", "RUN_FAIL", "NOT_RUN"):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": state})
            verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
            self.assertEqual(verdict, G.NOT_CONFORMING,
                             f"Uring must be NOT CONFORMING even if shared={state}")


class SubprocessOutcomeMappingTest(unittest.TestCase):
    """Cases 8 of task §4: subprocess non-zero / timeout map deterministically."""

    def test_nonzero_subprocess_is_run_fail(self):
        self.assertEqual(G._state_for_rc(0), G.PASS)
        self.assertEqual(G._state_for_rc(1), G.RUN_FAIL)
        self.assertEqual(G._state_for_rc(2), G.RUN_FAIL)

    def test_timeout_subprocess_is_run_fail(self):
        self.assertEqual(G._state_for_rc(124), G.RUN_FAIL)


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
        impl = M.implemented_evidence_for_backend("Uring")
        ids = {e.evidence_id for e in impl}
        self.assertNotIn("uring_c2b_identity_not_implemented", ids)

    def test_applicable_includes_not_implemented_for_tagged_backend(self):
        appl = M.applicable_evidence_for_backend("Uring")
        ids = {e.evidence_id for e in appl}
        self.assertIn("uring_c2b_identity_not_implemented", ids)

    def test_applicable_excludes_not_implemented_for_other_backend(self):
        # The Uring identity gap is tagged backends=("Uring",); it MUST NOT apply
        # to Fake or ThreadPool.
        for name in ("Fake", "ThreadPool"):
            appl = M.applicable_evidence_for_backend(name)
            ids = {e.evidence_id for e in appl}
            self.assertNotIn("uring_c2b_identity_not_implemented", ids,
                             f"{name} must not see the Uring identity gap")

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
        self.assertEqual(verdict, G.NOT_CONFORMING)
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
        self.assertEqual(verdict, G.NOT_CONFORMING)

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
        self.assertEqual(verdict, G.NOT_CONFORMING)


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
    "uring_c2b_identity_not_implemented",
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

    def test_uring_c2b_gap_is_not_implemented(self):
        ev = M.evidence_by_id("uring_c2b_identity_not_implemented")
        self.assertIn("Uring", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_NOT_IMPLEMENTED)

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

    def test_uring_has_mandatory_not_implemented_c2b_record(self):
        appl = M.applicable_evidence_for_backend("Uring")
        c2b = [e for e in appl if e.evidence_id in C2B_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_NOT_IMPLEMENTED
                            for e in c2b),
                        "Uring must have a mandatory not_implemented C2b record")

    def test_uring_c2b_gap_surfaces_in_verdict(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertTrue(
            any("uring_c2b_identity_not_implemented" in r for r in reasons),
            f"Uring C2b gap must appear in reasons: {reasons}")

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

    def test_arena_pass_cannot_erase_uring_c2b_gap(self):
        # The arena matrix record is backend-agnostic (backends=()); it applies
        # to Uring but cannot satisfy Uring's C2b obligation because Uring's
        # own tagged not_implemented record remains INCOMPLETE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2b_arena_state_identity_matrix"] = G.RunResult(
            "c2b_arena_state_identity_matrix",
            "request_lifecycle_scheme_b_test",
            G.PASS, detail="stub arena PASS")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertTrue(
            any("uring_c2b_identity_not_implemented" in r for r in reasons))

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

    def test_uring_c2b_gap_evidence_is_mandatory_not_implemented_applicable(self):
        # Direct manifest-level assertions (no verdict loop involved): the
        # Uring C2b gap is mandatory + not_implemented and enters Uring's
        # applicable evidence set — the properties the C2b gap record must
        # carry regardless of the KernelIoProfile verdict early-return.
        ev = M.evidence_by_id("uring_c2b_identity_not_implemented")
        self.assertIn("Uring", ev.backends)
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_NOT_IMPLEMENTED)
        appl = M.applicable_evidence_for_backend("Uring")
        self.assertIn("uring_c2b_identity_not_implemented",
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
    "uring_c2c_borrow_waiter_not_implemented",
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

    def test_uring_c2c_gap_is_not_implemented(self):
        ev = M.evidence_by_id("uring_c2c_borrow_waiter_not_implemented")
        self.assertIn("Uring", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_NOT_IMPLEMENTED)

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

    def test_uring_has_mandatory_not_implemented_c2c_record(self):
        appl = M.applicable_evidence_for_backend("Uring")
        c2c = [e for e in appl if e.evidence_id in C2C_EVIDENCE_IDS]
        self.assertTrue(any(e.mandatory and e.status == M.STATUS_NOT_IMPLEMENTED
                            for e in c2c),
                        "Uring must have a mandatory not_implemented C2c record")

    def test_uring_c2c_gap_surfaces_in_verdict(self):
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertTrue(
            any("uring_c2c_borrow_waiter_not_implemented" in r for r in reasons),
            f"Uring C2c gap must appear in reasons: {reasons}")

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
        # to Uring but cannot satisfy Uring's C2c obligation because Uring's
        # own tagged not_implemented record remains INCOMPLETE.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        g.results["c2c_arena_borrow_waiter_lease_matrix"] = G.RunResult(
            "c2c_arena_borrow_waiter_lease_matrix",
            "request_waiter_borrow_lease_test",
            G.PASS, detail="stub arena PASS")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertTrue(
            any("uring_c2c_borrow_waiter_not_implemented" in r for r in reasons))

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

    def test_uring_c2c_gap_evidence_is_mandatory_not_implemented_applicable(self):
        # Direct manifest-level assertions (no verdict loop involved): the
        # Uring C2c gap is mandatory + not_implemented and enters Uring's
        # applicable evidence set.
        ev = M.evidence_by_id("uring_c2c_borrow_waiter_not_implemented")
        self.assertIn("Uring", ev.backends)
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_NOT_IMPLEMENTED)
        appl = M.applicable_evidence_for_backend("Uring")
        self.assertIn("uring_c2c_borrow_waiter_not_implemented",
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
        self.assertEqual(verdict, G.NOT_CONFORMING)
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
        verdict, _ = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)

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
#   * uring_c2e_close_drain_not_implemented — lifecycle, Uring, not_implemented
#     (never PASS).
#
# The tests below prove (Issue #68 §"Manifest / conformance gate"):
#   * implemented C2e evidence is the ONLY thing that can make a backend's
#     C2e verdict PASS (a missing close/drain driver run is NOT_RUN ->
#     INCOMPLETE, never ELIGIBLE);
#   * Fake/ThreadPool verdicts read their OWN close_drain_by_backend result —
#     one backend's close/drain RUN_FAIL does not contaminate the other;
#   * a backend whose close/drain run is MISSING (harness error) is INCOMPLETE;
#   * Uring's not_implemented C2e gap keeps Uring INCOMPLETE/NOT CONFORMING —
#     never PASS, never skip-as-pass;
#   * the gate drives _run_close_drain_suite exactly once and never routes the
#     shared close/drain suite through the generic _drive() loop.
# ---------------------------------------------------------------------------

C2E_EVIDENCE_IDS = (
    "c2e_shared_close_drain_suite",
    "c2e_threadpool_close_drain_race",
    "c2e_fake_close_drain_death",
    "uring_c2e_close_drain_not_implemented",
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
        self.assertNotIn("Uring", ev.backends)
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

    def test_uring_gap_is_not_implemented(self):
        ev = M.evidence_by_id("uring_c2e_close_drain_not_implemented")
        self.assertIn("Uring", ev.backends)
        self.assertEqual(ev.layer, "lifecycle")
        self.assertTrue(ev.mandatory)
        self.assertEqual(ev.status, M.STATUS_NOT_IMPLEMENTED)

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

    def test_uring_c2e_gap_enters_verdict_never_pass(self):
        # Uring's C2e gap is a mandatory not_implemented record: the state the
        # gate assigns it is INCOMPLETE (never PASS), and it appears in the
        # applicable set so Uring's OWN verdict surfaces it.
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        # Uring has no close_drain_by_backend entry (no seam) by construction.
        self.assertNotIn("Uring", g.close_drain_by_backend)
        # Seed the gap's results entry the way the gate run loop does
        # (NotImplementedEntersVerdictTest pattern).
        g.results["uring_c2e_close_drain_not_implemented"] = G.RunResult(
            "uring_c2e_close_drain_not_implemented",
            "backend_conformance_test", G.INCOMPLETE,
            detail="manifest status not_implemented (Phase C2/D)")
        state = g._backend_run_state(
            M.evidence_by_id("uring_c2e_close_drain_not_implemented"),
            "Uring", "KernelIoProfile")
        self.assertEqual(state, G.INCOMPLETE,
                         "not_implemented must map to INCOMPLETE, never PASS")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertTrue(
            any("uring_c2e_close_drain_not_implemented" in r for r in reasons),
            f"Uring C2e gap must surface in the verdict reasons: {reasons}")

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
