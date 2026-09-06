#!/usr/bin/env python3
"""FDG-0 Phase B hermetic facet tests (issue #298).

Registry structural negatives (task book §17 B12 / §18 invariants):

    FV1  duplicate facet id                       -> registry invalid
    FV2  duplicate anchor id                      -> registry invalid
    FV3  unknown facet anchor_ref                 -> registry invalid
    FV4  facet suite not in parent claim          -> registry invalid
    FV5  parent suite omitted from every facet    -> registry invalid
    FV6  faceted anchor referenced by no facet    -> registry invalid
    FV7  facet with zero anchors                  -> registry invalid
    FV8  facet with zero formal targets           -> registry invalid
    FV9  facet evidence path missing              -> registry invalid
    FV10 schema-1 registry declaring facets       -> registry invalid
    FV11 unknown trace event (authority vocab)    -> registry invalid
    FV12 valid faceted registry                   -> no problems
    FV13 trace_events declared, authority import unavailable -> invalid (C5)
    FV14 trace_events declared, KNOWN_EVENTS missing         -> invalid (C5)
    FV15 trace_events declared, KNOWN_EVENTS empty           -> invalid (C5)
    FV16 no trace_events declared -> no dependency on the authority (C5)

Facet routing semantics (task book §11-§16 / §17 B10/B11):

    FR1  DIRECT anchor hit            -> PRECISE, facet targets, trust
    FR2  helper STRUCTURAL hit        -> PRECISE, terminal anchor's facet
    FR3  multi-terminal structural    -> facet UNION (B7 shape)
    FR4  shared anchor, two claims    -> both claims, respective facets (B5)
    FR5  COARSE file-only hit         -> CONSERVATIVE_ALL, parent targets (B8)
    FR6  stale graph demotion         -> CONSERVATIVE_ALL + candidate_facets (B9 shape)
    FR7  unresolved anchor demotion   -> CONSERVATIVE_ALL, never PRECISE (B10)
    FR8  non-faceted claim            -> CONSERVATIVE_ALL, parent targets (B11)
    FR9  no facets in registry at all -> NO facet fields (legacy shape)
    FR10 facet targets always ⊆ parent; claim class never weakened by facets

Corrective-1 corpus gates (PR #303 review C1-C4; live B3 shape = CR8/CR9
is exercised by scripts/formal/fdg0_phase_b_eval.py on the real machine):

    CR1  frozen PRECISE denominator is exactly the 7 prereg row keys
    CR2  B5/F06 participates in the threshold denominator
    CR3  missing B4 row -> integrity failure + non-authoritative threshold
    CR4  specimen exception -> integrity failure
    CR5  duplicate frozen row -> integrity failure
    CR6  prereg comparison reports a missing expected row
    CR7  prereg comparison reports unexpected extra claim rows cleanly

Pure stdlib, synthetic graphs; no scip-clang, no build, no repo mutation.
"""
from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FORMAL_DIR = SCRIPT_DIR.parent / "formal"
sys.path.insert(0, str(FORMAL_DIR))
sys.path.insert(0, str(SCRIPT_DIR))

import formal_impact as fi  # noqa: E402
import fdg0_phase_b_eval as ev  # noqa: E402


# --- synthetic world ----------------------------------------------------------
#
# src/async/fake_sched.cpp defines:
#   Fake::signal   anchor A of FA (facet wake)     refs Fake::we
#   Fake::park     anchor B of FA (facet park)     refs Fake::signal, Fake::helper_inert, Fake::helper_epoch
#   Fake::runloop  anchor D of FA (facet startup)  refs Fake::mid
#   Fake::mid      (NOT an anchor)                 refs Fake::park
#   Fake::we       anchor C of FA (facet wake, state)
#   Fake::helper_inert  (NOT an anchor; refs nothing)        <- FR2 shape
#   Fake::helper_epoch  (NOT an anchor; refs Fake::we)       <- FR3 shape
#   Fake::shared   anchor S of FB (no facets) AND FC (facet fc1)  <- FR4/FR8
# src/other.cpp defines Other::unrelated (bound to FC's suite file-binding only).


def node(sym, display, segs, refs, defs, def_line):
    return {
        "display": display,
        "segments": segs,
        "kind": 0,
        "def_files": defs,
        "def_range": [def_line, 4, def_line, 20],
        "refs": sorted(refs),
    }


S_SIGNAL = "cxx . . $ Fake#signal(1)."
S_PARK = "cxx . . $ Fake#park(2)."
S_RUNLOOP = "cxx . . $ Fake#runloop(3)."
S_WE = "cxx . . $ Fake#we."
S_HELPER_INERT = "cxx . . $ Fake#helper_inert(4)."
S_HELPER_EPOCH = "cxx . . $ Fake#helper_epoch(5)."
S_SHARED = "cxx . . $ Fake#shared(6)."
S_UNRELATED = "cxx . . $ Other#unrelated(7)."
S_MID = "cxx . . $ Fake#mid(8)."


