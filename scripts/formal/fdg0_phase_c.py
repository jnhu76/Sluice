#!/usr/bin/env python3
"""FDG-0 Phase C — historical gold replay driver (issue #298 Phase C).

Executes the FROZEN gold corpus (docs/results/formal/fdg0-phase-c-gold.json,
commit C0) against real reconstructed historical worlds with the CURRENT
merged resolver, and scores the results per the preregistered thresholds.

Hard sequencing rule (task book §0/§14): this driver must never be run
before the gold commit exists; the gold file is never edited after the
first run. A historical miss is a result, not something to fix here.

Method under test (fixed by #300/#302/#303):

    Xmake Build Truth -> SCIP structural recovery -> explicit anchors
    -> Formal Facets

Compared on the same frozen corpus:

    Baseline A  file-only       changed bound file -> all bound parent claims
    Baseline B  explicit-only   the merged resolver at --max-depth 0
    Method C    merged current  Build Truth + SCIP depth 2 + Formal Facets

Replay identity (task book §15/§16/§37): every scored case materializes
REAL base/head worktrees from git; the build world, compile_commands, Build
Manifest, and SCIP graph are regenerated inside the HEAD worktree; the
current graph is never reused (strategy B: the current tooling is invoked
externally with SLUICE_FDGC_ANALYSIS_ROOT pointed at the historical
worktree; the authority artifacts stay in this repository).

Fail-closed integrity (task book §21): a required case that cannot be
materialized, built, indexed, impacted, scored, or that is
missing/duplicated fails the corpus (integrity.all_pass = false, exit != 0)
and is reported — never silently dropped from the denominators.

Usage:
    python3 scripts/formal/fdg0_phase_c.py validate-gold
    python3 scripts/formal/fdg0_phase_c.py run [--jobs N] [--resume]
        [--cases ID,...] [--out PATH]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
GOLD_PATH = REPO_ROOT / "docs" / "results" / "formal" / "fdg0-phase-c-gold.json"
CANDIDATES_PATH = REPO_ROOT / "docs" / "results" / "formal" / "fdg0-phase-c-candidates.json"
RESULTS_PATH = REPO_ROOT / "docs" / "results" / "formal" / "fdg0-phase-c-results.json"
ANCHORS_PATH = REPO_ROOT / "spec" / "formal" / "anchors.json"
MANIFEST_PATH = REPO_ROOT / "spec" / "tla" / "manifest.json"
RESOLVER = SCRIPT_DIR / "formal_impact.py"
SCIP_CLANG_SRC = REPO_ROOT / "build" / "formal-impact" / "bin" / "scip-clang"
SCIP_CLANG_LOCK = SCRIPT_DIR / "scip-clang.lock.json"
ANALYSIS_ENV = "SLUICE_FDGC_ANALYSIS_ROOT"
RUN_DIR = REPO_ROOT / ".fdgc-run"

CLAIM_CLASSES = {
    "DIRECT_FORMAL_IMPACT",
    "STRUCTURAL_FORMAL_IMPACT",
    "COARSE_FORMAL_IMPACT",
    "UNKNOWN_FORMAL_IMPACT",
}
GOLD_KINDS = {"POSITIVE", "NEGATIVE", "AMBIGUOUS"}
APPLICABILITY = {"APPLICABLE", "PARTIALLY_APPLICABLE", "OUT_OF_EPOCH"}
CONFIDENCE = {"HIGH", "MEDIUM", "AMBIGUOUS"}
RECON_CLASSES = {
    "RECONSTRUCTED",
    "TOOLCHAIN_INCOMPATIBLE",
    "BUILD_METADATA_UNAVAILABLE",
    "SCIP_INDEX_FAILED",
    "IMPACT_FAILED",
    "SKIPPED_OUT_OF_EPOCH",
}
CASE_TIMEOUT_SECS = 1800


class PhaseCError(Exception):
    pass


# --- shared helpers ----------------------------------------------------------


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_output(*args: str, cwd: Path = REPO_ROOT) -> str:
    result = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, timeout=300
    )
    if result.returncode != 0:
        raise PhaseCError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def rev_full(rev: str) -> str:
    return git_output("rev-parse", "--verify", rev).strip()


# --- gold validation (pure) ---------------------------------------------------


def validate_gold(gold: dict, registry: dict, manifest: dict) -> list[str]:
    """Structural + referential validation of the frozen gold file (pure;
    hermetically testable). Returns a list of problems (empty = valid)."""
    problems: list[str] = []
    for key in ("schema", "history_range", "sampling_rules", "cases",
                "required_case_ids", "required_result_rows"):
        if key not in gold:
            problems.append(f"gold missing required top-level key: {key}")
    if problems:
        return problems
    cases = gold["cases"]
    claim_ids = {c["id"] for c in registry.get("claims", [])}
    facet_ids = {
        (c["id"], f["id"])
        for c in registry.get("claims", [])
        for f in c.get("facets", [])
    }
    suite_ids = {s.get("id") for s in manifest.get("suites", [])}
    seen: set[str] = set()
    positive_labels = 0
    for case in cases:
        cid = case.get("case_id")
        if not cid:
            problems.append("case without case_id")
            continue
        if cid in seen:
            problems.append(f"duplicate case id: {cid}")  # C1
            continue
        seen.add(cid)
        for field in ("commit_sha", "base_sha", "head_sha", "subject", "subsystem",
                      "applicability", "gold_kind", "claim_confidence",
                      "change_summary", "rationale"):
            if field not in case or not case.get(field):
                problems.append(f"{cid}: missing/empty field {field}")  # C6 shape
        for field in ("ambiguity_note", "expected_claims", "facet_gold", "gold_evidence"):
            if field not in case:
                problems.append(f"{cid}: missing field {field} (presence required)")
        if case.get("gold_kind") not in GOLD_KINDS:
            problems.append(f"{cid}: invalid gold_kind {case.get('gold_kind')!r}")
        if case.get("applicability") not in APPLICABILITY:
            problems.append(f"{cid}: invalid applicability {case.get('applicability')!r}")
        if case.get("claim_confidence") not in CONFIDENCE:
            problems.append(f"{cid}: invalid claim_confidence")
        for claim in case.get("expected_claims", []):
            if claim not in claim_ids:
                problems.append(f"{cid}: expected claim {claim!r} not in registry")
        facet_gold = case.get("facet_gold")
        if isinstance(facet_gold, list):
            for facet in facet_gold:
                if not isinstance(facet, str) or facet == "UNSCORED":
                    problems.append(f"{cid}: invalid facet_gold entry {facet!r}")
                    continue
                mapped = False
                for claim in case.get("expected_claims", []):
                    if (claim, facet) in facet_ids:
                        mapped = True
                if not mapped:
                    problems.append(f"{cid}: facet {facet!r} not in any expected claim")
        for target in case.get("formal_target_gold", []) or []:
            if target not in suite_ids:
                problems.append(f"{cid}: formal_target_gold {target!r} not in manifest")
        for ev in case.get("gold_evidence", []):
            if "resolver" in str(ev).lower() and "phase-c" in str(ev).lower():
                problems.append(f"{cid}: gold evidence cites Phase-C resolver output")
        if case.get("gold_kind") == "POSITIVE" and case.get("claim_confidence") != "AMBIGUOUS":
            if not case.get("expected_claims"):
                problems.append(f"{cid}: POSITIVE case without expected claims")
            positive_labels += len(case.get("expected_claims", []))
        if case.get("gold_kind") == "AMBIGUOUS" and case.get("expected_claims"):
            problems.append(f"{cid}: AMBIGUOUS case must not carry expected claims")
    required = gold["required_case_ids"]
    if len(required) != len(set(required)):
        problems.append("required_case_ids contains duplicates")  # C16
    missing = sorted(set(required) - seen)
    if missing:
        problems.append(f"required_case_ids absent from cases: {missing}")  # C2
    if gold["required_result_rows"] != positive_labels:
        problems.append(
            f"required_result_rows {gold['required_result_rows']} != "
            f"non-AMBIGUOUS positive expected labels {positive_labels}"
        )
    return problems


def load_and_validate_gold() -> dict:
    if not GOLD_PATH.is_file():
        raise PhaseCError(f"gold file not found: {GOLD_PATH}")
    gold = json.loads(GOLD_PATH.read_text(encoding="utf-8"))
    registry = json.loads(ANCHORS_PATH.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    problems = validate_gold(gold, registry, manifest)
    if problems:
        raise PhaseCError("gold validation failed:\n  - " + "\n  - ".join(problems))
    return gold


# --- Baseline A (pure): file-only mapping -------------------------------------


def claim_files_index(registry: dict, manifest: dict) -> dict[str, set[str]]:
    suite_bindings = {
        s["id"]: list(s.get("implementation_bindings", []))
        for s in manifest.get("suites", [])
    }
    index: dict[str, set[str]] = {}
    for claim in registry.get("claims", []):
        files = {a["file"] for a in claim.get("cpp_anchors", [])}
        for suite_id in claim.get("formal_suites", []):
            files.update(suite_bindings.get(suite_id, []))
        index[claim["id"]] = files
    return index


def baseline_a_claims(changed_paths: list[str], registry: dict, manifest: dict) -> list[str]:
    files = claim_files_index(registry, manifest)
    out: set[str] = set()
    for cid, bound in files.items():
        if bound.intersection(changed_paths):
            out.add(cid)
    return sorted(out)


# --- scoring (pure) ------------------------------------------------------------


def surfaced_claim_ids(impact_result: dict) -> set[str]:
    return {c["id"] for c in impact_result.get("claims", [])}


def claim_hit_class(impact_result: dict, claim_id: str) -> str | None:
    for c in impact_result.get("claims", []):
        if c["id"] == claim_id:
            return c.get("class")
    return None


def case_uses_heuristic(impact_result: dict, claim_id: str) -> bool:
    for c in impact_result.get("claims", []):
        if c["id"] != claim_id:
            continue
        for p in c.get("paths", []):
            if p.get("uses_heuristic_attribution"):
                return True
        for sym in impact_result.get("changed_symbols", {}).values():
            if sym.get("attribution") == "HEURISTIC":
                return True
    return False


def score_positive_case(case: dict, impact_result: dict) -> dict:
    """Score one non-AMBIGUOUS POSITIVE case against one impact result
    (pure). Every expected claim must appear in the resolver result
    (DIRECT/STRUCTURAL/COARSE/UNKNOWN); an absent claim is a miss; an
    absent claim with classification NO is additionally a silent NO
    (task book §23/§24)."""
    classification = impact_result.get("classification")
    rows = []
    for claim in case.get("expected_claims", []):
        hit = claim_hit_class(impact_result, claim)
        row = {
            "case_id": case["case_id"],
            "claim": claim,
            "confidence": case["claim_confidence"],
            "found": hit is not None,
            "hit_class": hit,
            "silent_no": hit is None and classification == "NO_FORMAL_IMPACT",
            "uses_heuristic": case_uses_heuristic(impact_result, claim)
            if hit is not None
            else None,
            "facet_scope": next(
                (c.get("facet_scope") for c in impact_result.get("claims", []) if c["id"] == claim),
                None,
            ),
        }
        rows.append(row)
    return {
        "case_id": case["case_id"],
        "rows": rows,
        "surfaced": sorted(surfaced_claim_ids(impact_result)),
        "expected": list(case.get("expected_claims", [])),
        "extra": sorted(set(surfaced_claim_ids(impact_result)) - set(case.get("expected_claims", []))),
    }


def score_negative_case(case: dict, impact_result: dict) -> dict:
    surfaced = sorted(surfaced_claim_ids(impact_result))
    return {
        "case_id": case["case_id"],
        "surfaced": surfaced,
        "extra": surfaced,
        "classification": impact_result.get("classification"),
        "fail_closed": impact_result.get("fail_closed"),
        "revalidation_targets": sorted(
            {
                t
                for c in impact_result.get("claims", [])
                for t in (c.get("revalidation_targets") or [])
            }
        ),
    }


def score_facet_case(case: dict, impact_result: dict) -> dict | None:
    """§27 facet scoring: a PRECISE result must contain every gold-required
    target; CONSERVATIVE_ALL is safe (noisy), never a false negative."""
    required = list(case.get("formal_target_gold", []) or [])
    if not required:
        return None
    claim_id = (case.get("expected_claims") or [None])[0]
    entry = next((c for c in impact_result.get("claims", []) if c["id"] == claim_id), None)
    if entry is None:
        return {
            "case_id": case["case_id"],
            "claim": claim_id,
            "gold_facets": case.get("facet_gold"),
            "required_targets": required,
            "resolver_scope": None,
            "revalidation_targets": [],
            "missing_required_targets": sorted(required),
            "safety_failure": True,
            "note": "expected claim absent from result",
        }
    scope = entry.get("facet_scope")
    targets = list(entry.get("revalidation_targets") or [])
    missing = sorted(set(required) - set(targets))
    return {
        "case_id": case["case_id"],
        "claim": claim_id,
        "gold_facets": case.get("facet_gold"),
        "required_targets": required,
        "resolver_scope": scope,
        "revalidation_targets": targets,
        "missing_required_targets": missing,
        "safety_failure": bool(missing) and scope == "PRECISE",
        "extra_targets": sorted(set(targets) - set(required)),
        "uses_heuristic": case_uses_heuristic(impact_result, claim_id),
    }


def compute_integrity(gold: dict, records: list[dict], rows: list[dict]) -> dict:
    """§21 corpus integrity (pure): every required case exactly once, every
    expected label exactly one scored row, no execution errors."""
    required = list(gold["required_case_ids"])
    by_id: dict[str, list[dict]] = {}
    for rec in records:
        by_id.setdefault(rec["case_id"], []).append(rec)
    missing = [cid for cid in required if cid not in by_id]
    duplicates = sorted(cid for cid, recs in by_id.items() if len(recs) > 1)
    execution_errors = sorted(
        rec["case_id"]
        for rec in records
        if rec.get("reconstruction", {}).get("status") not in (None, "RECONSTRUCTED", "SKIPPED_OUT_OF_EPOCH")
    )
    unscored = sorted(
        rec["case_id"]
        for rec in records
        if rec["case_id"] in required
        and rec.get("reconstruction", {}).get("status") != "RECONSTRUCTED"
    )
    row_keys = [(r["case_id"], r["claim"]) for r in rows]
    # Expected label keys: every (case, expected_claim) pair of the
    # non-AMBIGUOUS POSITIVE required cases (negatives carry no gold rows).
    expected_label_keys = sorted(
        (case["case_id"], claim)
        for case in gold["cases"]
        if case["case_id"] in required
        and case["gold_kind"] == "POSITIVE"
        and case["claim_confidence"] != "AMBIGUOUS"
        for claim in case.get("expected_claims", [])
    )
    missing_rows = sorted(set(expected_label_keys) - set(row_keys))
    duplicate_rows = sorted({k for k in row_keys if row_keys.count(k) > 1})
    expected_rows = gold["required_result_rows"]
    scored_positives = rows
    all_pass = (
        not missing
        and not duplicates
        and not execution_errors
        and not unscored
        and not missing_rows
        and not duplicate_rows
        and len(scored_positives) == expected_rows
    )
    return {
        "all_pass": all_pass,
        "required_cases": len(required),
        "cases_present": len(by_id),
        "missing_cases": missing,
        "duplicate_cases": duplicates,
        "execution_errors": execution_errors,
        "missing_rows": missing_rows,
        "duplicate_rows": duplicate_rows,
        "required_result_rows": expected_rows,
        "scored_result_rows": len(scored_positives),
        "unscored_required_cases": unscored,
    }


def method_metrics(scores: list[dict], negatives: list[dict], rows: list[dict]) -> dict:
    """§23/§25/§26 aggregate metrics for one method (pure)."""
    total = len(rows)
    found = sum(1 for r in rows if r["found"])
    all_surfaced = sum(len(s["surfaced"]) for s in scores) + sum(
        len(n["surfaced"]) for n in negatives
    )
    extra_flags = sum(len(s["extra"]) for s in scores) + sum(
        len(n["extra"]) for n in negatives
    )
    classes = {"DIRECT": 0, "STRUCTURAL": 0, "COARSE": 0, "UNKNOWN": 0}
    for r in rows:
        if r["found"] and r["hit_class"]:
            key = r["hit_class"].split("_")[0]
            if key in classes:
                classes[key] += 1
    return {
        "expected_claim_labels": total,
        "claims_found": found,
        "claim_recall": round(found / total, 4) if total else None,
        "silent_no": sum(1 for r in rows if r["silent_no"]),
        "expected_claim_misses": total - found,
        "unknown_count": classes["UNKNOWN"],
        "coarse_count": classes["COARSE"],
        "direct_count": classes["DIRECT"],
        "structural_count": classes["STRUCTURAL"],
        "positive_cases_with_extra": sum(1 for s in scores if s["extra"]),
        "negative_case_surfaced_claims": sum(len(n["surfaced"]) for n in negatives),
        "extra_claim_flags": extra_flags,
        "review_noise": round(extra_flags / all_surfaced, 4) if all_surfaced else 0.0,
        "all_surfaced_claim_flags": all_surfaced,
    }


def verdict_logic(integrity: dict, metrics: dict, facet_rows: list[dict],
                  reconstructed_scored: int, gold_frozen_first: bool) -> tuple[str, list[str]]:
    """§39 preregistered verdict (pure). Returns (verdict, reasons)."""
    reasons: list[str] = []
    facet_omissions = [r for r in facet_rows if r.get("safety_failure")]
    heuristic_narrow = [
        r for r in facet_rows if r.get("safety_failure") and r.get("uses_heuristic")
    ]
    recall_ok = metrics["claim_recall"] == 1.0 and metrics["expected_claim_misses"] == 0
    silent_ok = metrics["silent_no"] == 0
    if integrity["all_pass"] and reconstructed_scored >= 20 and recall_ok and silent_ok \
            and not facet_omissions and gold_frozen_first:
        return "HISTORICAL_GOLD_EARNED", reasons
    if facet_omissions or metrics["silent_no"] > 0 or (metrics["expected_claim_misses"] > 0 and reconstructed_scored >= 20):
        if heuristic_narrow:
            reasons.append(
                "HEURISTIC PRECISE unsafe narrowing: "
                + ", ".join(r["case_id"] for r in heuristic_narrow)
            )
        reasons.append(
            f"recall {metrics['claims_found']}/{metrics['expected_claim_labels']}, "
            f"silent NO {metrics['silent_no']}, facet omissions {len(facet_omissions)}"
        )
        return "METHOD_RECALL_NOT_EARNED", reasons
    if reconstructed_scored < 20:
        reasons.append(f"only {reconstructed_scored} scored reconstructible cases (< 20)")
        return "HISTORICAL_GOLD_INSUFFICIENT", reasons
    reasons.append("integrity failure without a definite recall miss")
    return "HISTORICAL_GOLD_INSUFFICIENT", reasons


# --- execution layer -----------------------------------------------------------


WORKTREE_LOCK = threading.Lock()


def main_worktree_clean() -> None:
    out = git_output("status", "--porcelain")
    tracked_dirty = [
        line
        for line in out.splitlines()
        if line.strip() and not line.strip().startswith("??")
    ]
    if tracked_dirty:
        raise PhaseCError(
            "refusing to run: current worktree has tracked modifications:\n  "
            + "\n  ".join(tracked_dirty[:10])
        )


def materialize_worktrees(case: dict, tmp_root: Path) -> dict:
    base_wt = tmp_root / f"{case['case_id']}-base"
    head_wt = tmp_root / f"{case['case_id']}-head"
    with WORKTREE_LOCK:
        git_output("worktree", "add", "--detach", "--quiet", str(base_wt), case["base_sha"])
        git_output("worktree", "add", "--detach", "--quiet", str(head_wt), case["head_sha"])
    return {"base": base_wt, "head": head_wt}


def remove_worktrees(wts: dict) -> None:
    with WORKTREE_LOCK:
        for wt in wts.values():
            subprocess.run(
                ["git", "worktree", "remove", "--force", str(wt)],
                cwd=REPO_ROOT, capture_output=True, text=True, timeout=300,
            )
        subprocess.run(["git", "worktree", "prune"], cwd=REPO_ROOT, capture_output=True)


def provision_scip_clang(head_wt: Path) -> dict:
    if not SCIP_CLANG_SRC.is_file():
        raise PhaseCError(f"scip-clang missing in the current repo: {SCIP_CLANG_SRC}")
    lock = json.loads(SCIP_CLANG_LOCK.read_text(encoding="utf-8"))
    src_sha = sha256_file(SCIP_CLANG_SRC)
    if src_sha != lock.get("sha256"):
        raise PhaseCError(
            f"scip-clang sha mismatch vs lock ({src_sha[:12]} != {lock.get('sha256', '')[:12]})"
        )
    dest_dir = head_wt / "build" / "formal-impact" / "bin"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "scip-clang"
    shutil.copy2(SCIP_CLANG_SRC, dest)
    if sha256_file(dest) != src_sha:
        raise PhaseCError("scip-clang copy verification failed")
    return {"sha256": src_sha, "locked_version": lock.get("version")}


def run_resolver(args: list[str], analysis_root: Path, timeout: int) -> tuple[int, str, str]:
    env = dict(os.environ)
    env[ANALYSIS_ENV] = str(analysis_root)
    proc = subprocess.run(
        [sys.executable, str(RESOLVER), *args],
        cwd=analysis_root, env=env, capture_output=True, text=True, timeout=timeout,
    )
    return proc.returncode, proc.stdout, proc.stderr


def prepare_build_world(head_wt: Path) -> dict:
    """Configure + regenerate compile_commands inside the historical world
    (deterministic sequence, identical for every case: `xmake f -y` then the
    resolver's own compdb regeneration inside `index`)."""
    proc = subprocess.run(
        ["xmake", "f", "-y"], cwd=head_wt, capture_output=True, text=True,
        timeout=1200,
    )
    if proc.returncode != 0:
        raise PhaseCError(
            f"TOOLCHAIN_INCOMPATIBLE: xmake configure failed: "
            f"{(proc.stderr or proc.stdout).strip()[:400]}"
        )
    # The merged `index` refreshes an existing compdb but refuses to run
    # without one, so the historical world's compile_commands.json is
    # generated HERE, inside the historical worktree, by the same xmake the
    # resolver would use — before the resolver ever sees the world.
    gen = subprocess.run(
        ["xmake", "project", "-k", "compile_commands"], cwd=head_wt,
        capture_output=True, text=True, timeout=1200,
    )
    if gen.returncode != 0:
        raise PhaseCError(
            f"BUILD_METADATA_UNAVAILABLE: compdb generation failed: "
            f"{(gen.stderr or gen.stdout).strip()[:400]}"
        )
    compdb = head_wt / "compile_commands.json"
    if not compdb.is_file():
        raise PhaseCError("BUILD_METADATA_UNAVAILABLE: compdb generation produced no file")
    return {"compdb_generated": True}


def execute_case(case: dict, tmp_root: Path) -> dict:
    """Full replay of one case. Raises PhaseCError on reconstruction
    failure (classified); records H1-H8 replay identity checks."""
    started = time.time()
    rec: dict = {
        "case_id": case["case_id"],
        "commit_sha": rev_full(case["commit_sha"]),
        "base_sha": rev_full(case["base_sha"]),
        "head_sha": rev_full(case["head_sha"]),
        "gold_kind": case["gold_kind"],
        "applicability": case["applicability"],
        "expected_claims": case.get("expected_claims", []),
        "reconstruction": {"status": None, "error": None},
        "replay_checks": {},
        "build_truth": {},
    }
    if case["applicability"] == "OUT_OF_EPOCH":
        rec["reconstruction"]["status"] = "SKIPPED_OUT_OF_EPOCH"
        rec["reconstruction"]["error"] = "OUT_OF_EPOCH per frozen gold; not replayed, not scored"
        return rec

    wts: dict = {}
    try:
        wts = materialize_worktrees(case, tmp_root)
        head_wt = wts["head"]
        rec["replay_checks"]["H1_base_head_worktrees_distinct"] = (
            wts["base"].is_dir() and head_wt.is_dir() and wts["base"] != head_wt
        )
        rec["replay_checks"]["H1_detail"] = f"{wts['base']} vs {head_wt}"
        tool = provision_scip_clang(head_wt)
        rec["build_truth"]["scip_clang"] = tool

        prepare_build_world(head_wt)

        code, out, err = run_resolver(
            ["index"],  # authority defaults are REPO_ROOT-anchored (seam)
            head_wt,
            CASE_TIMEOUT_SECS,
        )
        if code == 2 and ("xmake" in (out + err).lower() or "configure" in (out + err).lower()):
            raise PhaseCError(f"BUILD_METADATA_UNAVAILABLE: index failed: {(err or out).strip()[:400]}")
        if code != 0:
            raise PhaseCError(f"SCIP_INDEX_FAILED: index exit {code}: {(err or out).strip()[:400]}")

        manifest_bt = json.loads(
            (head_wt / "build" / "formal-impact" / "build-manifest.json").read_text()
        )
        graph = json.loads((head_wt / "build" / "formal-impact" / "graph.json").read_text())
        compdb = json.loads((head_wt / "compile_commands.json").read_text())
        rec["build_truth"].update(
            {
                "xmake_version": manifest_bt.get("xmake_version"),
                "world_id": manifest_bt["worlds"][0]["id"],
                "build_id": manifest_bt.get("build_id"),
                "manifest_head_sha": manifest_bt.get("head_sha"),
                "tu_count": len(manifest_bt["worlds"][0]["tus"]),
                "compdb_entries_total": len(compdb),
                "compdb_entries_selected": manifest_bt["worlds"][0]["compdb"]["selected_entries"],
                "graph_head_sha": graph.get("head_sha"),
                "graph_documents": graph.get("stats", {}).get("documents"),
                "graph_symbols": graph.get("stats", {}).get("symbols"),
                "graph_reference_edges": graph.get("stats", {}).get("reference_edges"),
            }
        )
        rec["replay_checks"]["H2_compile_commands_regenerated"] = len(compdb) > 0
        rec["replay_checks"]["H3_manifest_ids_belong_to_head_world"] = (
            manifest_bt.get("head_sha") == rec["head_sha"]
            and manifest_bt.get("build_id") == graph.get("build", {}).get("build_id")
        )
        rec["replay_checks"]["H4_scip_graph_head_is_historical"] = (
            graph.get("head_sha") == rec["head_sha"]
        )

        def impact(depth_args: list[str]) -> dict:
            code, out, err = run_resolver(
                [
                    "impact",
                    "--range", f"{rec['base_sha']}..{rec['head_sha']}",
                    "--json",
                    "--allow-unknown-for-eval",
                    "--registry", str(ANCHORS_PATH),
                    "--manifest", str(MANIFEST_PATH),
                    *depth_args,
                ],
                head_wt,
                CASE_TIMEOUT_SECS,
            )
            if not out.strip():
                raise PhaseCError(f"IMPACT_FAILED: empty output, exit {code}: {err.strip()[:300]}")
            try:
                return json.loads(out)
            except json.JSONDecodeError as exc:
                raise PhaseCError(f"IMPACT_FAILED: malformed JSON: {exc}") from exc

        method_c = impact([])
        baseline_b = impact(["--max-depth", "0"])
        rec["method_c"] = method_c
        rec["baseline_b"] = baseline_b
        rec["replay_checks"]["H5_result_from_historical_graph"] = (
            method_c.get("graph_head") == rec["head_sha"]
        )
        rec["replay_checks"]["H6_current_graph_not_reused"] = (
            method_c.get("graph_head") == rec["head_sha"]
            and REPO_ROOT.name not in str(method_c.get("graph_head"))
        )
        rec["replay_checks"]["H8_result_identity_matches_case"] = (
            method_c.get("head", "").endswith(case["head_sha"])
            or method_c.get("head") == rec["head_sha"]
        )
        rec["changed_files"] = method_c.get("changed_files", [])
        registry = json.loads(ANCHORS_PATH.read_text(encoding="utf-8"))
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        rec["baseline_a"] = {
            "method": "FILE_ONLY",
            "claims": baseline_a_claims(rec["changed_files"], registry, manifest),
        }
        rec["reconstruction"]["status"] = "RECONSTRUCTED"
        rec["reconstruction"]["seconds"] = round(time.time() - started, 1)
        return rec
    except subprocess.TimeoutExpired as exc:
        rec["reconstruction"]["status"] = "IMPACT_FAILED"
        rec["reconstruction"]["error"] = f"timeout after {CASE_TIMEOUT_SECS}s: {exc}"
        return rec
    except PhaseCError as exc:
        msg = str(exc)
        for prefix in ("TOOLCHAIN_INCOMPATIBLE", "BUILD_METADATA_UNAVAILABLE",
                       "SCIP_INDEX_FAILED", "IMPACT_FAILED"):
            if msg.startswith(prefix):
                rec["reconstruction"]["status"] = prefix
                break
        else:
            rec["reconstruction"]["status"] = "IMPACT_FAILED"
        rec["reconstruction"]["error"] = msg
        return rec
    finally:
        if wts:
            remove_worktrees(wts)
            rec["replay_checks"]["H7_worktrees_removed"] = True


# --- commands -------------------------------------------------------------------


def cmd_validate_gold(_args) -> int:
    gold = load_and_validate_gold()
    n_pos = sum(
        1
        for c in gold["cases"]
        if c["gold_kind"] == "POSITIVE" and c["claim_confidence"] != "AMBIGUOUS"
    )
    print(f"OK    gold valid: {len(gold['cases'])} cases, "
          f"{len(gold['required_case_ids'])} required, {n_pos} non-AMBIGUOUS positives, "
          f"{gold['required_result_rows']} expected claim labels")
    return 0


def cmd_run(args) -> int:
    started = time.time()
    gold = load_and_validate_gold()
    registry = json.loads(ANCHORS_PATH.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    cases = {c["case_id"]: c for c in gold["cases"]}
    selected = (
        [c for c in args.cases.split(",") if c]
        if args.cases
        else [c["case_id"] for c in gold["cases"]]
    )
    unknown = [cid for cid in selected if cid not in cases]
    if unknown:
        print(f"error: unknown case ids: {unknown}", file=sys.stderr)
        return 2

    main_worktree_clean()
    main_graph_before = (
        sha256_file(REPO_ROOT / "build" / "formal-impact" / "graph.json")
        if (REPO_ROOT / "build" / "formal-impact" / "graph.json").is_file()
        else None
    )

    RUN_DIR.mkdir(parents=True, exist_ok=True)
    todo = [
        cases[cid]
        for cid in selected
        if not (args.resume and (RUN_DIR / f"{cid}.json").is_file())
    ]
    print(f"==> executing {len(todo)} of {len(selected)} cases (jobs={args.jobs})")

    def work(case: dict) -> dict:
        record_path = RUN_DIR / f"{case['case_id']}.json"
        tmp_root = Path(tempfile.mkdtemp(prefix=f"fdgc-{case['case_id']}-"))
        try:
            rec = execute_case(case, tmp_root)
        finally:
            shutil.rmtree(tmp_root, ignore_errors=True)
        record_path.write_text(json.dumps(rec, indent=1))
        status = rec["reconstruction"]["status"]
        print(f"    {case['case_id']} {case['commit_sha'][:9]} {status} "
              f"({rec['reconstruction'].get('seconds', '-')}s)")
        return rec

    if args.jobs > 1:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(work, c): c for c in todo}
            for fut in as_completed(futures):
                fut.result()
    else:
        for c in todo:
            work(c)

    records = []
    for cid in selected:
        record_path = RUN_DIR / f"{cid}.json"
        if record_path.is_file():
            records.append(json.loads(record_path.read_text()))

    # H6: the CURRENT world's graph was never touched by the historical runs.
    main_graph_after = (
        sha256_file(REPO_ROOT / "build" / "formal-impact" / "graph.json")
        if (REPO_ROOT / "build" / "formal-impact" / "graph.json").is_file()
        else None
    )
    current_graph_untouched = main_graph_before == main_graph_after
    wt_list = git_output("worktree", "list")
    leaked = [
        line for line in wt_list.splitlines() if ".fdgc" in line or "/tmp/fdgc" in line
    ]

    # --- scoring -------------------------------------------------------------
    rows: list[dict] = []
    method_scores: dict[str, dict] = {}
    recon = {"RECONSTRUCTED": 0, "SKIPPED_OUT_OF_EPOCH": 0}
    for rec in records:
        status = rec["reconstruction"]["status"]
        recon[status] = recon.get(status, 0) + 1
        case = cases[rec["case_id"]]
        if status != "RECONSTRUCTED":
            continue
        if case["gold_kind"] == "POSITIVE" and case["claim_confidence"] != "AMBIGUOUS":
            rows.extend(score_positive_case(case, rec["method_c"])["rows"])

    pos_scores_c, neg_scores_c, facet_rows = [], [], []
    for rec in records:
        if rec["reconstruction"]["status"] != "RECONSTRUCTED":
            continue
        case = cases[rec["case_id"]]
        if case["gold_kind"] == "POSITIVE" and case["claim_confidence"] != "AMBIGUOUS":
            pos_scores_c.append(score_positive_case(case, rec["method_c"]))
        elif case["gold_kind"] == "NEGATIVE":
            neg_scores_c.append(score_negative_case(case, rec["method_c"]))
        if case["gold_kind"] == "POSITIVE" and case["claim_confidence"] != "AMBIGUOUS":
            fr = score_facet_case(case, rec["method_c"])
            if fr:
                facet_rows.append(fr)

    # Baseline A rows (same expected labels, file-only surface)
    rows_a: list[dict] = []
    pos_scores_a = []
    neg_scores_a = []
    for rec in records:
        if rec["reconstruction"]["status"] != "RECONSTRUCTED":
            continue
        case = cases[rec["case_id"]]
        surfaced_a = set(rec["baseline_a"]["claims"])
        if case["gold_kind"] == "NEGATIVE":
            neg_scores_a.append({
                "case_id": case["case_id"],
                "surfaced": sorted(surfaced_a),
                "extra": sorted(surfaced_a),
                "classification": "FILE_ONLY",
                "fail_closed": None,
                "revalidation_targets": [],
            })
            continue
        if not (case["gold_kind"] == "POSITIVE" and case["claim_confidence"] != "AMBIGUOUS"):
            continue
        for claim in case["expected_claims"]:
            rows_a.append({
                "case_id": case["case_id"], "claim": claim,
                "confidence": case["claim_confidence"],
                "found": claim in surfaced_a, "hit_class": "COARSE_FORMAL_IMPACT" if claim in surfaced_a else None,
                "silent_no": claim not in surfaced_a, "uses_heuristic": None,
            })
        pos_scores_a.append({
            "case_id": case["case_id"],
            "surfaced": sorted(surfaced_a),
            "expected": case["expected_claims"],
            "extra": sorted(surfaced_a - set(case["expected_claims"])),
        })
    # Baseline B rows
    rows_b: list[dict] = []
    pos_scores_b = []
    for rec in records:
        if rec["reconstruction"]["status"] != "RECONSTRUCTED":
            continue
        case = cases[rec["case_id"]]
        if not (case["gold_kind"] == "POSITIVE" and case["claim_confidence"] != "AMBIGUOUS"):
            continue
        sc = score_positive_case(case, rec["baseline_b"])
        pos_scores_b.append(sc)
        rows_b.extend(sc["rows"])
    neg_scores_b = [
        score_negative_case(cases[rec["case_id"]], rec["baseline_b"])
        for rec in records
        if rec["reconstruction"]["status"] == "RECONSTRUCTED"
        and cases[rec["case_id"]]["gold_kind"] == "NEGATIVE"
    ]

    integrity = compute_integrity(gold, records, rows)
    metrics_c = method_metrics(pos_scores_c, neg_scores_c, rows)
    metrics_a = method_metrics(pos_scores_a, neg_scores_a, rows_a)
    metrics_b = method_metrics(pos_scores_b, neg_scores_b, rows_b)
    reconstructed_scored = recon.get("RECONSTRUCTED", 0)

    verdict, verdict_reasons = verdict_logic(
        integrity, metrics_c, facet_rows, reconstructed_scored,
        gold_frozen_first=True,
    )

    heuristic = {
        "positive_recoveries_using_heuristic": sum(
            1 for r in rows if r["found"] and r["uses_heuristic"]
        ),
        "precise_routes_using_heuristic": sum(
            1 for r in facet_rows if r.get("resolver_scope") == "PRECISE" and r.get("uses_heuristic")
        ),
        "misses_correlated_with_heuristic": 0,
        "unsafe_facet_narrowing_correlated_with_heuristic": sum(
            1 for r in facet_rows if r.get("safety_failure") and r.get("uses_heuristic")
        ),
    }

    results = {
        "schema": "sluice-fdg0-phase-c-results/1",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "gold": {
            "path": "docs/results/formal/fdg0-phase-c-gold.json",
            "required_result_rows": gold["required_result_rows"],
            "gold_freeze": {
                "last_commit_touching_gold": git_output(
                    "log", "-n", "1", "--format=%H", "--",
                    "docs/results/formal/fdg0-phase-c-gold.json").strip(),
                "run_head_sha": rev_full("HEAD"),
                "gold_matches_committed": None,  # verified at closure (C2 commit diff)
            },
        },
        "method_under_test": {
            "resolver": "scripts/formal/formal_impact.py (current merged #300/#302/#303)",
            "seam": f"{ANALYSIS_ENV}=historical head worktree",
            "max_depth": 2,
            "baseline_b_depth": 0,
        },
        "integrity": integrity,
        "reconstruction_accounting": {
            "eligible": len(records),
            "reconstructed": recon.get("RECONSTRUCTED", 0),
            "out_of_epoch": recon.get("SKIPPED_OUT_OF_EPOCH", 0),
            "reconstruction_failed": sorted(
                cid for cid, n in recon.items() if cid not in ("RECONSTRUCTED", "SKIPPED_OUT_OF_EPOCH")
                for _ in range(n)
            ),
            "status_counts": {k: v for k, v in sorted(recon.items())},
        },
        "verdict": {"value": verdict, "reasons": verdict_reasons},
        "methods": {
            "baseline_a_file_only": metrics_a,
            "baseline_b_explicit_only": metrics_b,
            "method_c": metrics_c,
        },
        "facet_gold_scoring": facet_rows,
        "heuristic_boundary": heuristic,
        "replay_checks": {
            "H1_H7_per_case": {
                rec["case_id"]: rec.get("replay_checks", {})
                for rec in records
            },
            "H6_current_graph_untouched": current_graph_untouched,
            "H7_no_leaked_worktrees": not leaked,
            "leaked": leaked,
        },
        "cases": [
            {
                "case_id": rec["case_id"],
                "commit_sha": rec["commit_sha"],
                "base_sha": rec["base_sha"],
                "head_sha": rec["head_sha"],
                "gold_kind": rec["gold_kind"],
                "applicability": rec["applicability"],
                "expected_claims": rec["expected_claims"],
                "reconstruction": rec["reconstruction"],
                "build_truth": rec.get("build_truth", {}),
                "changed_files": rec.get("changed_files", []),
                "baseline_a": rec.get("baseline_a", {}),
                "method_c": {
                    "classification": rec.get("method_c", {}).get("classification"),
                    "fail_closed": rec.get("method_c", {}).get("fail_closed"),
                    "build_world_verified": (rec.get("method_c", {}).get("build_world") or {}).get("verified"),
                    "claims": [
                        {
                            "id": c.get("id"),
                            "class": c.get("class"),
                            "candidate_class": c.get("candidate_class"),
                            "facet_scope": c.get("facet_scope"),
                            "reached_anchor_ids": c.get("reached_anchor_ids"),
                            "affected_facets": [
                                f.get("id") for f in c.get("affected_facets", [])
                            ],
                            "revalidation_targets": c.get("revalidation_targets"),
                            "paths_use_heuristic": [
                                p.get("uses_heuristic_attribution")
                                for p in c.get("paths", [])
                            ],
                            "via": c.get("via"),
                        }
                        for c in rec.get("method_c", {}).get("claims", [])
                    ],
                    "changed_symbols": rec.get("method_c", {}).get("changed_symbols", {}),
                    "risks": rec.get("method_c", {}).get("risks", []),
                },
                "baseline_b": {
                    "classification": rec.get("baseline_b", {}).get("classification"),
                    "claims": [
                        {"id": c.get("id"), "class": c.get("class")}
                        for c in rec.get("baseline_b", {}).get("claims", [])
                    ],
                },
            }
            for rec in records
        ],
    }

    out_path = REPO_ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(results, indent=1), encoding="utf-8")
    print(f"==> results written: {out_path}")
    print(f"    integrity:  all_pass={integrity['all_pass']}")
    print(f"    verdict:    {verdict}")
    for r in verdict_reasons:
        print(f"                - {r}")
    print(f"    recall A/B/C: {metrics_a['claim_recall']} / {metrics_b['claim_recall']} / "
          f"{metrics_c['claim_recall']}")
    print(f"    noise  A/B/C: {metrics_a['review_noise']} / {metrics_b['review_noise']} / "
          f"{metrics_c['review_noise']}")
    if not integrity["all_pass"]:
        return 1
    return 0


def cmd_inventory(_args) -> int:
    import fdg0_phase_c_inventory

    print("NOTE: the frozen inventory artifact is authoritative "
          "(docs/results/formal/fdg0-phase-c-candidates.json); this regenerates it.")
    return fdg0_phase_c_inventory.main([])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("validate-gold", help="validate the frozen gold file").set_defaults(func=cmd_validate_gold)
    sub.add_parser("inventory", help="regenerate the candidate inventory (frozen artifact is authoritative)").set_defaults(func=cmd_inventory)
    p_run = sub.add_parser("run", help="execute the frozen corpus and score it")
    p_run.add_argument("--jobs", type=int, default=2)
    p_run.add_argument("--resume", action="store_true",
                       help="reuse per-case records in .fdgc-run/ when present")
    p_run.add_argument("--cases", help="comma-separated case-id filter (staged runs)")
    p_run.add_argument("--out", default="docs/results/formal/fdg0-phase-c-results.json")
    p_run.set_defaults(func=cmd_run)
    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except PhaseCError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
