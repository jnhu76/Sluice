#!/usr/bin/env python3
"""Deterministic self-tests for the FTLR-0 formal impact resolver (#299 §17).

Covers S1-S10 plus the corrective-1 exit/provenance contract (R1-R10):

    S1  duplicate claim id                          -> registry invalid
    S2  missing formal suite                        -> registry invalid
    S3  unresolved registered C++ symbol            -> check fails / explicit status
    S4  changed direct anchor                       -> affected claim appears (DIRECT)
    S5  helper -> anchor path                       -> affected claim appears (STRUCTURAL)
    S6  unrelated change                            -> NO_FORMAL_IMPACT
    S7  multi-link (shared authority)               -> both claims appear
    S8  deleted/renamed anchor                      -> fail closed (UNKNOWN, never NO)
    S9  malformed SCIP index / graph                -> fail closed (decode error)
    S10 no SCIP graph available                     -> fail closed UNKNOWN, not NO

    R1  impact UNKNOWN                              -> non-zero exit
    R2  impact UNKNOWN in --json mode               -> non-zero exit
    R3  stale graph                                 -> UNKNOWN + non-zero exit
    R4  stale graph keeps candidates, not authority -> candidate_* fields, class UNKNOWN
    R5  default check without graph                 -> non-zero exit
    R6  check --structure-only without graph        -> zero exit
    R7  helper-only diff (T8 shape)                 -> STRUCTURAL at depth >= 1
    R8  helper-only diff at depth 0                 -> no direct anchor hit (NO)
    R9  provenance in machine-readable results      -> EXPLICIT/COMPILER/HEURISTIC present
    R10 DIRECT/STRUCTURAL/COARSE/NO success paths     -> exit 0

These tests are pure stdlib and operate on synthetic graphs + a minimal
hand-built SCIP byte payload; they do NOT require scip-clang or a build.
The pilot is deliberately NOT wired into pre-push (#299 §18).
"""
from __future__ import annotations

import contextlib
import io
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
import scip_index as si  # noqa: E402


# --- fixtures ---------------------------------------------------------------


def make_graph(head_sha: str = "0123456789abcdef0123456789abcdef01234567") -> fi.Graph:
    """Synthetic repo graph:

    src/async/fake_sched.cpp defines:
        Fake::park        (registered anchor, references Fake::state and
                           Fake::newhelper — the T8 delegation shape)
        Fake::state       (plain data authority)
        Fake::helper      (references Fake::park)          <- depth-1 helper
        Fake::helper2     (references Fake::helper)        <- depth-2 helper
        Fake::shared      (shared authority of two claims)
        Fake::newhelper   (NEW helper the anchor delegates to; NOT an anchor;
                           no callers besides park)         <- R7/R8 T8 shape
        Fake::gone        -- absent (renamed away)         <- S8
    src/other.cpp defines Other::unrelated (no formal relation).
    """
    def node(sym, display, segs, refs, defs, def_line):
        return {
            "display": display,
            "segments": segs,
            "kind": 0,
            "def_files": defs,
            "def_range": [def_line, 4, def_line, 20],
            "refs": sorted(refs),
        }

    park = "cxx . . $ Fake#park(1)."
    state = "cxx . . $ Fake#state."
    helper = "cxx . . $ Fake#helper(2)."
    helper2 = "cxx . . $ Fake#helper2(3)."
    shared = "cxx . . $ Fake#shared(4)."
    unrelated = "cxx . . $ Other#unrelated(5)."
    newhelper = "cxx . . $ Fake#newhelper(6)."
    nodes = {
        park: node(park, "Fake::park", ["Fake", "park"],
                   ["cxx . . $ Fake#state.", newhelper],
                   ["src/async/fake_sched.cpp"], 10),
        state: node(state, "Fake::state", ["Fake", "state"],
                    [], ["src/async/fake_sched.cpp"], 5),
        helper: node(helper, "Fake::helper", ["Fake", "helper"],
                     [park], ["src/async/fake_sched.cpp"], 20),
        helper2: node(helper2, "Fake::helper2", ["Fake", "helper2"],
                      [helper], ["src/async/fake_sched.cpp"], 30),
        shared: node(shared, "Fake::shared", ["Fake", "shared"],
                     [], ["src/async/fake_sched.cpp"], 40),
        unrelated: node(unrelated, "Other::unrelated", ["Other", "unrelated"],
                        [], ["src/other.cpp"], 50),
        newhelper: node(newhelper, "Fake::newhelper", ["Fake", "newhelper"],
                        ["cxx . . $ Fake#state."], ["src/async/fake_sched.cpp"], 60),
    }
    reverse = {
        # park is referenced by helper (helper calls the anchor);
        # newhelper is referenced by park (the anchor delegates to it);
        # helper is referenced by helper2.
        park: [helper],
        helper: [helper2],
        newhelper: [park],
    }
    fake_defs = sorted(nodes)[:6] + [newhelper]
    documents = {
        "src/async/fake_sched.cpp": fake_defs,
        "src/other.cpp": [unrelated],
    }
    def_positions = {
        "src/async/fake_sched.cpp": [
            [10, 4, park],
            [20, 4, helper],
            [30, 4, helper2],
            [40, 4, shared],
            [60, 4, newhelper],
        ],
        "src/other.cpp": [[50, 4, unrelated]],
    }
    data = {
        "schema": fi.GRAPH_SCHEMA,
        "head_sha": head_sha,
        "generated_at": "2026-09-05T00:00:00Z",
        "toolchain": {},
        "nodes": nodes,
        "reverse": reverse,
        "documents": documents,
        "def_positions": def_positions,
        "stats": {},
    }
    return fi.Graph(data)