def make_graph(head_sha: str = "0123456789abcdef0123456789abcdef01234567") -> fi.Graph:
    nodes = {
        S_SIGNAL: node(S_SIGNAL, "Fake::signal", ["Fake", "signal"], [S_WE],
                       ["src/async/fake_sched.cpp"], 10),
        S_PARK: node(S_PARK, "Fake::park", ["Fake", "park"],
                     [S_SIGNAL, S_HELPER_INERT, S_HELPER_EPOCH],
                     ["src/async/fake_sched.cpp"], 20),
        S_RUNLOOP: node(S_RUNLOOP, "Fake::runloop", ["Fake", "runloop"], [S_MID],
                        ["src/async/fake_sched.cpp"], 30),
        S_WE: node(S_WE, "Fake::we", ["Fake", "we"], [],
                   ["src/async/fake_sched.cpp"], 5),
        S_HELPER_INERT: node(S_HELPER_INERT, "Fake::helper_inert", ["Fake", "helper_inert"], [],
                             ["src/async/fake_sched.cpp"], 40),
        S_HELPER_EPOCH: node(S_HELPER_EPOCH, "Fake::helper_epoch", ["Fake", "helper_epoch"], [S_WE],
                             ["src/async/fake_sched.cpp"], 50),
        S_SHARED: node(S_SHARED, "Fake::shared", ["Fake", "shared"], [],
                       ["src/async/fake_sched.cpp"], 60),
        S_UNRELATED: node(S_UNRELATED, "Other::unrelated", ["Other", "unrelated"], [],
                          ["src/other.cpp"], 70),
        S_MID: node(S_MID, "Fake::mid", ["Fake", "mid"], [S_PARK],
                    ["src/async/fake_sched.cpp"], 80),
    }
    data = {
        "schema": fi.GRAPH_SCHEMA,
        "head_sha": head_sha,
        "generated_at": "2026-09-06T00:00:00Z",
        "toolchain": {},
        "nodes": nodes,
        "reverse": {
            S_PARK: [S_MID],
            S_MID: [S_RUNLOOP],
            S_SIGNAL: [S_PARK],
            S_HELPER_INERT: [S_PARK],
            S_HELPER_EPOCH: [S_PARK],
            S_WE: [S_HELPER_EPOCH, S_SIGNAL],
            S_SHARED: [],
            S_UNRELATED: [],
        },
        "documents": {
            "src/async/fake_sched.cpp": sorted(nodes)[:-1] + [S_MID],
            "src/other.cpp": [S_UNRELATED],
        },
        "def_positions": {
            "src/async/fake_sched.cpp": [
                [5, 4, S_WE], [10, 4, S_SIGNAL], [20, 4, S_PARK], [30, 4, S_RUNLOOP],
                [40, 4, S_HELPER_INERT], [50, 4, S_HELPER_EPOCH], [60, 4, S_SHARED],
                [80, 4, S_MID],
            ],
            "src/other.cpp": [[70, 4, S_UNRELATED]],
        },
        "stats": {},
    }
    return fi.Graph(data)


def facet_anchor(aid, symbol, file="src/async/fake_sched.cpp", role="authority"):
    return {"id": aid, "file": file, "symbol": symbol, "role": role}


def make_registry() -> dict:
    return {
        "schema_version": 2,
        "claim_class_vocabulary": ["MODEL_PROPERTY"],
        "anchor_roles": ["authority", "state", "gate"],
        "claims": [
            {
                "id": "FA",
                "title": "faceted wake claim",
                "claim_class": "MODEL_PROPERTY",
                "cpp_anchors": [
                    facet_anchor("wake-signal", "Fake::signal"),
                    facet_anchor("park-boundary", "Fake::park"),
                    facet_anchor("startup-settlement", "Fake::runloop"),
                    facet_anchor("wake-epoch-state", "Fake::we", role="state"),
                ],
                "formal_suites": ["s1", "s2", "s3", "s4"],
                "facets": [
                    {
                        "id": "wake",
                        "anchor_refs": ["wake-signal", "wake-epoch-state"],
                        "formal_suites": ["s1", "s2", "s3"],
                        "semantic_labels": {"trace_events": [], "model_actions": []},
                    },
                    {
                        "id": "park",
                        "anchor_refs": ["park-boundary"],
                        "formal_suites": ["s1", "s2", "s4"],
                    },
                    {
                        "id": "startup",
                        "anchor_refs": ["startup-settlement"],
                        "formal_suites": ["s1", "s2"],
                    },
                ],
                "evidence": [],
            },
            {
                "id": "FB",
                "title": "non-faceted shared claim",
                "claim_class": "MODEL_PROPERTY",
                "cpp_anchors": [
                    {"file": "src/async/fake_sched.cpp", "symbol": "Fake::shared", "role": "gate"}
                ],
                "formal_suites": ["s5"],
                "evidence": [],
            },
            {
                "id": "FC",
                "title": "faceted shared claim",
                "claim_class": "MODEL_PROPERTY",
                "cpp_anchors": [facet_anchor("shared-gate", "Fake::shared", role="gate")],
                "formal_suites": ["s1", "s6"],
                "facets": [
                    {
                        "id": "fc1",
                        "anchor_refs": ["shared-gate"],
                        "formal_suites": ["s1", "s6"],
                    }
                ],
                "evidence": [],
            },
        ],
    }


