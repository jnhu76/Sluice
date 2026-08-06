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
        with mock.patch.object(M, "EVIDENCE", M.EVIDENCE + (gap,)), \
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
    # Phase C2a: per-backend shared CAPACITY-suite state. Only backends with a
    # capacity seam (Fake, ThreadPool) are driven; default them to PASS so the
    # ONLY variable under test (in the attribution tests) is the shared-suite
    # state. Uring has no capacity driver case, so it is not seeded here.
    g.capacity_by_backend = {}
    for b in M.BACKENDS:
        if b.capacity_driver_case:
            g.capacity_by_backend[b.name] = G.RunResult(
                f"shared_capacity_suite:{b.name}", "backend_conformance_test",
                G.PASS, detail="stub capacity PASS")
    return g


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
        with mock.patch.object(M, "EVIDENCE", M.EVIDENCE + (diag,)):
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
        # The real Uring capacity gap record is not_implemented; implemented
        # helper must NOT return it.
        impl = M.implemented_evidence_for_backend("Uring")
        ids = {e.evidence_id for e in impl}
        self.assertNotIn("uring_capacity_not_implemented", ids)

    def test_applicable_includes_not_implemented_for_tagged_backend(self):
        appl = M.applicable_evidence_for_backend("Uring")
        ids = {e.evidence_id for e in appl}
        self.assertIn("uring_capacity_not_implemented", ids)

    def test_applicable_excludes_not_implemented_for_other_backend(self):
        # The Uring gap record is tagged backends=("Uring",); it MUST NOT apply
        # to Fake or ThreadPool.
        for name in ("Fake", "ThreadPool"):
            appl = M.applicable_evidence_for_backend(name)
            ids = {e.evidence_id for e in appl}
            self.assertNotIn("uring_capacity_not_implemented", ids,
                             f"{name} must not see the Uring capacity gap")

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
        with mock.patch.object(M, "EVIDENCE", M.EVIDENCE + (gap,)):
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
        with mock.patch.object(M, "EVIDENCE", M.EVIDENCE + (gap,)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            g.results["fake_capacity_gap_injected"] = G.RunResult(
                "fake_capacity_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            v_tp, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
            self.assertEqual(v_tp, G.ELIGIBLE,
                             "ThreadPool must not be affected by Fake's gap")

    def test_uring_capacity_gap_surfaces_in_verdict(self):
        # The real uring_capacity_not_implemented record: Uring is NOT CONFORMING
        # (KernelIoProfile rule), and the capacity gap MUST appear among the
        # reasons (reinforcing, not replacing, the rule).
        g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS", "Uring": "PASS"})
        # The run loop seeds not_implemented records as INCOMPLETE.
        g.results["uring_capacity_not_implemented"] = G.RunResult(
            "uring_capacity_not_implemented", "backend_conformance_test",
            G.INCOMPLETE, detail="not_implemented")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertTrue(
            any("uring_capacity_not_implemented" in r for r in reasons),
            f"Uring capacity gap must appear in reasons: {reasons}")

    def test_not_implemented_never_counts_as_pass_in_verdict(self):
        # A not_implemented record can never satisfy a mandatory slot. Even
        # though it seeds INCOMPLETE (which is not RUN_FAIL), the verdict logic
        # treats INCOMPLETE as insufficient -> INCOMPLETE, never ELIGIBLE.
        gap = M.Evidence(
            evidence_id="tp_only_gap_injected",
            target="backend_conformance_test", layer="shared",
            backends=("ThreadPool",), status=M.STATUS_NOT_IMPLEMENTED,
            mandatory=True,
        )
        with mock.patch.object(M, "EVIDENCE", M.EVIDENCE + (gap,)):
            g = _stub_gate({"Fake": "PASS", "ThreadPool": "PASS",
                            "Uring": "PASS"})
            g.results["tp_only_gap_injected"] = G.RunResult(
                "tp_only_gap_injected", "backend_conformance_test",
                G.INCOMPLETE, detail="not_implemented")
            verdict, _ = g._backend_verdict(M.backend_by_name("ThreadPool"))
            self.assertNotEqual(verdict, G.ELIGIBLE)


# ---------------------------------------------------------------------------
# Phase C2a: shared_capacity_suite is driven per-backend in isolated
# subprocesses, and the verdict reads each backend's OWN capacity result.
# Backends without a capacity seam (Uring) are not driven (their gap is the
# manifest's not_implemented record, surfaced via applicable_evidence).
# ---------------------------------------------------------------------------

class CapacitySuiteDriverCaseTest(unittest.TestCase):
    """The capacity_driver_case field: Fake/TP have one, Uring does not."""

    def test_fake_and_threadpool_have_capacity_driver_case(self):
        for name in ("Fake", "ThreadPool"):
            b = M.backend_by_name(name)
            self.assertTrue(b.capacity_driver_case,
                            f"{name} must have a capacity_driver_case")

    def test_uring_has_no_capacity_driver_case(self):
        # Uring has not migrated onto RequestArena (Phase D pending); it has no
        # capacity seam, so it has no capacity driver case. The gap is the
        # uring_capacity_not_implemented record, never a skip-as-pass.
        b = M.backend_by_name("Uring")
        self.assertFalse(b.capacity_driver_case,
                         "Uring must NOT have a capacity_driver_case "
                         "(Phase D pending)")

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

    def test_uring_verdict_unaffected_by_capacity_seam_absence(self):
        # Uring has no capacity_driver_case, so it has no capacity_by_backend
        # entry. Its verdict stays NOT CONFORMING (KernelIoProfile rule) and the
        # uring_capacity_not_implemented record appears in its reasons.
        g = self._stub_capacity({"Fake": "PASS", "ThreadPool": "PASS"})
        g.results["uring_capacity_not_implemented"] = G.RunResult(
            "uring_capacity_not_implemented", "backend_conformance_test",
            G.INCOMPLETE, detail="not_implemented")
        verdict, reasons = g._backend_verdict(M.backend_by_name("Uring"))
        self.assertEqual(verdict, G.NOT_CONFORMING)
        self.assertTrue(
            any("uring_capacity_not_implemented" in r for r in reasons))

    def test_shared_capacity_suite_evidence_tagged_for_fake_and_tp(self):
        # The implemented shared_capacity_suite record applies to Fake and
        # ThreadPool only; it must NOT apply to Uring.
        appl_fake = M.applicable_evidence_for_backend("Fake")
        appl_tp = M.applicable_evidence_for_backend("ThreadPool")
        appl_uring = M.applicable_evidence_for_backend("Uring")
        ids_fake = {e.evidence_id for e in appl_fake}
        ids_tp = {e.evidence_id for e in appl_tp}
        ids_uring = {e.evidence_id for e in appl_uring}
        self.assertIn("shared_capacity_suite", ids_fake)
        self.assertIn("shared_capacity_suite", ids_tp)
        self.assertNotIn("shared_capacity_suite", ids_uring,
                         "Uring must not see the implemented capacity record "
                         "(its gap is uring_capacity_not_implemented)")


if __name__ == "__main__":
    # Standalone invocation mirrors `unittest discover`: exit non-zero on any
    # failure, zero on full pass. No top-level sys.exit during import.
    unittest.main(verbosity=2)
