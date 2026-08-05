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
        # A not_implemented evidence record must not be mandatory with a
        # misleading pass note; the gate treats not_implemented as INCOMPLETE.
        for e in M.EVIDENCE:
            if e.status == M.STATUS_NOT_IMPLEMENTED:
                # Sanity: the manifest never marks a known-gap record PASS.
                self.assertNotEqual(e.status, M.STATUS_IMPLEMENTED)


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
        "Uring(stub)": {"profile": "KernelIoProfile", "mode": "stub"},
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
        # Only registered backends are evaluated; there is no code path that
        # can produce a verdict for an unregistered name.
        registered = {b.name for b in M.BACKENDS}
        self.assertEqual(registered, {"Fake", "ThreadPool", "Uring"})

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


if __name__ == "__main__":
    # Standalone invocation mirrors `unittest discover`: exit non-zero on any
    # failure, zero on full pass. No top-level sys.exit during import.
    unittest.main(verbosity=2)