MANIFEST = {
    "suites": [
        {"id": sid, "spec_dir": f"spec/tla/{sid}", "implementation_bindings": bindings}
        for sid, bindings in (
            ("s1", []), ("s2", []), ("s3", []), ("s4", []), ("s5", []),
            # only s6 (an FC suite) carries a file-level binding, so a coarse
            # hit on src/other.cpp contaminates FC but not FA/FB
            ("s6", ["src/other.cpp"]),
        )
    ]
}


def classify(graph, changes, registry=None, max_depth=2, expected_head=None):
    registry = registry or make_registry()
    claim_index = fi.ClaimIndex(registry, MANIFEST)
    families: dict[str, set[str]] = {}
    unresolved: set[str] = set()
    unresolved_gated: set[str] = set()
    facet_index = fi.build_facet_index(registry, graph)
    if graph is not None:
        resolutions = fi.resolve_anchors(registry, graph)
        for claim in registry["claims"]:
            syms: set[str] = set()
            for res in resolutions[claim["id"]]:
                if res["status"] == "resolved":
                    syms.update(res["symbols"])
                elif res["status"] == "UNRESOLVED_ANCHOR":
                    unresolved.add(claim["id"])
                    if res.get("config_gate"):
                        unresolved_gated.add(claim["id"])
            if syms:
                families[claim["id"]] = syms
    return fi.classify_impact(
        graph, None, changes, claim_index, families,
        unresolved_anchor_claims=unresolved,
        unresolved_gated_claims=unresolved_gated,
        max_depth=max_depth,
        expected_head=expected_head,
        facet_index=facet_index,
    )


def changes_for(path: str, hunks, status="M") -> dict:
    return {path: {"status": status, "hunks": list(hunks)}}


def claim_by_id(result, cid):
    return next((c for c in result["claims"] if c["id"] == cid), None)


# --- B12: registry structural negatives ----------------------------------------


