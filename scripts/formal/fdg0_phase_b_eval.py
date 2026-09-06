#!/usr/bin/env python3
"""FDG-0 Phase B — Formal Facets real-machine corpus driver (issue #298).

Executes the preregistered adversarial corpus B1-B9 / B11 (plus the
supplementary F04/F06 rows S-04a/S-04b/S-06a/S-06b) against the live
repository and emits docs/results/formal/fdg0-phase-b.json.

    B1a body edit  signal_wake_locked        -> F08 DIRECT  facet wake-publication
    B1b body edit  park_on_wake_source       -> F08 DIRECT  facet park-commit-return
    B2  two-phase  helper called by park (INERT body, no wake_epoch_ touch)
                                   -> F08 STRUCTURAL facet park-commit-return
    B3  two-phase  rogue wake_epoch_ writer (never called; declaration at the
        audited-safe point)      -> F08 STRUCTURAL facet wake-publication via
        wake-epoch-state ONLY (hard negative: wake-signal)
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

Execution integrity (corrective-1 C3): the run is FAIL-CLOSED. A specimen
that errors, returns no result, or fails to produce a preregistered
required claim row sets integrity.all_pass = false and the driver exits
non-zero; the frozen PRECISE threshold is only authoritative when every
frozen (specimen, claim) row key is present exactly once and no specimen
errored. Unexpected extra rows remain evidence and are reported, but never
fail the run and never enter the denominator.

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

# The frozen threshold denominator (corrective-1 C1): EXACTLY the
# (specimen, claim) row keys preregistered in
# predictions.precise_b1_b7_rows — seven keys, with B5 participating
# through its F06 row only. B5/F01, B5/F03, the B6* producer rows, B8/B9
# and the supplementary S-* rows are reported raw but are NOT in the
# denominator. The keys are read from the frozen preregistration at run
# time, never re-derived from results.
THRESHOLD = 0.30
SUPPLEMENTARY_ROWS = ["S-04a", "S-04b", "S-06a", "S-06b"]


def load_prereg() -> dict:
    return json.loads(PREREG_PATH.read_text(encoding="utf-8"))


def frozen_precise_row_keys(prereg: dict) -> list[tuple[str, str]]:
    """The frozen threshold denominator as exact (specimen, claim) row keys
    (corrective-1 C1). The preregistered list names bare specimen ids for
    the single-claim F08 rows and one explicit "B5/F06" key for the shared
    B5 specimen; both forms resolve strictly against the frozen artifact."""
    claims_by_specimen: dict[str, list[str]] = {}
    for spec in prereg["b_corpus_expectations"]:
        claims = (spec.get("expected") or {}).get("claims") or []
        if claims:
            claims_by_specimen[spec["id"]] = claims
    keys: list[tuple[str, str]] = []
    for entry in prereg["predictions"]["precise_b1_b7_rows"]:
        spec, sep, claim = entry.partition("/")
        if sep:
            if not spec or not claim:
                raise ValueError(f"malformed frozen precise row key: {entry!r}")
            keys.append((spec, claim))
        else:
            claims = claims_by_specimen.get(entry) or []
            if len(claims) != 1:
                raise ValueError(
                    f"frozen precise row {entry!r} does not name exactly one claim"
                )
            keys.append((entry, claims[0]))
    if len(set(keys)) != len(keys):
        raise ValueError("frozen precise row keys contain duplicates")
    return keys


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


PARK_SIG = "void Scheduler::park_on_wake_source(WorkerState* ws,"
PARK_BODY_OPEN = " bool bounded_backend_observation) SLUICE_NO_THREAD_SAFETY_ANALYSIS {"


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


# wake_epoch_ is a plain std::uint64_t (scheduler.hpp), so both state-touching
# fixtures (B3/B7) advance it with a well-formed assignment. The original T9
# shape used an ill-formed member call (fetch_add/store) on the non-atomic
# integer and only captured its wake_epoch_ reference through clang error
# recovery — an experiment-integrity defect repaired here (corrective-1 C2).
EPOCH_WRITE = "    wake_epoch_ = wake_epoch_ + 1;\n"


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
    return [
        WorktreeEdit("src/async/scheduler_park_wake.cpp", PARK_SIG, helper_def + PARK_SIG),
        WorktreeEdit(
            "src/async/scheduler_park_wake.cpp",
            PARK_BODY_OPEN,
            PARK_BODY_OPEN + "\n    fdg0b_specimen_park_helper_inert();",
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
        + EPOCH_WRITE
        + "}\n"
        "\n"
    )
    return [
        WorktreeEdit(
            "include/sluice/async/scheduler.hpp",
            B7_DECL_ANCHOR,
            B7_DECL_ANCHOR + f"\n    void {helper_name}();  // fdg0-B7 specimen",
        ),
        WorktreeEdit("src/async/scheduler_park_wake.cpp", PARK_SIG, helper_def + PARK_SIG),
        WorktreeEdit(
            "src/async/scheduler_park_wake.cpp",
            PARK_BODY_OPEN,
            PARK_BODY_OPEN + f"\n    {helper_name}();",
        ),
    ]


def build_b7_phase_b() -> WorktreeEdit:
    return WorktreeEdit(
        "src/async/scheduler_park_wake.cpp",
        "    // fdg0-B7 specimen helper: parked-side delegation that advances\n",
        "    // fdg0-B7 specimen helper: parked-side delegation that advances\n"
        "    // fdg0-b specimen touch (evaluated diff: helper body only)\n",
    )


def build_b3_phase_a() -> list[WorktreeEdit]:
    """Phase A (corrective-1 C2): a ROGUE Scheduler member exists whose body
    writes the anchored wake epoch state directly, bypassing
    signal_wake_locked / park_on_wake_source / run_impl (T9 shape: declared
    + defined, never called by anyone). Its declaration sits ONLY at the
    audited-safe insertion point shared with B7, so the documented
    nearest-preceding attribution fold cannot fabricate an anchor edge —
    the old T9 insertion after signal_wake_locked's declaration folded the
    new declaration occurrence into wake-signal's refs and misrouted the
    specimen."""
    helper_name = "fdg0b_specimen_rogue_epoch_writer"
    helper_def = (
        f"void Scheduler::{helper_name}() {{\n"
        "    // fdg0-B3 specimen: rogue wake_epoch_ writer bypassing\n"
        "    // signal_wake_locked (never called — T9 shape).\n"
        + EPOCH_WRITE
        + "}\n"
        "\n"
    )
    return [
        WorktreeEdit(
            "include/sluice/async/scheduler.hpp",
            B7_DECL_ANCHOR,
            B7_DECL_ANCHOR
            + f"\n    void {helper_name}();  // fdg0-B3 specimen",
        ),
        WorktreeEdit("src/async/scheduler_park_wake.cpp", PARK_SIG, helper_def + PARK_SIG),
    ]


def build_b3_phase_b() -> WorktreeEdit:
    """The evaluated diff touches ONLY the rogue helper body."""
    return WorktreeEdit(
        "src/async/scheduler_park_wake.cpp",
        "    // fdg0-B3 specimen: rogue wake_epoch_ writer bypassing\n",
        "    // fdg0-B3 specimen: rogue wake_epoch_ writer bypassing\n"
        "    // fdg0-b specimen touch (evaluated diff: rogue body only)\n",
    )


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
    """Ordered ({id, run}) plan for the whole corpus. Every entry carries
    its specimen id so a failing run can be attributed and (corrective-1
    C3) fail closed."""
    plan: list[dict] = []

    plan.append({"id": "B1a", "run": lambda d: d.diff_file_specimen(
        "B1a",
        [touch(F08, "void Scheduler::signal_wake_locked() {")],
        "direct edit of the unified wake source authority")})
    plan.append({"id": "B1b", "run": lambda d: d.diff_file_specimen(
        "B1b",
        [touch(
            F08,
            "void Scheduler::park_on_wake_source(WorkerState* ws,\n"
            "                                    bool bounded_backend_observation) "
            "SLUICE_NO_THREAD_SAFETY_ANALYSIS {",
            indent="    ",
        )],
        "direct edit of the park commit authority")})
    plan.append({"id": "B2", "run": lambda d: d.two_phase_specimen(
        "B2", build_b2_phase_a(), build_b2_phase_b(),
        "helper-only STRUCTURAL under park; inert helper (no wake_epoch_)")})
    plan.append({"id": "B3", "run": lambda d: d.two_phase_specimen(
        "B3", build_b3_phase_a(), build_b3_phase_b(),
        "rogue wake_epoch_ writer bypassing the wake functions (T9 shape, "
        "two-phase: never called, safe declaration point)")})
    plan.append({"id": "B4", "run": lambda d: d.diff_file_specimen(
        "B4",
        [touch("src/async/scheduler.cpp",
               "void Scheduler::run_impl(unsigned worker_count, RunMode mode) {")],
        "startup population authority, no other F08 anchor")})
    plan.append({"id": "B5", "run": lambda d: d.diff_file_specimen(
        "B5",
        [WorktreeEdit(
            "include/sluice/async/detail/request_arena.hpp",
            "    RequestSlot* validate_(SlotHandle h) noexcept {",
            "    RequestSlot* validate_(SlotHandle h) noexcept {\n"
            "        // fdg0-b specimen touch",
        )],
        "shared F01/F06 identity gate")})
    plan.append({"id": "B6a", "run": lambda d: d.diff_file_specimen(
        "B6a",
        [touch("src/async/threadpool_backend.cpp",
               "detail::TerminalResult ThreadPoolBackend::run_syscall(const PreparedBlockingOp& p) noexcept {")],
        "F03 threadpool terminal producer (default world)")})
    plan.append({"id": "B6b", "run": lambda d: d.diff_file_specimen(
        "B6b",
        [touch("src/async/uring_backend.cpp",
               "void UringAsyncBackend::finalize_operation_terminal_(")],
        "F03 uring terminal producer (config-gated anchor; liburing world)")})
    plan.append({"id": "B7", "run": lambda d: d.two_phase_specimen(
        "B7", build_b7_phase_a(), build_b7_phase_b(),
        "helper under park whose body advances wake_epoch_ (multi-anchor union)")})
    plan.append({"id": "B8", "run": lambda d: d.diff_file_specimen(
        "B8",
        [touch("src/async/scheduler_condition.cpp",
               (REPO_ROOT / "src/async/scheduler_condition.cpp").read_text(
                   encoding="utf-8").splitlines()[0], indent="")],
        "file-level coarse hit (line 1, before any definition)")})
    plan.append({"id": "B9", "run": lambda d: d.working_tree_specimen(
        "B9",
        [touch(F08, "void Scheduler::signal_wake_locked() {")],
        extra_mutations=lambda dr: corrupt_build_manifest(dr),
        note="trusted-graph edit + Build Manifest drift -> UNKNOWN, facets demoted")})
    plan.append({"id": "B11", "run": lambda d: d.diff_file_specimen(
        "B11",
        [touch("include/sluice/async/detail/request_arena.hpp",
               "    void free_slot_locked_(RequestSlot* s, std::uint32_t idx) noexcept {",
               indent="        ")],
        "non-faceted claim compatibility (F01 anchor)")})
    plan.append({"id": "S-04a", "run": lambda d: d.diff_file_specimen(
        "S-04a",
        [touch("include/sluice/async/detail/request_arena.hpp",
               "    std::size_t reap(SynchronousReadySink& sink) {",
               indent="        ")],
        "F04 arena reap facet row")})
    plan.append({"id": "S-04b", "run": lambda d: d.diff_file_specimen(
        "S-04b",
        [touch("include/sluice/async/completion.hpp",
               "    void publish_from_reap(Result<T>&& res) noexcept {",
               indent="        ")],
        "F04 ready-release LP facet row")})
    plan.append({"id": "S-06a", "run": lambda d: d.diff_file_specimen(
        "S-06a",
        [touch("src/async/cancel.cpp", "void CancelToken::request() noexcept {")],
        "F06 request-epoch facet row")})
    plan.append({"id": "S-06b", "run": lambda d: d.diff_file_specimen(
        "S-06b",
        [touch("src/async/cancel.cpp",
               "Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept {")],
        "F06 delivery-gate facet row")})
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


def expected_row_keys(prereg: dict) -> list[tuple[str, str]]:
    """Every (specimen, claim) row the preregistration requires the live
    corpus to produce (corrective-1 C4). B9-style class-only expectations
    and the hermetic B10/B12 shapes require no specific claim row."""
    keys: list[tuple[str, str]] = []
    for spec in prereg["b_corpus_expectations"]:
        exp = spec.get("expected") or {}
        if "rows" in exp:
            for row in exp["rows"]:
                for claim in row.get("claims", []):
                    keys.append((spec["id"], claim))
        else:
            for claim in exp.get("claims", []):
                keys.append((spec["id"], claim))
    for spec in prereg.get("supplementary_precise_rows", []):
        for claim in (spec.get("expected") or {}).get("claims", []):
            keys.append((spec["id"], claim))
    return keys


def compare_with_prereg(all_rows: list[dict], prereg: dict) -> dict:
    """Frozen-preregistration comparison with FULL corpus coverage
    (corrective-1 C4): missing expected rows/specimens, unexpected extra
    claim rows, and duplicate rows are all reported explicitly — the old
    comparison only iterated rows that existed, so a missing row could
    silently disappear. Unexpected rows remain evidence (not failures);
    missing required rows fail the run via compute_integrity."""
    expected_keys = set(expected_row_keys(prereg))
    expectations = {e["id"]: e for e in prereg["b_corpus_expectations"]}
    expectations.update(
        {e["id"]: e for e in prereg.get("supplementary_precise_rows", [])}
    )

    counts: dict[tuple[str, str], int] = {}
    for r in all_rows:
        key = (r["specimen"], r["claim"])
        counts[key] = counts.get(key, 0) + 1

    def is_unexpected(spec: str, claim: str) -> bool:
        """A row is unexpected only when the preregistration says something
        definite about the specimen and this claim is not part of it. A
        class-only expectation (e.g. B9, no named claims) covers whatever
        claim rows the specimen legitimately produces; an unknown specimen
        is always unexpected."""
        e = expectations.get(spec)
        if e is None:
            return True
        exp = e.get("expected") or {}
        if "rows" in exp:
            return not any(r.get("claims") == [claim] for r in exp["rows"])
        claims = exp.get("claims") or []
        if claims:
            return claim not in claims
        return False

    missing = sorted(expected_keys - set(counts))
    unexpected = sorted(k for k in counts if is_unexpected(*k))
    duplicates = sorted(k for k, n in counts.items() if n > 1)

    deviations: list[dict] = []
    matches = 0
    checked = 0
    for row in all_rows:
        key = (row["specimen"], row["claim"])
        if key in unexpected:
            continue  # reported as evidence above; no value comparison
        spec = expectations.get(row["specimen"])
        if not spec:
            continue
        exp = spec.get("expected") or {}
        if "rows" in exp:
            exp_row = next(
                (r for r in exp["rows"] if r.get("claims") == [row["claim"]]), None
            )
            if exp_row is None:
                continue
            exp = exp_row
        elif row["claim"] not in (exp.get("claims") or []):
            if exp.get("claims"):
                continue
            # class-only shape (e.g. B9): the expectation constrains the
            # claim rows it produces without naming them
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
        if "fail_closed" in exp and exp["fail_closed"] != row["fail_closed"]:
            problems.append(f"fail_closed {row['fail_closed']} != {exp['fail_closed']}")
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
        "rows_expected": sorted([list(k) for k in expected_keys]),
        "rows_present": len(all_rows),
        "rows_checked": checked,
        "rows_matching": matches,
        "missing_expected_rows": [{"specimen": s, "claim": c} for s, c in missing],
        "missing_specimens": sorted({s for s, _ in missing}),
        "unexpected_rows": [
            {"specimen": s, "claim": c,
             "note": "not a preregistered required claim row; retained as evidence"}
            for s, c in unexpected
        ],
        "duplicate_rows": [
            {"specimen": s, "claim": c, "occurrences": counts[(s, c)]}
            for s, c in duplicates
        ],
        "deviations": deviations,
    }


def compute_integrity(all_rows: list[dict], records: list[dict],
                      required_rows: list[tuple[str, str]],
                      precise_keys: list[tuple[str, str]]) -> dict:
    """Fail-closed corpus integrity (corrective-1 C3): any specimen error /
    missing result, any missing required prereg row, or any duplicate row
    fails the corpus (integrity.all_pass = false -> non-zero exit), and a
    specimen error or an imperfect frozen PRECISE row population makes the
    threshold NON-authoritative (met = null) so the denominator can never
    be silently shrunk."""
    specimen_errors = [
        {"id": r.get("id", "?"), "error": r.get("error", "specimen produced no result")}
        for r in records
        if "error" in r or "result" not in r
    ]
    counts: dict[tuple[str, str], int] = {}
    for r in all_rows:
        key = (r["specimen"], r["claim"])
        counts[key] = counts.get(key, 0) + 1
    missing_required = sorted(set(required_rows) - set(counts))
    duplicates = sorted(k for k, n in counts.items() if n > 1)
    frozen_complete = (
        not specimen_errors
        and all(counts.get(k, 0) == 1 for k in precise_keys)
    )
    return {
        "specimen_errors": specimen_errors,
        "missing_required_rows": [{"specimen": s, "claim": c} for s, c in missing_required],
        "unexpected_rows": sorted([list(k) for k in set(counts) - set(required_rows)]),
        "duplicate_rows": [
            {"specimen": s, "claim": c, "occurrences": counts[(s, c)]}
            for s, c in duplicates
        ],
        "frozen_precise_rows_complete": frozen_complete,
        "all_pass": not specimen_errors and not missing_required and not duplicates,
    }


def measurement_section(all_rows: list[dict], precise_keys: list[tuple[str, str]],
                        integrity: dict) -> dict:
    precise_rows = []
    for key in precise_keys:
        matched = [r for r in all_rows if (r["specimen"], r["claim"]) == key]
        if len(matched) == 1:
            precise_rows.append(matched[0])
    precise_key_set = {(r["specimen"], r["claim"]) for r in precise_rows}
    supp = [
        r for r in all_rows
        if r["specimen"] in SUPPLEMENTARY_ROWS and r["facet_scope"] == "PRECISE"
    ]
    supp_keys = {(r["specimen"], r["claim"]) for r in supp}
    conservative = [
        r for r in all_rows
        if (r["specimen"], r["claim"]) not in precise_key_set
        and (r["specimen"], r["claim"]) not in supp_keys
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
    b_stats = stats(precise_rows)
    # Authoritative only when every frozen row key is present exactly once
    # and no specimen errored (corrective-1 C3); otherwise met is null and
    # FACETS_EARNED is unreachable.
    authoritative = bool(integrity["frozen_precise_rows_complete"]) and \
        len(precise_rows) == len(precise_keys)
    mean = b_stats["mean_reduction"]
    return {
        "precise_b1_b7": b_stats,
        "supplementary_precise": stats(supp),
        "conservative_cases": {k: v for k, v in stats(conservative).items() if k != "mean_reduction"},
        "threshold": {
            "metric": "mean reduction over the frozen PRECISE B1-B7 (specimen, claim) row keys",
            "value": THRESHOLD,
            "required_row_keys": sorted([list(k) for k in precise_keys]),
            "rows_present": len(precise_rows),
            "authoritative": authoritative,
            "met": (mean >= THRESHOLD) if (authoritative and mean is not None) else None,
        },
    }


# --- main ---------------------------------------------------------------------


def run_corpus() -> dict:
    started = time.time()
    driver = Driver()
    # Freshness precondition: diff-file specimens trust the on-disk graph,
    # so the corpus MUST start from an index built at the current HEAD.
    driver.reindex()
    prereg = load_prereg()
    precise_keys = frozen_precise_row_keys(prereg)
    required_rows = expected_row_keys(prereg)
    records = []
    for entry in build_plan():
        try:
            record = entry["run"](driver)
            if not isinstance(record, dict) or "id" not in record:
                record = {"id": entry["id"], "error": "specimen record is missing its id"}
            elif "result" not in record and "error" not in record:
                record = dict(record, error="specimen produced no result")
        except Exception as exc:  # noqa: BLE001 — a failed specimen is evidence AND fail-closed (corrective-1 C3)
            record = {"id": entry["id"], "error": f"{type(exc).__name__}: {exc}"}
        finally:
            driver.restore()
        records.append(record)
    # resync the world for later consumers
    driver.reindex()

    all_rows = []
    for r in records:
        if "result" in r:
            all_rows.extend(extract_rows(r))
    integrity = compute_integrity(all_rows, records, required_rows, precise_keys)
    measurement = measurement_section(all_rows, precise_keys, integrity)
    comparison = compare_with_prereg(all_rows, prereg)

    summary = {
        "experiment": "FDG-0 Phase B formal facets adversarial corpus (issue #298)",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "head_sha": sh("git", "rev-parse", "HEAD").strip(),
        "prereg": str(PREREG_PATH.relative_to(REPO_ROOT)),
        "frozen_precise_row_keys": [list(k) for k in precise_keys],
        "b10_b12_coverage": (
            "hermetic: scripts/tests/test_formal_impact_facets.py "
            "(FR6 stale demotion, FR7 unresolved never PRECISE, FV1-FV16 "
            "registry structural negatives incl. corrective-1 C5 trace-vocab "
            "fail-closed)"
        ),
        "specimens": records,
        "rows": all_rows,
        "integrity": integrity,
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
    integrity = summary["integrity"]
    comp = summary["prereg_comparison"]
    print(f"==> PRECISE B1-B7 (frozen keys): {m['precise_b1_b7']}")
    print(f"==> threshold: authoritative={m['threshold']['authoritative']} "
          f"met={m['threshold']['met']}")
    print(f"==> integrity: all_pass={integrity['all_pass']} "
          f"specimen_errors={len(integrity['specimen_errors'])} "
          f"missing_required={len(integrity['missing_required_rows'])} "
          f"duplicates={len(integrity['duplicate_rows'])} "
          f"unexpected_rows={len(integrity['unexpected_rows'])}")
    print(f"==> prereg: {comp['rows_matching']}/{comp['rows_checked']} rows match, "
          f"{len(comp['deviations'])} deviation(s), "
          f"{len(comp['missing_expected_rows'])} missing, "
          f"{len(comp['unexpected_rows'])} unexpected")
    # Fail-closed exit contract (corrective-1 C3)
    return 0 if integrity["all_pass"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