REGISTRY = {
    "schema_version": 1,
    "claim_class_vocabulary": ["MODEL_PROPERTY"],
    "anchor_roles": ["authority", "state"],
    "claims": [
        {
            "id": "FK1",
            "title": "fake park/wake",
            "claim_class": "MODEL_PROPERTY",
            "cpp_anchors": [
                {"file": "src/async/fake_sched.cpp", "symbol": "Fake::park", "role": "authority"},
            ],
            "formal_suites": ["fake-suite"],
            "evidence": [],
        },
        {
            "id": "FK2",
            "title": "fake shared authority",
            "claim_class": "MODEL_PROPERTY",
            "cpp_anchors": [
                {"file": "src/async/fake_sched.cpp", "symbol": "Fake::shared", "role": "authority"},
            ],
            "formal_suites": ["fake-suite"],
            "evidence": [],
        },
    ],
}

MANIFEST = {
    "suites": [
        {
            "id": "fake-suite",
            "spec_dir": "spec/tla/fake_suite",
            "implementation_bindings": ["src/async/fake_sched.cpp"],
        }
    ]
}


def changes_for(path: str, hunks, status="M") -> dict:
    return {path: {"status": status, "hunks": list(hunks)}}


def classify(graph, changes, registry=None, manifest=None, max_depth=2, expected_head=None):
    registry = registry or REGISTRY
    manifest = manifest or MANIFEST
    claim_index = fi.ClaimIndex(registry, manifest)
    families = {}
    unresolved = set()
    unresolved_gated = set()
    if graph is not None:
        resolutions = fi.resolve_anchors(registry, graph)
        for claim in registry["claims"]:
            syms = set()
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
    )


# --- S1 / S2 / S3: registry validation ---------------------------------------