class FacetRegistryValidation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "src" / "async").mkdir(parents=True)
        (self.root / "src" / "async" / "fake_sched.cpp").write_text("// fixture\n")
        (self.root / "src" / "other.cpp").write_text("// fixture\n")

    def tearDown(self):
        self.tmp.cleanup()

    def validate(self, registry):
        return fi.validate_registry(registry, MANIFEST, root=self.root)

    def mutate(self, **kw):
        reg = make_registry()
        for path, value in kw.items():
            cursor = reg
            for key in path[:-1]:
                cursor = cursor[key]
            cursor[path[-1]] = value
        return reg

    def test_fv12_valid_faceted_registry_passes(self):
        self.assertEqual(self.validate(make_registry()), [])

    def test_fv1_duplicate_facet_id(self):
        reg = make_registry()
        dup = copy.deepcopy(reg["claims"][0]["facets"][1])
        dup["anchor_refs"] = ["wake-signal"]
        dup["formal_suites"] = ["s1"]
        reg["claims"][0]["facets"].append(dup)
        self.assertTrue(any("duplicate facet id: park" in p for p in self.validate(reg)))

    def test_fv2_duplicate_anchor_id(self):
        reg = make_registry()
        reg["claims"][0]["cpp_anchors"][1]["id"] = "wake-signal"
        self.assertTrue(any("duplicate anchor id: wake-signal" in p for p in self.validate(reg)))

    def test_fv3_unknown_anchor_ref(self):
        reg = make_registry()
        reg["claims"][0]["facets"][0]["anchor_refs"].append("no-such-anchor")
        self.assertTrue(any("unknown anchor_ref: 'no-such-anchor'" in p for p in self.validate(reg)))

    def test_fv4_facet_suite_not_in_parent(self):
        reg = make_registry()
        reg["claims"][0]["facets"][2]["formal_suites"] = ["s1", "s5"]
        self.assertTrue(
            any("suite not in parent claim: s5" in p for p in self.validate(reg)),
            self.validate(reg),
        )

    def test_fv5_parent_suite_omitted_from_every_facet(self):
        reg = make_registry()
        # drop s4 from the park facet: no facet carries s4 anymore
        reg["claims"][0]["facets"][1]["formal_suites"] = ["s1", "s2"]
        self.assertTrue(
            any("omitted from every facet: ['s4']" in p for p in self.validate(reg)),
            self.validate(reg),
        )

    def test_fv6_faceted_anchor_referenced_by_no_facet(self):
        reg = make_registry()
        reg["claims"][0]["facets"][2]["anchor_refs"] = ["park-boundary"]
        problems = self.validate(reg)
        self.assertTrue(any("referenced by no facet: ['startup-settlement']" in p for p in problems), problems)

    def test_fv7_facet_zero_anchors(self):
        reg = make_registry()
        reg["claims"][0]["facets"][2]["anchor_refs"] = []
        self.assertTrue(any("facet startup: zero anchors" in p for p in self.validate(reg)))

    def test_fv8_facet_zero_targets(self):
        reg = make_registry()
        reg["claims"][0]["facets"][2]["formal_suites"] = []
        problems = self.validate(reg)
        self.assertTrue(any("facet startup: zero formal targets" in p for p in problems), problems)

    def test_fv9_facet_evidence_path_missing(self):
        reg = make_registry()
        reg["claims"][0]["facets"][0]["evidence"] = ["docs/no/such/file.md"]
        self.assertTrue(
            any("facet wake: evidence path missing" in p for p in self.validate(reg)),
            self.validate(reg),
        )

    def test_fv10_schema1_with_facets_invalid(self):
        reg = make_registry()
        reg["schema_version"] = 1
        problems = self.validate(reg)
        self.assertTrue(any("facets require registry schema_version 2" in p for p in problems), problems)

    def test_fv11_unknown_trace_event_rejected(self):
        reg = make_registry()
        reg["claims"][0]["facets"][0]["semantic_labels"]["trace_events"] = ["NoSuchTraceEvent"]
        problems = self.validate(reg)
        self.assertTrue(any("unknown trace event" in p for p in problems), problems)

    # C5: the trace-vocabulary authority is a fail-closed dependency.

    @staticmethod
    def poison_e9_authority(mode):
        """Swap the e9_trace_validate module entry for the duration of one
        test: None makes `import` raise (authority unavailable); any other
        value becomes a bare module whose KNOWN_EVENTS is that value (a
        string = attribute absent, a list = empty/usable vocabulary)."""
        import types

        saved = sys.modules.get("e9_trace_validate")
        if mode is None:
            sys.modules["e9_trace_validate"] = None
        else:
            fake = types.ModuleType("e9_trace_validate")
            if not isinstance(mode, list):
                mode = None
            if mode is not None:
                fake.KNOWN_EVENTS = mode
            sys.modules["e9_trace_validate"] = fake

        def restore():
            if saved is None:
                sys.modules.pop("e9_trace_validate", None)
            else:
                sys.modules["e9_trace_validate"] = saved

        return restore

    def with_poisoned_authority(self, mode):
        self.addCleanup(self.poison_e9_authority(mode))

    def test_fv13_trace_events_authority_unavailable_invalid(self):
        reg = make_registry()
        reg["claims"][0]["facets"][0]["semantic_labels"]["trace_events"] = ["WakePublished"]
        self.with_poisoned_authority(None)
        problems = self.validate(reg)
        self.assertTrue(
            any("vocabulary authority (e9_trace_validate) is unavailable" in p
                for p in problems),
            problems,
        )

    def test_fv14_known_events_missing_invalid(self):
        reg = make_registry()
        reg["claims"][0]["facets"][0]["semantic_labels"]["trace_events"] = ["WakePublished"]
        self.with_poisoned_authority("module-without-known-events")
        problems = self.validate(reg)
        self.assertTrue(any("owns no KNOWN_EVENTS vocabulary" in p for p in problems), problems)

    def test_fv15_known_events_empty_invalid(self):
        reg = make_registry()
        reg["claims"][0]["facets"][0]["semantic_labels"]["trace_events"] = ["WakePublished"]
        self.with_poisoned_authority([])
        problems = self.validate(reg)
        self.assertTrue(any("owns no KNOWN_EVENTS vocabulary" in p for p in problems), problems)

    def test_fv16_no_trace_events_declared_needs_no_authority(self):
        # facets declare no trace_events (empty list or absent labels): the
        # registry must validate WITHOUT any dependency on the authority
        reg = make_registry()
        self.with_poisoned_authority(None)
        self.assertEqual(self.validate(reg), [])

    def test_anchor_without_id_on_faceted_claim_rejected(self):
        reg = make_registry()
        del reg["claims"][0]["cpp_anchors"][0]["id"]
        problems = self.validate(reg)
        self.assertTrue(any("anchor without id" in p for p in problems), problems)


