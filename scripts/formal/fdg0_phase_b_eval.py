#!/usr/bin/env python3
"""FDG-0 Phase B — Formal Facets real-machine corpus driver (issue #298).

Executes the preregistered adversarial corpus B1-B9 / B11 (plus the
supplementary F04/F06 rows S-04a/S-04b/S-06a/S-06b) against the live
repository and emits docs/results/formal/fdg0-phase-b.json.

    B1a body edit  signal_wake_locked        -> F08 DIRECT  facet wake-publication
    B1b body edit  park_on_wake_source       -> F08 DIRECT  facet park-commit-return
    B2  two-phase  helper called by park (INERT body, no wake_epoch_ touch)
                                   -> F08 STRUCTURAL facet park-commit-return
    B3  T9 shape   rogue wake_epoch_ writer  -> F08 STRUCTURAL facet wake-publication
    B4  body edit  run_impl                  -> F08 DIRECT  facet startup-population
    B5  body edit  RequestArena::validate_   -> F01 + F03 + F06 rows (shared anchor)
    B6a body edit  ThreadPoolBackend::run_syscall        -> F03 conservative
    B6b body edit  UringAsyncBackend::finalize_operation_terminal_ (gated; the
        selected world must have liburing enabled) -> F03 conservative
    B7  two-phase  helper called by park whose body ADVANCES wake_epoch_
                                   -> F08 STRUCTURAL facet union
    B8  line-1 edit scheduler_condition.cpp  -> F08 COARSE conservative-all
    B9  B1a edit + Build-Manifest build_id drift -> UNKNOWN fail-closed,
        facet precision demoted to candidate diagnostics
    B11 body edit  free_slot_locked_         -> F01 non-faceted compatibility
    S-* supplementary F04/F06 facet rows

B10 (unresolved/frontier) and B12 (registry structural negatives) are
hermetic: scripts/tests/test_formal_impact_facets.py.

Expected outcomes come from the PREREGISTRATION
(docs/results/formal/fdg0-phase-b-prereg.json, frozen before this
implementation at a708884d). Deviations are recorded, never silently
rewritten.

Worktree safety (mirrors ftlr0_eval.py / fdg0_phase_a_eval.py): refuses a
dirty tracked tree, snapshots every touched file byte-verbatim, restores
on exit, rebuilds the index at HEAD around every reindex specimen, and
re-verifies `git status --short`.

USAGE
    python3 scripts/formal/fdg0_phase_b_eval.py run
    python3 scripts/formal/fdg0_phase_b_eval.py verify-clean
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

import formal_impact as fi  # noqa: E402

RESULTS_PATH = REPO_ROOT / "docs" / "results" / "formal" / "fdg0-phase-b.json"
PREREG_PATH = REPO_ROOT / "docs" / "results" / "formal" / "fdg0-phase-b-prereg.json"
BUILD_MANIFEST = fi.BUILD_DIR / "build-manifest.json"

# Precise B1-B7 rows are the frozen threshold denominator (issue #298 §22).
PRECISE_B_ROWS = ["B1a", "B1b", "B2", "B3", "B4", "B7"]
SUPPLEMENTARY_ROWS = ["S-04a", "S-04b", "S-06a", "S-06b"]


def sh(*args: str, cwd: Path = REPO_ROOT, ok: tuple = (0,)) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode not in ok:
        raise RuntimeError(f"{' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def git_status_short() -> str:
    return sh("git", "status", "--short")


class WorktreeEdit:
    def __init__(self, path: str, old: str, new: str):
        self.path = path
        self.old = old
        self.new = new

    def apply(self):
        p = REPO_ROOT / self.path
        text = p.read_text(encoding="utf-8")
        count = text.count(self.old)
        if count != 1:
            raise RuntimeError(
                f"specimen edit anchor not applicable in {self.path}: {count} matches"
            )
        p.write_text(text.replace(self.old, self.new), encoding="utf-8")


def touch(path: str, after: str, indent="    ") -> WorktreeEdit:
    """One inert comment line inserted after a unique anchor string."""
    return WorktreeEdit(path, after, after + f"\n{indent}// fdg0-b specimen touch")


# --- specimen edit builders ----------------------------------------------------


# B7's helper must be a MEMBER (it touches the private wake_epoch_ state).
# Its declaration is inserted immediately after external_wake_possible_locked's
# inline body: that member references no formal anchor, so the documented
# nearest-preceding attribution fold (the declaration occurrence lands in the
# preceding inline member's refs) is harmless. Plain member declarations are
# NOT def positions, and nothing follows before the class tail, so the helper
# hoovers no further occurrences.
B7_DECL_ANCHOR = """    bool external_wake_possible_locked() const SLUICE_REQUIRES(global_mtx_) {
        return !waiting_ready_.empty() || waiting_waitq_count_ > 0 ||
               any_active_deadline_locked() || waiting_select_count_ > 0;
    }"""


def build_b2_phase_a() -> list[WorktreeEdit]:
    """Phase A: an INERT helper exists and park_on_wake_source delegates to
    it (indexed; NOT part of the evaluated diff). The helper is a free
    function defined in the same TU before its caller — no header edit, no
    member access needed, and it deliberately does NOT touch wake_epoch_,
    so the wake facet must not be pulled in."""
    helper_def = (
        "static void fdg0b_specimen_park_helper_inert() {\n"
        "    // fdg0-B2 specimen helper: parked-side delegation, inert body\n"
        "    // (deliberately no wake_epoch_ touch).\n"
        "}\n"
        "\n"
    )
    sig = "void Scheduler::park_on_wake_source(WorkerState* ws,"
    body_open = " bool bounded_backend_observation) SLUICE_NO_THREAD_SAFETY_ANALYSIS {"
    return [
        WorktreeEdit("src/async/scheduler_park_wake.cpp", sig, helper_def + sig),
        WorktreeEdit(
            "src/async/scheduler_park_wake.cpp",
            body_open,
            body_open + "\n    fdg0b_specimen_park_helper_inert();",
        ),
    ]


def build_b2_phase_b() -> WorktreeEdit:
    return WorktreeEdit(
        "src/async/scheduler_park_wake.cpp",
        "// fdg0-B2 specimen helper: parked-side delegation, inert body\n",
        "// fdg0-B2 specimen helper: parked-side delegation, inert body\n"
        "// fdg0-b specimen touch (evaluated diff: helper body only)\n",
    )


def build_b7_phase_a() -> list[WorktreeEdit]:
    """Phase A: a member helper called by park_on_wake_source whose body
    ADVANCES the anchored wake epoch state (the B7 multi-anchor shape).
    Declared after external_wake_possible_locked's inline body (see
    B7_DECL_ANCHOR for the attribution-fold rationale)."""
    helper_name = "fdg0b_specimen_park_helper_epoch"
    helper_def = (
        f"void Scheduler::{helper_name}() {{\n"
        "    // fdg0-B7 specimen helper: parked-side delegation that advances\n"
        "    // the formalized wake state.\n"
        "    wake_epoch_.fetch_add(1, std::memory_order_acq_rel);\n"
        "}\n"
        "\n"
    )
    sig = "void Scheduler::park_on_wake_source(WorkerState* ws,"
    body_open = " bool bounded_backend_observation) SLUICE_NO_THREAD_SAFETY_ANALYSIS {"
    return [
        WorktreeEdit(
            "include/sluice/async/scheduler.hpp",
            B7_DECL_ANCHOR,
            B7_DECL_ANCHOR + f"\n    void {helper_name}();  // fdg0-B7 specimen",
        ),
        WorktreeEdit("src/async/scheduler_park_wake.cpp", sig, helper_def + sig),
        WorktreeEdit(
            "src/async/scheduler_park_wake.cpp",
            body_open,
            body_open + f"\n    {helper_name}();",
        ),
    ]


def build_b7_phase_b() -> WorktreeEdit:
    return WorktreeEdit(
        "src/async/scheduler_park_wake.cpp",
        "    // fdg0-B7 specimen helper: parked-side delegation that advances\n",
        "    // fdg0-B7 specimen helper: parked-side delegation that advances\n"
        "    // fdg0-b specimen touch (evaluated diff: helper body only)\n",
    )


def build_b3_edits() -> list[WorktreeEdit]:
    """T9 shape (imported verbatim from the #300 corpus): a rogue member
    writes the anchored wake state, bypassing signal_wake_locked."""
    import ftlr0_eval

    return ftlr0_eval.build_t9_edits()


# --- driver ---------------------------------------------------------------------


class Driver:
    def __init__(self):
        self.saved: dict[str, bytes | None] = {}
        self.records: list[dict] = []

    def snapshot(self, rel: str):
        if rel not in self.saved:
            p = REPO_ROOT / rel
            self.saved[rel] = p.read_bytes() if p.exists() else None

    def apply(self, edits: list[WorktreeEdit]):
        for e in edits:
            self.snapshot(e.path)
        for e in edits:
            e.apply()

    def restore(self):
        for rel, data in self.saved.items():
            p = REPO_ROOT / rel
            if data is None:
                p.unlink(missing_ok=True)
            else:
                p.write_bytes(data)
        self.saved.clear()

    def reindex(self):
        sh(sys.executable, str(SCRIPT_DIR / "formal_impact.py"), "index")

    def run_impact(self, args: list[str]) -> dict:
        out = sh(
            sys.executable,
            str(SCRIPT_DIR / "formal_impact.py"),
            "impact", "--json", "--allow-unknown-for-eval", *args,
        )
        return json.loads(out)

    # -- specimen shapes --

    def diff_file_specimen(self, spec_id: str, edits: list[WorktreeEdit], note: str = "") -> dict:
        self.apply(edits)
        patch = sh("git", "diff", "--no-renames", "-U0", "--", *{e.path for e in edits})
        self.restore()
        patch_path = fi.BUILD_DIR / f"fdg0b-specimen-{spec_id}.patch"
        patch_path.parent.mkdir(parents=True, exist_ok=True)
        patch_path.write_text(patch, encoding="utf-8")
        result = self.run_impact(["--diff-file", str(patch_path), "--max-depth", "2"])
        return {
            "id": spec_id,
            "mode": "diff-file",
            "patch": str(patch_path.relative_to(REPO_ROOT)),
            "note": note,
            "result": result,
        }

    def two_phase_specimen(self, spec_id: str, phase_a: list[WorktreeEdit],
                           phase_b: WorktreeEdit, note: str = "") -> dict:
        rel_file = phase_b.path
        self.apply(phase_a)
        self.reindex()
        baseline_bytes = (REPO_ROOT / rel_file).read_bytes()
        self.apply([phase_b])
        self.reindex()
        base_prefix = "build/formal-impact/fdg0b-baseline/"
        base_file = REPO_ROOT / base_prefix / rel_file
        base_file.parent.mkdir(parents=True, exist_ok=True)
        base_file.write_bytes(baseline_bytes)
        patch = sh(
            "git", "diff", "--no-index", "--no-renames", "-U0",
            base_prefix + rel_file, rel_file, ok=(0, 1),
        )
        patch = patch.replace(f"a/{base_prefix}{rel_file}", f"a/{rel_file}")
        shutil.rmtree(REPO_ROOT / base_prefix, ignore_errors=True)
        patch_path = fi.BUILD_DIR / f"fdg0b-specimen-{spec_id}.patch"
        patch_path.write_text(patch, encoding="utf-8")
        # Query against the POST-PHASE-B world (tree still edited, graph
        # rebuilt above): the #300 T8 corrective-1 semantics. The evaluated
        # patch's hunk numbering belongs to the phase-B world, so attributing
        # it against a restored HEAD-world graph would skew hunks into the
        # shifted neighbors and fabricate a DIRECT hit (fixture bug class).
        result = self.run_impact(["--diff-file", str(patch_path), "--max-depth", "2"])
        self.restore()
        self.reindex()
        return {
            "id": spec_id,
            "mode": "two-phase-reindex",
            "patch": str(patch_path.relative_to(REPO_ROOT)),
            "note": note,
            "result": result,
        }

    def working_tree_specimen(self, spec_id: str, edits: list[WorktreeEdit],
                              extra_mutations=None, note: str = "") -> dict:
        self.apply(edits)
        if extra_mutations:
            extra_mutations(self)
        result = self.run_impact(["--working-tree", "--max-depth", "2"])
        self.restore()
        return {
            "id": spec_id,
            "mode": "working-tree",
            "note": note,
            "result": result,
        }


# --- corpus -------------------------------------------------------------------

F08 = "src/async/scheduler_park_wake.cpp"


def build_plan() -> list[dict]:
    """Ordered (builder, runner) plan for the whole corpus."""
    plan: list[dict] = []

    plan.append(("call", lambda d: d.diff_file_specimen(
        "B1a",
        [touch(F08, "void Scheduler::signal_wake_locked() {")],
        "direct edit of the unified wake source authority")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "B1b",
        [touch(
            F08,
            "void Scheduler::park_on_wake_source(WorkerState* ws,\n"
            "                                    bool bounded_backend_observation) "
            "SLUICE_NO_THREAD_SAFETY_ANALYSIS {",
            indent="    ",
        )],
        "direct edit of the park commit authority")))
    plan.append(("call", lambda d: d.two_phase_specimen(
        "B2", build_b2_phase_a(), build_b2_phase_b(),
        "helper-only STRUCTURAL under park; inert helper (no wake_epoch_)")))
    plan.append(("call", lambda d: d.working_tree_specimen(
        "B3", build_b3_edits(),
        note="rogue wake_epoch_ writer bypassing the wake functions (T9 shape)")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "B4",
        [touch("src/async/scheduler.cpp",
               "void Scheduler::run_impl(unsigned worker_count, RunMode mode) {")],
        "startup population authority, no other F08 anchor")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "B5",
        [WorktreeEdit(
            "include/sluice/async/detail/request_arena.hpp",
            "    RequestSlot* validate_(SlotHandle h) noexcept {",
            "    RequestSlot* validate_(SlotHandle h) noexcept {\n"
            "        // fdg0-b specimen touch",
        )],
        "shared F01/F06 identity gate")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "B6a",
        [touch("src/async/threadpool_backend.cpp",
               "detail::TerminalResult ThreadPoolBackend::run_syscall(const PreparedBlockingOp& p) noexcept {")],
        "F03 threadpool terminal producer (default world)")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "B6b",
        [touch("src/async/uring_backend.cpp",
               "void UringAsyncBackend::finalize_operation_terminal_(")],
        "F03 uring terminal producer (config-gated anchor; liburing world)")))
    plan.append(("call", lambda d: d.two_phase_specimen(
        "B7", build_b7_phase_a(), build_b7_phase_b(),
        "helper under park whose body advances wake_epoch_ (multi-anchor union)")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "B8",
        [touch("src/async/scheduler_condition.cpp",
               (REPO_ROOT / "src/async/scheduler_condition.cpp").read_text(
                   encoding="utf-8").splitlines()[0], indent="")],
        "file-level coarse hit (line 1, before any definition)")))
    plan.append(("call", lambda d: d.working_tree_specimen(
        "B9",
        [touch(F08, "void Scheduler::signal_wake_locked() {")],
        extra_mutations=lambda dr: corrupt_build_manifest(dr),
        note="trusted-graph edit + Build Manifest drift -> UNKNOWN, facets demoted")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "B11",
        [touch("include/sluice/async/detail/request_arena.hpp",
               "    void free_slot_locked_(RequestSlot* s, std::uint32_t idx) noexcept {",
               indent="        ")],
        "non-faceted claim compatibility (F01 anchor)")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "S-04a",
        [touch("include/sluice/async/detail/request_arena.hpp",
               "    std::size_t reap(SynchronousReadySink& sink) {",
               indent="        ")],
        "F04 arena reap facet row")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "S-04b",
        [touch("include/sluice/async/completion.hpp",
               "    void publish_from_reap(Result<T>&& res) noexcept {",
               indent="        ")],
        "F04 ready-release LP facet row")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "S-06a",
        [touch("src/async/cancel.cpp", "void CancelToken::request() noexcept {")],
        "F06 request-epoch facet row")))
    plan.append(("call", lambda d: d.diff_file_specimen(
        "S-06b",
        [touch("src/async/cancel.cpp",
               "Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept {")],
        "F06 delivery-gate facet row")))
    return plan


def corrupt_build_manifest(driver: Driver):
    """B9 extra mutation: drift the Build Manifest build_id away from the
    graph's recorded identity (gitignored artifact; snapshotted + restored)."""
    driver.snapshot(str(BUILD_MANIFEST.relative_to(REPO_ROOT)))
    data = json.loads(BUILD_MANIFEST.read_text(encoding="utf-8"))
    data["build_id"] = "f" * 64
    BUILD_MANIFEST.write_text(json.dumps(data, indent=1), encoding="utf-8")


# --- measurement + prereg comparison --------------------------------------------


def extract_rows(record: dict) -> list[dict]:
    result = record["result"]
    rows = []
    for c in result.get("claims", []):
        baseline = len(c.get("formal_suites", []))
        targets = c.get("revalidation_targets", c.get("formal_suites", []))
        rows.append({
            "specimen": record["id"],
            "claim": c["id"],
            "class": c["class"],
            "facet_scope": c.get("facet_scope"),
            "reached_anchor_ids": c.get("reached_anchor_ids", []),
            "affected_facets": [f["id"] for f in c.get("affected_facets", [])],
            "baseline_targets": baseline,
            "facet_targets": len(targets),
            "reduction": round((baseline - len(targets)) / baseline, 4) if baseline else 0.0,
            "fail_closed": result.get("fail_closed", False),
        })
    return rows


def compare_with_prereg(all_rows: list[dict], records: list[dict]) -> dict:
    prereg = json.loads(PREREG_PATH.read_text(encoding="utf-8"))
    expected_by_id = {e["id"]: e for e in prereg["b_corpus_expectations"]}
    deviations = []
    matches = 0
    checked = 0
    for row in all_rows:
        spec = expected_by_id.get(row["specimen"])
        if not spec:
            continue
        if "rows" in spec:  # B5: per-claim expectations
            exp = next((r for r in spec["rows"] if r["claims"] == [row["claim"]]), None)
        else:
            exp = spec["expected"]
        if not exp:
            continue
        checked += 1
        problems = []
        if exp.get("claims") and row["claim"] not in exp["claims"]:
            problems.append(f"claim {row['claim']} not expected")
        if "class" in exp and exp["class"] != row["class"]:
            problems.append(f"class {row['class']} != {exp['class']}")
        if "facet_scope" in exp and exp["facet_scope"] != row["facet_scope"]:
            problems.append(f"facet_scope {row['facet_scope']} != {exp['facet_scope']}")
        if "affected_facets" in exp and sorted(exp["affected_facets"]) != sorted(row["affected_facets"]):
            problems.append(
                f"facets {row['affected_facets']} != {exp['affected_facets']}"
            )
        if "facet_targets" in exp and exp["facet_targets"] != row["facet_targets"]:
            problems.append(f"targets {row['facet_targets']} != {exp['facet_targets']}")
        if "targets" in exp and exp["targets"] != row["facet_targets"]:
            problems.append(f"targets {row['facet_targets']} != {exp['targets']}")
        if problems:
            deviations.append({
                "specimen": row["specimen"], "claim": row["claim"],
                "deviations": problems,
                "note": "prereg value NOT rewritten; deviation explained in the phase-B doc",
            })
        else:
            matches += 1
    return {
        "prereg_head": prereg["frozen_at_head"],
        "rows_checked": checked,
        "rows_matching": matches,
        "deviations": deviations,
    }


def measurement_section(all_rows: list[dict]) -> dict:
    def rows_for(ids):
        return [r for r in all_rows if r["specimen"] in ids]

    precise_b = rows_for(PRECISE_B_ROWS)
    precise_b = [r for r in precise_b if r["facet_scope"] == "PRECISE"]
    supp = [
        r for r in rows_for(SUPPLEMENTARY_ROWS)
        if r["specimen"] in SUPPLEMENTARY_ROWS and r["facet_scope"] == "PRECISE"
    ]
    conservative = [
        r for r in all_rows
        if r not in precise_b and r not in supp
    ]
    def stats(rows):
        if not rows:
            return {"rows": 0, "mean_reduction": None}
        return {
            "rows": len(rows),
            "mean_reduction": round(sum(r["reduction"] for r in rows) / len(rows), 4),
            "detail": [
                {k: r[k] for k in ("specimen", "claim", "baseline_targets",
                                   "facet_targets", "reduction")}
                for r in rows
            ],
        }
    threshold = 0.30
    b_stats = stats(precise_b)
    return {
        "precise_b1_b7": b_stats,
        "supplementary_precise": stats(supp),
        "conservative_cases": {k: v for k, v in stats(conservative).items() if k != "mean_reduction"},
        "threshold": {"metric": "mean reduction over PRECISE B1-B7 rows", "value": threshold,
                      "met": (b_stats["mean_reduction"] is not None
                              and b_stats["mean_reduction"] >= threshold)},
    }


# --- main ---------------------------------------------------------------------


def run_corpus() -> dict:
    started = time.time()
    driver = Driver()
    # Freshness precondition: diff-file specimens trust the on-disk graph,
    # so the corpus MUST start from an index built at the current HEAD.
    driver.reindex()
    records = []
    for kind, payload in build_plan():
        try:
            if kind == "call":
                records.append(payload(driver))
        except Exception as exc:  # noqa: BLE001 — a failed specimen is evidence
            records.append({"id": getattr(payload, "__name__", "?"), "error":
                            f"{type(exc).__name__}: {exc}"})
        finally:
            driver.restore()
    # resync the world for later consumers
    driver.reindex()

    all_rows = []
    for r in records:
        if "result" in r:
            all_rows.extend(extract_rows(r))
    measurement = measurement_section(all_rows)
    comparison = compare_with_prereg(all_rows, records)

    summary = {
        "experiment": "FDG-0 Phase B formal facets adversarial corpus (issue #298)",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "head_sha": sh("git", "rev-parse", "HEAD").strip(),
        "prereg": str(PREREG_PATH.relative_to(REPO_ROOT)),
        "b10_b12_coverage": (
            "hermetic: scripts/tests/test_formal_impact_facets.py "
            "(FR6 stale demotion, FR7 unresolved never PRECISE, FV1-FV12 "
            "registry structural negatives)"
        ),
        "specimens": records,
        "rows": all_rows,
        "measurement": measurement,
        "prereg_comparison": comparison,
        "wall_seconds": round(time.time() - started, 1),
    }
    return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("run", help="run the Phase-B corpus")
    sub.add_parser("verify-clean", help="fail if the working tree is dirty")
    args = parser.parse_args(argv)

    if args.command == "verify-clean":
        status = git_status_short()
        if status:
            print("DIRTY:", status)
            return 1
        print("clean")
        return 0

    dirty = [
        line for line in git_status_short().splitlines()
        if line and not line.startswith("??")
    ]
    if dirty:
        print("error: refuse to run with modified tracked files:", file=sys.stderr)
        print("\n".join(dirty), file=sys.stderr)
        return 2
    summary = run_corpus()
    out = Path(RESULTS_PATH)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    print(f"==> results written: {out}")
    for r in summary["specimens"]:
        if "error" in r:
            print(f"  {r['id']}: ERROR {r['error']}")
            continue
        result = r["result"]
        bits = []
        for c in result.get("claims", []):
            facet = c.get("facet_scope", "-")
            targets = len(c.get("revalidation_targets", c.get("formal_suites", [])))
            facets = ",".join(f["id"] for f in c.get("affected_facets", [])) or "-"
            bits.append(f"{c['id']}:{c['class'].split('_')[0]}/{facet}/{facets}/{targets}t")
        fc = " [fail-closed]" if result.get("fail_closed") else ""
        print(f"  {r['id']}: {result['classification']}{fc} :: " + "; ".join(bits))
    m = summary["measurement"]
    print(f"==> PRECISE B1-B7: {m['precise_b1_b7']}")
    print(f"==> threshold met: {m['threshold']['met']}")
    print(f"==> prereg: {summary['prereg_comparison']['rows_matching']}/"
          f"{summary['prereg_comparison']['rows_checked']} rows match, "
          f"{len(summary['prereg_comparison']['deviations'])} deviation(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