class RegistryValidation(unittest.TestCase):
    def test_s1_duplicate_claim_id_fails(self):
        registry = json.loads(json.dumps(REGISTRY))
        registry["claims"].append(json.loads(json.dumps(registry["claims"][0])))
        problems = fi.validate_registry(registry, MANIFEST)
        self.assertTrue(any("duplicate claim id: FK1" in p for p in problems), problems)

    def test_s2_missing_formal_suite_fails(self):
        registry = json.loads(json.dumps(REGISTRY))
        registry["claims"][0]["formal_suites"] = ["no-such-suite"]
        problems = fi.validate_registry(registry, MANIFEST)
        self.assertTrue(any("formal suite not in manifest: no-such-suite" in p for p in problems), problems)

    def test_s3_unresolved_anchor_reported(self):
        registry = json.loads(json.dumps(REGISTRY))
        registry["claims"][0]["cpp_anchors"][0]["symbol"] = "Fake::renamed_away"
        graph = make_graph()
        resolutions = fi.resolve_anchors(registry, graph)
        status = resolutions["FK1"][0]["status"]
        self.assertEqual(status, "UNRESOLVED_ANCHOR", status)

    def test_registry_missing_paths_fail(self):
        registry = json.loads(json.dumps(REGISTRY))
        registry["claims"][0]["cpp_anchors"][0]["file"] = "src/does/not/exist.cpp"
        problems = fi.validate_registry(registry, MANIFEST)
        self.assertTrue(any("anchor file missing" in p for p in problems), problems)

    def test_valid_registry_has_no_problems(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src" / "async").mkdir(parents=True)
            (root / "src" / "async" / "fake_sched.cpp").write_text("// fixture\n")
            self.assertEqual(fi.validate_registry(REGISTRY, MANIFEST, root=root), [])


# --- S4-S7: impact classification --------------------------------------------


class ImpactClassification(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.graph = make_graph()

    def test_s4_direct_anchor_hit(self):
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(11, 12)]))
        ids = {c["id"] for c in result["claims"]}
        self.assertEqual(result["classification"], fi.DIRECT)
        self.assertIn("FK1", ids)

    def test_s5_helper_reaches_anchor(self):
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(21, 22)]))
        self.assertEqual(result["classification"], fi.STRUCTURAL, result)
        fk1 = next(c for c in result["claims"] if c["id"] == "FK1")
        self.assertTrue(any(len(p["hops"]) == 2 for p in fk1["paths"]), fk1["paths"])
        self.assertEqual(fk1["provenance"]["structural_attribution"], fi.P_HEURISTIC)

    def test_two_hop_helper_at_depth_2(self):
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(31, 32)]), max_depth=2)
        self.assertEqual(result["classification"], fi.STRUCTURAL, result)
        # depth 0 is not enough for a two-hop helper
        result_d0 = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(31, 32)]), max_depth=0)
        self.assertNotEqual(result_d0["classification"], fi.STRUCTURAL)

    def test_s6_unrelated_change_is_no_impact(self):
        result = classify(self.graph, changes_for("src/other.cpp", [(51, 52)]))
        self.assertEqual(result["classification"], fi.NO_IMPACT, result)
        self.assertEqual(result["claims"], [])
        self.assertEqual(result["risks"], [])

    def test_s7_shared_authority_yields_both_claims(self):
        # Both FK1 and FK2 anchor inside fake_sched.cpp; a body edit
        # attributes to the enclosing def at that position. Extend the
        # registry so FK1 also shares Fake::shared (T5 shape).
        registry = json.loads(json.dumps(REGISTRY))
        registry["claims"][0]["cpp_anchors"].append(
            {"file": "src/async/fake_sched.cpp", "symbol": "Fake::shared", "role": "authority"}
        )
        graph = make_graph()
        result = classify(graph, changes_for("src/async/fake_sched.cpp", [(41, 42)]), registry=registry)
        ids = {c["id"] for c in result["claims"]}
        self.assertEqual(result["classification"], fi.DIRECT)
        self.assertEqual(ids, {"FK1", "FK2"}, ids)

    def test_body_edit_attributes_to_enclosing_function(self):
        # Hunk inside park's body (line 15, def at line 10) attributes via
        # the nearest-preceding definition position.
        result = classify(self.graph, changes_for("src/async/fake_sched.cpp", [(15, 15)]))
        self.assertEqual(result["classification"], fi.DIRECT, result)

    def test_structural_not_top1_only(self):
        # A shared-neighborhood edit must surface every claim reached, not
        # only one (T5/top-1 rule). helper2 changed reaches FK3's anchor at
        # depth 1 (callees) and FK1's anchor at depth 2 — both must appear.
        registry = json.loads(json.dumps(REGISTRY))
        registry["claims"].append({
            "id": "FK3",
            "title": "via helper",
            "claim_class": "MODEL_PROPERTY",
            "cpp_anchors": [
                {"file": "src/async/fake_sched.cpp", "symbol": "Fake::helper", "role": "authority"},
            ],
            "formal_suites": ["fake-suite"],
            "evidence": [],
        })
        result = classify(make_graph(), changes_for("src/async/fake_sched.cpp", [(31, 32)]), registry=registry)
        ids = {c["id"] for c in result["claims"]}
        self.assertIn("FK3", ids)
        self.assertIn("FK1", ids)  # conservative: callee-of-callee anchor also surfaces


