#!/usr/bin/env python3
"""Deterministic self-tests for the FTLR-0 formal impact resolver (#299 §17).

Covers S1-S10:

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

These tests are pure stdlib and operate on synthetic graphs + a minimal
hand-built SCIP byte payload; they do NOT require scip-clang or a build.
The pilot is deliberately NOT wired into pre-push (#299 §18).
"""
from __future__ import annotations

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


def make_graph() -> fi.Graph:
    """Synthetic repo graph:

    src/async/fake_sched.cpp defines:
        Fake::park        (registered anchor, references Fake::state)
        Fake::helper      (references Fake::park)          <- depth-1 helper
        Fake::helper2     (references Fake::helper)        <- depth-2 helper
        Fake::shared      (shared authority of two claims)
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

    nodes = {
        "cxx . . $ Fake#park(1).": node(
            "cxx . . $ Fake#park(1).", "Fake::park", ["Fake", "park"],
            ["cxx . . $ Fake#state."], ["src/async/fake_sched.cpp"], 10),
        "cxx . . $ Fake#state.": node(
            "cxx . . $ Fake#state.", "Fake::state", ["Fake", "state"],
            [], ["src/async/fake_sched.cpp"], 5),
        "cxx . . $ Fake#helper(2).": node(
            "cxx . . $ Fake#helper(2).", "Fake::helper", ["Fake", "helper"],
            ["cxx . . $ Fake#park(1)."], ["src/async/fake_sched.cpp"], 20),
        "cxx . . $ Fake#helper2(3).": node(
            "cxx . . $ Fake#helper2(3).", "Fake::helper2", ["Fake", "helper2"],
            ["cxx . . $ Fake#helper(2)."], ["src/async/fake_sched.cpp"], 30),
        "cxx . . $ Fake#shared(4).": node(
            "cxx . . $ Fake#shared(4).", "Fake::shared", ["Fake", "shared"],
            [], ["src/async/fake_sched.cpp"], 40),
        "cxx . . $ Other#unrelated(5).": node(
            "cxx . . $ Other#unrelated(5).", "Other::unrelated", ["Other", "unrelated"],
            [], ["src/other.cpp"], 50),
    }
    reverse = {
        "cxx . . $ Fake#park(1).": ["cxx . . $ Fake#helper(2)."],
        "cxx . . $ Fake#helper(2).": ["cxx . . $ Fake#helper2(3)."],
    }
    documents = {
        "src/async/fake_sched.cpp": sorted(nodes)[0:5],
        "src/other.cpp": ["cxx . . $ Other#unrelated(5)."],
    }
    def_positions = {
        "src/async/fake_sched.cpp": [
            [10, 4, "cxx . . $ Fake#park(1)."],
            [20, 4, "cxx . . $ Fake#helper(2)."],
            [30, 4, "cxx . . $ Fake#helper2(3)."],
            [40, 4, "cxx . . $ Fake#shared(4)."],
        ],
        "src/other.cpp": [[50, 4, "cxx . . $ Other#unrelated(5)."]],
    }
    data = {
        "schema": fi.GRAPH_SCHEMA,
        "head_sha": "0123456789abcdef0123456789abcdef01234567",
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
    if graph is not None:
        resolutions = fi.resolve_anchors(registry, graph)
        for claim in registry["claims"]:
            syms = set()
            for res in resolutions[claim["id"]]:
                if res["status"] == "resolved":
                    syms.update(res["symbols"])
            if syms:
                families[claim["id"]] = syms
    return fi.classify_impact(
        graph, None, changes, claim_index, families,
        unresolved_anchor_claims=set(), max_depth=max_depth,
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
        self.assertTrue(any(len(p) == 2 for p in fk1["paths"]), fk1["paths"])

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
            claim_index, families, unresolved, max_depth=2, expected_head=None,
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