# --- FR1-FR10: facet routing semantics ------------------------------------------


class FacetRouting(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.graph = make_graph()

    def test_fr1_direct_anchor_hit_is_precise_with_facet_targets(self):
        # body edit inside Fake::signal (def line 10) -> DIRECT, wake facet
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(12, 12)]))
        self.assertEqual(result["classification"], fi.DIRECT)
        fa = claim_by_id(result, "FA")
        self.assertEqual(fa["facet_scope"], "PRECISE")
        self.assertEqual(fa["reached_anchor_ids"], ["wake-signal"])
        self.assertEqual([f["id"] for f in fa["affected_facets"]], ["wake"])
        self.assertEqual(fa["revalidation_targets"], ["s1", "s2", "s3"])
        # claim class is NOT weakened and parent info is preserved
        self.assertEqual(fa["class"], fi.DIRECT)
        self.assertEqual(fa["formal_suites"], ["s1", "s2", "s3", "s4"])

    def test_fr2_helper_structural_routes_by_terminal_anchor(self):
        # helper_inert (def 40) is called BY park; structural terminal = park
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(42, 42)]))
        self.assertEqual(result["classification"], fi.STRUCTURAL)
        fa = claim_by_id(result, "FA")
        self.assertEqual(fa["facet_scope"], "PRECISE")
        self.assertEqual(fa["reached_anchor_ids"], ["park-boundary"])
        self.assertEqual([f["id"] for f in fa["affected_facets"]], ["park"])
        self.assertEqual(fa["revalidation_targets"], ["s1", "s2", "s4"])
        self.assertNotEqual(set(fa["revalidation_targets"]), set(fa["formal_suites"]))

    def test_fr3_multi_terminal_structural_is_facet_union(self):
        # helper_epoch (def 50): reverse reaches park, forward reaches we
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(52, 52)]))
        fa = claim_by_id(result, "FA")
        self.assertEqual(result["classification"], fi.STRUCTURAL)
        self.assertEqual(fa["facet_scope"], "PRECISE")
        self.assertEqual(sorted(fa["reached_anchor_ids"]), ["park-boundary", "wake-epoch-state"])
        self.assertEqual([f["id"] for f in fa["affected_facets"]], ["park", "wake"])
        self.assertEqual(fa["revalidation_targets"], ["s1", "s2", "s3", "s4"])

    def test_fr4_shared_anchor_surfaces_both_claims_with_own_facets(self):
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(62, 62)]))
        ids = {c["id"] for c in result["claims"]}
        self.assertEqual(ids, {"FB", "FC"})
        fb = claim_by_id(result, "FB")
        self.assertEqual(fb["facet_scope"], "CONSERVATIVE_ALL")
        self.assertEqual(fb["revalidation_targets"], ["s5"])
        fc = claim_by_id(result, "FC")
        self.assertEqual(fc["facet_scope"], "PRECISE")
        self.assertEqual(fc["revalidation_targets"], ["s1", "s6"])

    def test_fr5_coarse_hit_stays_conservative_all(self):
        # hunk before any definition in a bound file -> COARSE fallback only
        result = classify(self.graph, changes_for("src/other.cpp", [(1, 1)]))
        fc = claim_by_id(result, "FC")
        self.assertEqual(result["classification"], fi.COARSE)
        self.assertEqual(fc["facet_scope"], "CONSERVATIVE_ALL")
        self.assertEqual(fc["revalidation_targets"], ["s1", "s6"])
        self.assertNotIn("affected_facets", fc)

    def test_fr5b_coarse_plus_direct_is_conservative_for_the_coarse_claim(self):
        # a coarse contribution disqualifies facet precision FOR THAT CLAIM;
        # another claim hit directly on the same diff stays PRECISE
        changes = changes_for("src/async/fake_sched.cpp", [(12, 12)])
        changes["src/other.cpp"] = {"status": "M", "hunks": [(1, 1)]}
        result = classify(self.graph, changes)
        fc = claim_by_id(result, "FC")
        self.assertEqual(fc["class"], fi.COARSE)
        self.assertEqual(fc["facet_scope"], "CONSERVATIVE_ALL")
        self.assertEqual(fc["revalidation_targets"], ["s1", "s6"])
        self.assertNotIn("candidate_facets", fc)  # no trusted anchor reach to diagnose
        fa = claim_by_id(result, "FA")
        self.assertEqual(fa["facet_scope"], "PRECISE")
        self.assertEqual(fa["revalidation_targets"], ["s1", "s2", "s3"])

    def test_fr6_stale_graph_demotion_keeps_candidate_facets(self):
        result = classify(
            self.graph,
            changes_for("src/async/fake_sched.cpp", [(12, 12)]),
            expected_head="ffffffffffffffffffffffffffffffffffffffff",
        )
        self.assertEqual(result["classification"], fi.UNKNOWN)
        fa = claim_by_id(result, "FA")
        self.assertEqual(fa["class"], fi.UNKNOWN)
        self.assertEqual(fa["facet_scope"], "CONSERVATIVE_ALL")
        self.assertEqual(fa["revalidation_targets"], ["s1", "s2", "s3", "s4"])
        cand = fa.get("candidate_facets")
        self.assertIsNotNone(cand)
        self.assertEqual([f["id"] for f in cand["affected_facets"]], ["wake"])
        self.assertIn("diagnostic only", cand["note"])

    def test_fr7_unresolved_anchor_never_precise(self):
        registry = make_registry()
        registry["claims"][0]["cpp_anchors"][2]["symbol"] = "Fake::renamed_away"
        graph = make_graph()
        result = classify(graph, changes_for("src/async/fake_sched.cpp", [(12, 12)]), registry=registry)
        fa = claim_by_id(result, "FA")
        self.assertEqual(fa["facet_scope"], "CONSERVATIVE_ALL")
        self.assertEqual(fa["revalidation_targets"], ["s1", "s2", "s3", "s4"])
        self.assertIn("candidate_facets", fa)

    def test_fr8_non_faceted_claim_backward_compatible(self):
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(62, 62)]))
        fb = claim_by_id(result, "FB")
        self.assertEqual(fb["class"], fi.DIRECT)
        self.assertEqual(fb["facet_scope"], "CONSERVATIVE_ALL")
        self.assertEqual(fb["revalidation_targets"], ["s5"])
        self.assertNotIn("reached_anchor_ids", fb)

    def test_fr9_legacy_registry_has_no_facet_fields(self):
        registry = make_registry()
        for claim in registry["claims"]:
            claim.pop("facets", None)
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(12, 12)]), registry=registry)
        self.assertEqual(result["classification"], fi.DIRECT)
        for c in result["claims"]:
            self.assertNotIn("facet_scope", c)
            self.assertNotIn("revalidation_targets", c)

    def test_fr10_facet_targets_never_escape_parent(self):
        # invariant sweep over several reach shapes: facet targets are always
        # a subset of the parent suite set, and equal to it for the union case
        for hunk in ([12, 12], [22, 22], [32, 32], [42, 42], [52, 52], [62, 62]):
            result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [tuple(hunk)]))
            for c in result["claims"]:
                if "revalidation_targets" in c:
                    self.assertLessEqual(
                        set(c["revalidation_targets"]),
                        set(c["formal_suites"]),
                        (c["id"], hunk),
                    )
                    self.assertTrue(c["revalidation_targets"], (c["id"], hunk))