# --- S8-S10: fail-closed behavior ---------------------------------------------


class FailClosed(unittest.TestCase):
    def test_s8_renamed_anchor_fails_closed_on_touch(self):
        registry = json.loads(json.dumps(REGISTRY))
        registry["claims"].append({
            "id": "FK4",
            "title": "stale anchor",
            "claim_class": "MODEL_PROPERTY",
            "cpp_anchors": [
                {"file": "src/async/fake_sched.cpp", "symbol": "Fake::gone", "role": "authority"},
            ],
            "formal_suites": ["fake-suite"],
            "evidence": [],
        })
        graph = make_graph()
        claim_index = fi.ClaimIndex(registry, MANIFEST)
        resolutions = fi.resolve_anchors(registry, graph)
        self.assertEqual(resolutions["FK4"][0]["status"], "UNRESOLVED_ANCHOR")
        families: dict[str, set[str]] = {}
        unresolved = {"FK4"}
        for cid, entries in resolutions.items():
            for res in entries:
                if res["status"] == "resolved":
                    families.setdefault(cid, set()).update(res["symbols"])
        result = fi.classify_impact(
            graph, None, changes_for("src/async/fake_sched.cpp", [(11, 12)]),
            claim_index, families, unresolved, set(), max_depth=2, expected_head=None,
        )
        ids = {c["id"]: c["class"] for c in result["claims"]}
        self.assertEqual(ids.get("FK4"), fi.UNKNOWN, ids)  # never silently NO
        self.assertIn("FK4", {c["id"] for c in result["claims"]})

    def test_s9_malformed_scip_bytes_fail_closed(self):
        # A hand-built valid payload parses; truncated/corrupt payload raises.
        payload = build_test_index_bytes()
        meta, docs = si.parse_index(payload)
        self.assertEqual(len(docs), 1)
        with self.assertRaises(ValueError):
            si.parse_index(payload[: len(payload) // 2])
        with self.assertRaises(ValueError):
            si.parse_index(b"\xff\xff\xff")

    def test_s9_malformed_graph_schema_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "graph.json"
            bad.write_text(json.dumps({"schema": "bogus/1", "nodes": {}}), encoding="utf-8")
            with self.assertRaises(fi.ImpactError):
                fi.Graph.load(bad)

    def test_s9_malformed_graph_json_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "graph.json"
            bad.write_text("{not json", encoding="utf-8")
            with self.assertRaises(fi.ImpactError):
                fi.Graph.load(bad)

    def test_s10_missing_graph_is_unknown_not_no(self):
        result = classify(None, changes_for("src/somewhere/new_code.cpp", [(1, 5)]))
        self.assertEqual(result["classification"], fi.UNKNOWN, result)
        self.assertTrue(any("no SCIP-derived graph" in r for r in result["risks"]))

    def test_s10_missing_graph_still_yields_coarse_for_bound_files(self):
        result = classify(None, changes_for("src/async/fake_sched.cpp", [(1, 5)]))
        self.assertEqual(result["classification"], fi.COARSE, result)
        self.assertIn("FK1", {c["id"] for c in result["claims"]})


# --- corrective-1: stale-graph demotion (R3/R4) --------------------------------


class StaleGraphDemotion(unittest.TestCase):
    OTHER_HEAD = "ffffffffffffffffffffffffffffffffffffffff"

    def test_r3_stale_graph_is_unknown(self):
        result = classify(
            make_graph(),
            changes_for("src/async/fake_sched.cpp", [(65, 65)]),
            expected_head=self.OTHER_HEAD,
        )
        self.assertEqual(result["classification"], fi.UNKNOWN, result)
        self.assertEqual(result["unknown_reason"], "UNVERIFIED_STALE_GRAPH")
        self.assertTrue(result["stale_index"])
        self.assertTrue(result["fail_closed"])
        self.assertTrue(
            any("UNVERIFIED_STALE_GRAPH" in r for r in result["fail_closed_reasons"]),
            result["fail_closed_reasons"],
        )

    def test_r4_stale_graph_keeps_candidates_not_authority(self):
        result = classify(
            make_graph(),
            changes_for("src/async/fake_sched.cpp", [(65, 65)]),
            expected_head=self.OTHER_HEAD,
        )
        fk1 = next(c for c in result["claims"] if c["id"] == "FK1")
        self.assertEqual(fk1["class"], fi.UNKNOWN)  # never authoritative on a stale graph
        self.assertEqual(fk1["candidate_class"], fi.STRUCTURAL)
        self.assertEqual(fk1["unverified_reason"], "UNVERIFIED_STALE_GRAPH")
        self.assertEqual(result["candidate_classification"], fi.STRUCTURAL)
        self.assertEqual(result["candidate_claims"], ["FK1"])
        self.assertIn("stale graph", result["candidate_reason"])


# --- corrective-1: helper-only diff, the corrected T8 shape (R7/R8) -------------


class HelperOnlyT8Shape(unittest.TestCase):
    """The corrective-1 C3 fixture: the changed symbol is a NEW helper the
    anchor delegates to; the helper itself is NOT registered."""

    def changes(self):
        # Body hunk inside Fake::newhelper (def at line 60) — attributes to
        # the enclosing helper definition, never to the anchor.
        return changes_for("src/async/fake_sched.cpp", [(65, 65)])

    def test_r7_helper_only_diff_is_structural_at_depth_1(self):
        result = classify(make_graph(), self.changes(), max_depth=1)
        self.assertEqual(result["classification"], fi.STRUCTURAL, result)
        self.assertEqual({c["id"] for c in result["claims"]}, {"FK1"})
        fk1 = result["claims"][0]
        self.assertTrue(any(len(p["hops"]) == 2 for p in fk1["paths"]), fk1["paths"])

    def test_r8_helper_only_diff_at_depth_0_reports_no_direct_hit(self):
        result = classify(make_graph(), self.changes(), max_depth=0)
        self.assertNotIn(fi.DIRECT, [c["class"] for c in result["claims"]])
        self.assertEqual(result["claims"], [])
        self.assertEqual(result["classification"], fi.NO_IMPACT, result)

    def test_changed_symbol_is_the_helper_not_the_anchor(self):
        graph = make_graph()
        syms = fi.changed_symbols(graph, self.changes())
        self.assertEqual(len(syms), 1, syms)
        self.assertNotIn("cxx . . $ Fake#park(1).", syms)
        self.assertEqual(syms["cxx . . $ Fake#newhelper(6)."]["attribution"], fi.P_HEURISTIC)


# --- corrective-1: provenance in machine-readable results (R9) ------------------


class ProvenanceReporting(unittest.TestCase):
    def test_r9_structural_path_carries_heuristic_provenance(self):
        result = classify(make_graph(), changes_for("src/async/fake_sched.cpp", [(65, 65)]))
        fk1 = next(c for c in result["claims"] if c["id"] == "FK1")
        path = fk1["paths"][0]
        self.assertTrue(path["uses_heuristic_attribution"])
        self.assertEqual(path["provenance"], fi.P_HEURISTIC)
        self.assertEqual(path["hop_provenance"], [fi.P_HEURISTIC, fi.P_HEURISTIC])
        self.assertEqual(fk1["provenance"]["anchor_binding"], fi.P_EXPLICIT)
        self.assertEqual(fk1["provenance"]["symbol_identity"], fi.P_COMPILER)

    def test_r9_direct_defrange_hit_is_compiler_provenance(self):
        result = classify(make_graph(), changes_for("src/async/fake_sched.cpp", [(11, 12)]))
        fk1 = next(c for c in result["claims"] if c["id"] == "FK1")
        path = fk1["paths"][0]
        self.assertFalse(path["uses_heuristic_attribution"])
        self.assertEqual(path["provenance"], fi.P_COMPILER)
        self.assertEqual(path["hop_provenance"], [fi.P_COMPILER])

    def test_r9_legend_present_in_result(self):
        result = classify(make_graph(), changes_for("src/other.cpp", [(51, 52)]))
        self.assertEqual(
            set(result["provenance_legend"]),
            {fi.P_EXPLICIT, fi.P_COMPILER, fi.P_HEURISTIC, fi.P_FALLBACK, fi.P_BUILD},
        )

    def test_coarse_hit_records_fallback_provenance(self):
        # Graph missing + bound file: COARSE with the FALLBACK marker and a
        # fail-closed MISSING_GRAPH condition (exit contract row 5).
        result = classify(None, changes_for("src/async/fake_sched.cpp", [(1, 5)]))
        fk1 = next(c for c in result["claims"] if c["id"] == "FK1")
        self.assertEqual(fk1["provenance"].get("coarse_mapping"), fi.P_FALLBACK)
        self.assertTrue(result["fail_closed"])
        self.assertTrue(any("MISSING_GRAPH" in r for r in result["fail_closed_reasons"]))


# --- corrective-1: CLI exit contract (R1/R2/R5/R6/R10) --------------------------

CLI_REGISTRY = {
    "schema_version": 1,
    "claim_class_vocabulary": ["MODEL_PROPERTY"],
    "anchor_roles": ["authority", "state"],
    "claims": [
        {
            "id": "FK1",
            "title": "fake park/wake",
            "claim_class": "MODEL_PROPERTY",
            # Anchored to a REAL repo file: validate_registry checks path
            # existence against the repository root.
            "cpp_anchors": [
                {"file": "scripts/formal/formal_impact.py", "symbol": "Fake::park", "role": "authority"},
            ],
            "formal_suites": ["fake-suite"],
            "evidence": [],
        },
    ],
}

CLI_MANIFEST = {
    "suites": [
        {
            "id": "fake-suite",
            "spec_dir": "spec/tla/fake_suite",
            "implementation_bindings": ["src/async/fake_sched.cpp"],
        }
    ]
}


def diff_patch(path: str, start: int, count: int = 1) -> str:
    return (
        f"diff --git a/{path} b/{path}\n"
        f"--- a/{path}\n"
        f"+++ b/{path}\n"
        f"@@ -{start},0 +{start},{count} @@ ctx\n"
        + "".join(f"+added line {i}\n" for i in range(count))
    )


class ExitContract(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.registry = self.tmp / "registry.json"
        self.manifest = self.tmp / "manifest.json"
        self.graph = self.tmp / "graph.json"
        self.registry.write_text(json.dumps(CLI_REGISTRY), encoding="utf-8")
        self.manifest.write_text(json.dumps(CLI_MANIFEST), encoding="utf-8")
        self.write_graph_from(make_graph())

    def tearDown(self):
        self._tmp.cleanup()

    def write_graph_from(self, graph: fi.Graph):
        self.graph.write_text(
            json.dumps({
                "schema": fi.GRAPH_SCHEMA,
                "head_sha": graph.head_sha,
                "generated_at": graph.generated_at,
                "toolchain": {},
                "nodes": graph.nodes,
                "reverse": graph.reverse,
                "documents": graph.documents,
                "def_positions": graph.def_positions,
                "stats": {},
            }),
            encoding="utf-8",
        )

    def run_cli(self, *argv: str):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = fi.main(list(argv))
        return rc, buf.getvalue()

    def art(self):
        return [
            "--graph", str(self.graph),
            "--registry", str(self.registry),
            "--manifest", str(self.manifest),
        ]

    def test_r1_unknown_impact_exits_nonzero(self):
        patch = self.tmp / "unknown.patch"
        patch.write_text(diff_patch("src/new_unindexed.cpp", 1, 2), encoding="utf-8")
        rc, out = self.run_cli("impact", "--diff-file", str(patch), *self.art())
        self.assertEqual(rc, 1, out)
        self.assertIn("UNKNOWN_FORMAL_IMPACT", out)
        self.assertIn("FAIL-CLOSED", out)

    def test_r2_unknown_impact_json_mode_exits_nonzero(self):
        patch = self.tmp / "unknown.patch"
        patch.write_text(diff_patch("src/new_unindexed.cpp", 1, 2), encoding="utf-8")
        rc, out = self.run_cli("impact", "--diff-file", str(patch), "--json", *self.art())
        self.assertEqual(rc, 1, out)
        result = json.loads(out)
        self.assertEqual(result["classification"], fi.UNKNOWN)
        self.assertTrue(result["fail_closed"])
        self.assertEqual(result["semantic_disposition"], "UNDETERMINED")

    def test_unknown_with_eval_optin_exits_zero(self):
        patch = self.tmp / "unknown.patch"
        patch.write_text(diff_patch("src/new_unindexed.cpp", 1, 2), encoding="utf-8")
        rc, out = self.run_cli(
            "impact", "--diff-file", str(patch), "--allow-unknown-for-eval", *self.art()
        )
        self.assertEqual(rc, 0, out)
        self.assertIn("FAIL-CLOSED", out)  # condition still reported

    def test_r3_stale_graph_cli_exits_nonzero(self):
        patch = self.tmp / "helper.patch"
        patch.write_text(diff_patch("src/async/fake_sched.cpp", 65), encoding="utf-8")
        rc, out = self.run_cli(
            "impact", "--diff-file", str(patch), "--assume-head",
            "ffffffffffffffffffffffffffffffffffffffff", "--json", *self.art(),
        )
        self.assertEqual(rc, 1, out)
        result = json.loads(out)
        self.assertEqual(result["classification"], fi.UNKNOWN)
        self.assertEqual(result["unknown_reason"], "UNVERIFIED_STALE_GRAPH")

    def test_r4_stale_graph_json_keeps_candidates(self):
        patch = self.tmp / "helper.patch"
        patch.write_text(diff_patch("src/async/fake_sched.cpp", 65), encoding="utf-8")
        rc, out = self.run_cli(
            "impact", "--diff-file", str(patch), "--assume-head",
            "ffffffffffffffffffffffffffffffffffffffff", "--json", *self.art(),
        )
        self.assertEqual(rc, 1, out)
        result = json.loads(out)
        self.assertEqual(result["candidate_classification"], fi.STRUCTURAL)
        self.assertEqual(result["candidate_claims"], ["FK1"])
        fk1 = next(c for c in result["claims"] if c["id"] == "FK1")
        self.assertEqual(fk1["class"], fi.UNKNOWN)
        self.assertEqual(fk1["candidate_class"], fi.STRUCTURAL)

    def test_r5_default_check_without_graph_fails(self):
        rc, out = self.run_cli(
            "check", "--graph", str(self.tmp / "missing.json"),
            "--registry", str(self.registry), "--manifest", str(self.manifest),
        )
        self.assertEqual(rc, 1, out)
        self.assertIn("FAIL", out)

    def test_r6_structure_only_check_without_graph_passes(self):
        rc, out = self.run_cli(
            "check", "--structure-only",
            "--registry", str(self.registry), "--manifest", str(self.manifest),
        )
        self.assertEqual(rc, 0, out)
        self.assertIn("structure-only", out)

    def test_default_check_with_fresh_graph_passes(self):
        self.write_graph_from(make_graph(head_sha=fi.git_rev("HEAD")))
        rc, out = self.run_cli("check", *self.art())
        self.assertEqual(rc, 0, out)
        self.assertIn("all registered anchors resolve", out)

    def test_default_check_with_stale_graph_fails(self):
        rc, out = self.run_cli("check", *self.art())
        self.assertEqual(rc, 1, out)
        self.assertIn("stale", out)

    def test_r10_success_paths_exit_zero(self):
        # COARSE: hunk BEFORE any definition (no symbol attribution at all)
        # in a bound file -> file-level fallback hit, exit 0 with graph.
        cases = {
            "direct.patch": (11, fi.DIRECT),
            "structural.patch": (65, fi.STRUCTURAL),
            "coarse.patch": (1, fi.COARSE),
        }
        for name, (start, expected) in cases.items():
            patch = self.tmp / name
            patch.write_text(diff_patch("src/async/fake_sched.cpp", start), encoding="utf-8")
            rc, out = self.run_cli("impact", "--diff-file", str(patch), "--json", *self.art())
            self.assertEqual(rc, 0, f"{name}: {out}")
            result = json.loads(out)
            self.assertEqual(result["classification"], expected, name)
            self.assertFalse(result["fail_closed"], name)

    def test_r10_no_impact_exits_zero(self):
        patch = self.tmp / "other.patch"
        patch.write_text(diff_patch("src/other.cpp", 51), encoding="utf-8")
        rc, out = self.run_cli("impact", "--diff-file", str(patch), "--json", *self.art())
        self.assertEqual(rc, 0, out)
        self.assertEqual(json.loads(out)["classification"], fi.NO_IMPACT)


# --- diff parsing + SCIP symbol naming ----------------------------------------


class DiffParsing(unittest.TestCase):
    DIFF = """diff --git a/src/async/fake_sched.cpp b/src/async/fake_sched.cpp
index 111..222 100644
--- a/src/async/fake_sched.cpp
+++ b/src/async/fake_sched.cpp
@@ -12,0 +13,2 @@ some context
+added line one
+added line two
diff --git a/src/gone.cpp b/src/gone.cpp
deleted file mode 100644
index 333..000
--- a/src/gone.cpp
+++ /dev/null
@@ -1,3 +0,0 @@
-deleted a
-deleted b
-deleted c
"""

    def test_hunk_ranges_are_new_side_inclusive(self):
        changes = fi.diff_to_changes(self.DIFF)
        self.assertIn("src/async/fake_sched.cpp", changes)
        self.assertEqual(changes["src/async/fake_sched.cpp"]["hunks"], [(13, 14)])
        self.assertIn("src/gone.cpp", changes)
        self.assertEqual(changes["src/gone.cpp"]["status"], "D")
        self.assertEqual(changes["src/gone.cpp"]["hunks"], [(1, 1)])  # deletion point

    def test_scip_symbol_segments_real_format(self):
        sym = "cxx . . $ sluice/async/Scheduler#signal_wake_locked(49f6e7a06ebc5aa8)."
        self.assertEqual(si.symbol_segments(sym), ["sluice", "async", "Scheduler", "signal_wake_locked"])
        self.assertEqual(si.display_name(sym), "sluice::async::Scheduler::signal_wake_locked")

    def test_scip_symbol_segments_excludes_locals_and_files(self):
        self.assertIsNone(si.symbol_segments("local 7"))
        self.assertIsNone(si.symbol_segments("cxx . . $ `<file>/src/x.cpp`/"))

    def test_anchor_matching_is_segment_suffix(self):
        self.assertTrue(si.matches_anchor(["sluice", "async", "Scheduler", "park"], ["Scheduler", "park"]))
        self.assertFalse(si.matches_anchor(["Scheduler", "park"], ["Fake", "park"]))


# --- minimal SCIP encoder (round-trip fixture for S9) --------------------------


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def encode_field(field_number: int, payload: bytes) -> bytes:
    key = encode_varint((field_number << 3) | 2)
    return key + encode_varint(len(payload)) + payload


def build_test_index_bytes() -> bytes:
    """A minimal but valid SCIP index: one document, one definition
    occurrence, one reference occurrence, one SymbolInformation."""
    packed_range = encode_varint(4) + encode_varint(0) + encode_varint(8)  # single-line
    occurrence = encode_field(1, packed_range)
    occurrence += encode_field(2, b"cxx . . $ Fake#park(1).")
    occurrence += encode_field(3, encode_varint(1))
    symbol_info = encode_field(1, b"cxx . . $ Fake#park(1).")
    document = encode_field(1, b"src/async/fake_sched.cpp")
    document += encode_field(2, occurrence)
    document += encode_field(3, symbol_info)
    index = encode_field(2, document)
    return index


if __name__ == "__main__":
    unittest.main(verbosity=2)