class FacetCliJson(unittest.TestCase):
    """The `impact --json` surface carries facet fields end-to-end."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.registry = self.tmp / "registry.json"
        self.manifest = self.tmp / "manifest.json"
        self.graph = self.tmp / "graph.json"
        # validate_registry checks anchor file existence against the REAL
        # repo root, so the CLI fixture anchors to a file that exists there
        # (symbol resolution itself uses the synthetic graph only).
        reg = make_registry()
        for claim in reg["claims"]:
            for anchor in claim["cpp_anchors"]:
                anchor["file"] = "scripts/formal/formal_impact.py"
        self.registry.write_text(json.dumps(reg), encoding="utf-8")
        self.manifest.write_text(json.dumps(MANIFEST), encoding="utf-8")
        self.graph.write_text(
            json.dumps({
                "schema": fi.GRAPH_SCHEMA,
                "head_sha": make_graph().head_sha,
                "generated_at": "2026-09-06T00:00:00Z",
                "toolchain": {},
                "nodes": make_graph().nodes,
                "reverse": make_graph().reverse,
                "documents": make_graph().documents,
                "def_positions": make_graph().def_positions,
                "stats": {},
            }),
            encoding="utf-8",
        )

    def tearDown(self):
        self._tmp.cleanup()

    def run_cli(self, *argv):
        import contextlib
        import io

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = fi.main(list(argv))
        return rc, buf.getvalue()

    def test_json_output_carries_facet_fields(self):
        patch = self.tmp / "p.patch"
        patch.write_text(
            "diff --git a/src/async/fake_sched.cpp b/src/async/fake_sched.cpp\n"
            "--- a/src/async/fake_sched.cpp\n"
            "+++ b/src/async/fake_sched.cpp\n"
            "@@ -12,0 +13,1 @@\n"
            "+touch\n",
            encoding="utf-8",
        )
        rc, out = self.run_cli(
            "impact", "--diff-file", str(patch), "--json",
            "--allow-legacy-graph-for-eval",
            "--graph", str(self.graph),
            "--registry", str(self.registry),
            "--manifest", str(self.manifest),
        )
        self.assertEqual(rc, 0, out)
        result = json.loads(out)
        fa = next(c for c in result["claims"] if c["id"] == "FA")
        self.assertEqual(fa["facet_scope"], "PRECISE")
        self.assertEqual(fa["revalidation_targets"], ["s1", "s2", "s3"])

    def test_explain_prints_facets(self):
        rc, out = self.run_cli(
            "explain", "FA",
            "--graph", str(self.graph),
            "--registry", str(self.registry),
            "--manifest", str(self.manifest),
        )
        self.assertEqual(rc, 0, out)
        self.assertIn("facets (EXPLICIT hand-maintained mapping", out)
        self.assertIn("anchor id: wake-signal", out)
        self.assertIn("formal targets: s1, s2, s3", out)


# --- corrective-1: fail-closed corpus gates (C1/C3/C4) ---------------------------
#
# CR8/CR9 (repaired B3 reaches ONLY wake-epoch-state via wake-publication)
# are live-machine evidence produced by scripts/formal/fdg0_phase_b_eval.py;
# they are recorded in docs/results/formal/fdg0-phase-b.json, not here.


class Corrective1CorpusGates(unittest.TestCase):
    @staticmethod
    def row(spec, claim, reduction=0.0, baseline=6, facet=6,
            klass="DIRECT_FORMAL_IMPACT", scope="PRECISE"):
        return {
            "specimen": spec, "claim": claim, "class": klass,
            "facet_scope": scope, "reached_anchor_ids": [], "affected_facets": [],
            "baseline_targets": baseline, "facet_targets": facet,
            "reduction": reduction, "fail_closed": False,
        }

    def frozen_rows(self, drop=(), extra=(), duplicates=()):
        keys = ev.frozen_precise_row_keys(ev.load_prereg())
        reductions = {"B1a": 0.1667, "B1b": 0.5, "B2": 0.5, "B3": 0.1667,
                      "B4": 0.6667, "B5": 0.3333, "B7": 0.0}
        rows = []
        for spec, claim in keys:
            if (spec, claim) in drop:
                continue
            base = 6 if claim == "F08" else 3
            red = reductions[spec]
            rows.append(self.row(spec, claim, red, base, base - round(red * base)))
        rows.extend(extra)
        for key in duplicates:
            rows.append(self.row(*key))
        return keys, rows

    def test_cr1_frozen_denominator_is_exactly_seven_keys(self):
        keys = ev.frozen_precise_row_keys(ev.load_prereg())
        self.assertEqual(keys, [
            ("B1a", "F08"), ("B1b", "F08"), ("B2", "F08"), ("B3", "F08"),
            ("B4", "F08"), ("B5", "F06"), ("B7", "F08"),
        ])
        self.assertEqual(len(set(keys)), 7)

    def test_cr2_b5_f06_participates_in_threshold(self):
        keys, rows = self.frozen_rows()
        integrity = ev.compute_integrity(rows, [], keys, keys)
        self.assertTrue(integrity["all_pass"], integrity)
        m = ev.measurement_section(rows, keys, integrity)
        self.assertEqual(m["threshold"]["rows_present"], 7)
        self.assertTrue(m["threshold"]["authoritative"])
        self.assertTrue(m["threshold"]["met"])
        detail = {(d["specimen"], d["claim"]) for d in m["precise_b1_b7"]["detail"]}
        self.assertIn(("B5", "F06"), detail)

    def test_cr3_missing_b4_row_fails_corpus_and_threshold(self):
        keys, rows = self.frozen_rows(drop={("B4", "F08")})
        integrity = ev.compute_integrity(rows, [], keys, keys)
        self.assertFalse(integrity["all_pass"])
        self.assertIn({"specimen": "B4", "claim": "F08"},
                      integrity["missing_required_rows"])
        self.assertFalse(integrity["frozen_precise_rows_complete"])
        m = ev.measurement_section(rows, keys, integrity)
        self.assertFalse(m["threshold"]["authoritative"])
        self.assertIsNone(m["threshold"]["met"])

    def test_cr4_specimen_exception_fails_corpus(self):
        keys, rows = self.frozen_rows()
        records = [{"id": "B6b", "error": "RuntimeError: specimen exploded"}]
        integrity = ev.compute_integrity(rows, records, keys, keys)
        self.assertFalse(integrity["all_pass"])
        self.assertEqual(integrity["specimen_errors"],
                         [{"id": "B6b", "error": "RuntimeError: specimen exploded"}])
        self.assertFalse(integrity["frozen_precise_rows_complete"])
        # a record with neither result nor error is equally a failure
        records2 = [{"id": "B8"}]
        integrity2 = ev.compute_integrity(rows, records2, keys, keys)
        self.assertFalse(integrity2["all_pass"])
        self.assertIn("specimen produced no result",
                      integrity2["specimen_errors"][0]["error"])

    def test_cr5_duplicate_frozen_row_is_integrity_failure(self):
        keys, rows = self.frozen_rows(duplicates={("B1a", "F08")})
        integrity = ev.compute_integrity(rows, [], keys, keys)
        self.assertFalse(integrity["all_pass"])
        self.assertEqual(integrity["duplicate_rows"],
                         [{"specimen": "B1a", "claim": "F08", "occurrences": 2}])
        self.assertFalse(integrity["frozen_precise_rows_complete"])

    def test_cr6_prereg_comparison_reports_missing_expected_row(self):
        prereg = ev.load_prereg()
        all_expected = ev.expected_row_keys(prereg)
        self.assertIn(("B4", "F08"), all_expected)
        rows = [self.row(s, c) for s, c in all_expected if (s, c) != ("B4", "F08")]
        comp = ev.compare_with_prereg(rows, prereg)
        self.assertIn({"specimen": "B4", "claim": "F08"},
                      comp["missing_expected_rows"])
        self.assertIn("B4", comp["missing_specimens"])

    def test_cr7_prereg_comparison_reports_unexpected_rows_cleanly(self):
        prereg = ev.load_prereg()
        all_expected = ev.expected_row_keys(prereg)
        rows = [self.row(s, c) for s, c in all_expected]
        # the real B6b extras (F01/F06 reach) plus a fully unknown row
        rows.append(self.row("B6b", "F06", klass="STRUCTURAL_FORMAL_IMPACT"))
        rows.append(self.row("ZZ", "F99"))
        comp = ev.compare_with_prereg(rows, prereg)
        unexpected = {(u["specimen"], u["claim"]) for u in comp["unexpected_rows"]}
        self.assertIn(("B6b", "F06"), unexpected)
        self.assertIn(("ZZ", "F99"), unexpected)
        # unexpected rows are evidence only: not value-compared, so they
        # cannot masquerade as prereg deviations of the expected row
        dev_keys = {(d["specimen"], d["claim"]) for d in comp["deviations"]}
        self.assertNotIn(("ZZ", "F99"), dev_keys)
        # the preregistered B6b/F03 row is still compared normally
        self.assertNotIn(("B6b", "F03"), unexpected)
        # class-only shapes (B9) are compared, not flagged unexpected
        rows.append(self.row("B9", "F08", klass="UNKNOWN_FORMAL_IMPACT",
                             scope="CONSERVATIVE_ALL"))
        rows[-1]["fail_closed"] = True
        comp9 = ev.compare_with_prereg(rows, prereg)
        self.assertNotIn(("B9", "F08"),
                         {(u["specimen"], u["claim"]) for u in comp9["unexpected_rows"]})
        self.assertGreaterEqual(comp9["rows_checked"], comp["rows_checked"] + 1)

    def test_cr_prereg_duplicate_detection(self):
        prereg = ev.load_prereg()
        all_expected = ev.expected_row_keys(prereg)
        rows = [self.row(s, c) for s, c in all_expected]
        rows.append(self.row("B1a", "F08"))
        comp = ev.compare_with_prereg(rows, prereg)
        self.assertEqual(comp["duplicate_rows"],
                         [{"specimen": "B1a", "claim": "F08", "occurrences": 2}])

    def test_cr_real_prereg_expected_rows_are_wellformed(self):
        # sanity on the frozen artifact itself: 17 required live rows,
        # including the three B5 per-claim rows and the four supplementary rows
        keys = ev.expected_row_keys(ev.load_prereg())
        self.assertEqual(len(keys), 17)
        self.assertEqual(len(set(keys)), 17)
        for pair in (("B5", "F01"), ("B5", "F03"), ("B5", "F06"),
                     ("S-04a", "F04"), ("S-04b", "F04"),
                     ("S-06a", "F06"), ("S-06b", "F06")):
            self.assertIn(pair, keys)
        self.assertNotIn(("B9", "F08"), keys)  # class-only shape


if __name__ == "__main__":
    unittest.main(verbosity=2)
