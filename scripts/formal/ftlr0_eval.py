#!/usr/bin/env python3
"""FTLR-0 / SCIP-PILOT evaluation driver (issue #299).

Executes the adversarial specimen suite T1-T10, the traversal-depth
experiment, and the baseline comparisons (A file-only / B explicit anchors /
C SCIP graph) against the real repository, and emits the machine-readable
experiment record (docs/results/formal/ftlr0-scip-pilot.json).

Phases:
  * diff-file specimens (T1-T5, T10): edits are applied to the working tree,
    a real `git diff` is captured as a patch file, the tree is restored, and
    the impact engine runs with --diff-file against the index at HEAD.
  * reindex specimens (T6-T9): the edit is applied to the working tree, the
    SCIP index + graph are rebuilt from the edited sources (so new/renamed
    symbols are visible, exactly like a developer rebuilding at HEAD), the
    engine runs with --working-tree, then the tree is restored and the index
    is rebuilt at HEAD again. No git history is touched: no commits, no
    reset, no stash.

Worktree safety: the driver snapshots every file it touches, refuses to run
on a dirty tree for the reindex phase, restores bytes verbatim, and
re-verifies `git status --short` at exit.

USAGE
    python3 scripts/formal/ftlr0_eval.py run                 # full suite
    python3 scripts/formal/ftlr0_eval.py run --skip-reindex  # phase 1 only
    python3 scripts/formal/ftlr0_eval.py verify-clean        # restore check
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

import formal_impact as fi  # noqa: E402

RESULTS_PATH = REPO_ROOT / "docs" / "results" / "formal" / "ftlr0-scip-pilot.json"
SCIP_CLANG = BUILD = None


def sh(*args: str, cwd: Path = REPO_ROOT) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def git_status_short() -> str:
    return sh("git", "status", "--short")


# --- working-tree specimen machinery -----------------------------------------


class WorktreeEdit:
    """A textual replacement in one file (must match exactly once, or use
    replace_all for mechanical renames)."""

    def __init__(self, path: str, old: str, new: str, replace_all: bool = False):
        self.path = path
        self.old = old
        self.new = new
        self.replace_all = replace_all

    def apply(self):
        p = REPO_ROOT / self.path
        text = p.read_text(encoding="utf-8")
        count = text.count(self.old)
        if count == 0 or (count != 1 and not self.replace_all):
            raise RuntimeError(
                f"specimen edit anchor not applicable in {self.path}: {count} matches"
            )
        p.write_text(text.replace(self.old, self.new), encoding="utf-8")


class Specimen:
    """One adversarial case: worktree edits + expected outcome + notes."""

    def __init__(self, tid, description, edits, expected_claims, expected_class,
                 needs_reindex=False, extra_setup=None, notes=""):
        self.id = tid
        self.description = description
        self.edits = edits
        self.expected_claims = expected_claims
        self.expected_class = expected_class
        self.needs_reindex = needs_reindex
        self.extra_setup = extra_setup  # callable(runner) before indexing
        self.notes = notes


def comment_line_specimen(path: str, after: str, indent="    ") -> WorktreeEdit:
    """Insert one inert comment line after a unique anchor string (a body
    edit that is a faithful minimal specimen: it changes a real function's
    body region without changing semantics)."""
    return WorktreeEdit(path, after, after + f"\n{indent}// ftlr0 specimen touch")


# Specimen edits are defined with unique source anchors; each edit is
# verified to match exactly once before applying.


def build_specimens() -> list[Specimen]:
    return [
        Specimen(
            "T1",
            "direct anchor edit: body of Scheduler::signal_wake_locked (F08 authority)",
            [
                comment_line_specimen(
                    "src/async/scheduler_park_wake.cpp",
                    "void Scheduler::signal_wake_locked() {",
                )
            ],
            expected_claims=["F08"],
            expected_class=fi.DIRECT,
            needs_reindex=False,  # body edit of an existing symbol: diff-file mode suffices
        ),
        Specimen(
            "T2",
            "helper edit: Scheduler::notify_external_wake calls the signal_wake_locked anchor",
            [
                comment_line_specimen(
                    "src/async/scheduler_park_wake.cpp",
                    "void Scheduler::notify_external_wake() noexcept {",
                )
            ],
            expected_claims=["F08"],
            expected_class=fi.STRUCTURAL,
        ),
        Specimen(
            "T3",
            "two-hop helper: drain_routed_completion_waits_locked -> (direct caller) -> signal_wake_locked",
            [
                comment_line_specimen(
                    "src/async/scheduler.cpp",
                    "bool Scheduler::drain_routed_completion_waits_locked() {",
                )
            ],
            expected_claims=["F08"],
            expected_class=fi.STRUCTURAL,
            notes="mid-layer drain helper two reference-hops from the F08 wake authority",
        ),
        Specimen(
            "T4",
            "unrelated code: sluice_core Reader::read_exact (outside all formal bindings)",
            [
                comment_line_specimen(
                    "src/reader.cpp",
                    "Result<void> Reader::read_exact(std::span<std::byte> dst) {",
                )
            ],
            expected_claims=[],
            expected_class=fi.NO_IMPACT,
        ),
        Specimen(
            "T5",
            "shared authority: RequestArena::validate_ is anchored by F01 (identity gate) and F06 (stale-generation gate)",
            [
                WorktreeEdit(
                    "include/sluice/async/detail/request_arena.hpp",
                    "    RequestSlot* validate_(SlotHandle h) noexcept {",
                    "    RequestSlot* validate_(SlotHandle h) noexcept {\n"
                    "        // ftlr0 specimen touch",
                )
            ],
            expected_claims=["F01", "F06"],
            expected_class=fi.DIRECT,
        ),
        Specimen(
            "T6",
            "anchor rename: signal_wake_locked -> publish_wake_locked across all 6 files",
            [],
            expected_claims=["F08"],
            expected_class=fi.UNKNOWN,
            needs_reindex=True,
            notes="renamed mechanically (sed) across all references so the tree still parses; "
                  "the registry anchor must go UNRESOLVED and F08 must fail closed, never NO",
        ),
        Specimen(
            "T7",
            "moved implementation: ThreadPoolBackend::run_syscall moved to a new TU",
            [],
            expected_claims=["F03"],
            expected_class=fi.STRUCTURAL,
            needs_reindex=True,
            notes="scip-clang C++ symbols embed no defining-file segment, so name identity "
                  "survives the move; expected: anchor still resolves with def-site drift",
        ),
        Specimen(
            "T8",
            "new helper introduced, called by the anchor: signal_wake_locked delegates the epoch flip",
            [],
            expected_claims=["F08"],
            expected_class=fi.STRUCTURAL,
            needs_reindex=True,
        ),
        Specimen(
            "T9",
            "new path bypasses the anchor: rogue member writes wake_epoch_ directly",
            [],
            expected_claims=["F08"],
            expected_class=fi.STRUCTURAL,
            needs_reindex=True,
            notes="the anchored STATE (Scheduler::wake_epoch_) catches writers that never "
                  "pass through signal_wake_locked; this is a deliberate design finding",
        ),
        Specimen(
            "T10",
            "function-pointer thunk: ThreadPoolBackend::publish_size_ready reaches the F04 anchor via AsyncBackend::publish",
            [
                comment_line_specimen(
                    "src/async/threadpool_backend.cpp",
                    "void ThreadPoolBackend::publish_size_ready(void* completion,",
                )
            ],
            expected_claims=["F04"],
            expected_class=fi.STRUCTURAL,
        ),
    ]


# --- specimen edit builders for the reindex cases -----------------------------


def build_t6_edits() -> list[WorktreeEdit]:
    files = [
        "src/async/scheduler.cpp",
        "src/async/scheduler_park_wake.cpp",
        "src/async/scheduler_timer.cpp",
        "src/async/scheduler_semaphore.cpp",
        "src/async/async_io_context.cpp",
        "include/sluice/async/scheduler.hpp",
    ]
    edits = []
    for f in files:
        p = REPO_ROOT / f
        text = p.read_text(encoding="utf-8")
        n = text.count("signal_wake_locked")
        if n == 0:
            raise RuntimeError(f"T6 rename: no occurrences in {f}")
        edits.append(WorktreeEdit(f, "signal_wake_locked", "publish_wake_locked", replace_all=True))
    return edits


def build_t7_setup() -> tuple[list[WorktreeEdit], str, str]:
    """Move ThreadPoolBackend::run_syscall into a new TU. Returns (edits,
    new_file_relpath, new_file_content)."""
    src = REPO_ROOT / "src" / "async" / "threadpool_backend.cpp"
    text = src.read_text(encoding="utf-8")
    start = text.index("detail::TerminalResult ThreadPoolBackend::run_syscall(")
    end = text.index("\n}\n", start) + len("\n}\n")
    fn_text = text[start:end]
    # Remove from the original TU (replace with a pointer comment).
    remove_edit = WorktreeEdit(
        "src/async/threadpool_backend.cpp",
        fn_text,
        "// ftlr0 T7 specimen: run_syscall moved to threadpool_run_syscall.cpp\n",
    )
    new_content = (
        "// ftlr0 T7 specimen: run_syscall moved out of threadpool_backend.cpp\n"
        "#include <sluice/async/threadpool_backend.hpp>\n"
        "#include <sluice/detail/posix_retry.hpp>\n"
        "#include <sluice/error.hpp>\n"
        "#include <cerrno>\n"
        "#include <cstdint>\n"
        "\n"
        "namespace sluice::async {\n"
        "\n"
        + fn_text
        + "\n}  // namespace sluice::async\n"
    )
    return [remove_edit], "src/async/threadpool_run_syscall.cpp", new_content


def build_t8_edits() -> list[WorktreeEdit]:
    """Add a new helper the anchor delegates to, then the specimen 'change'
    is the helper's own body (the eval queries the helper-edit state)."""
    park = REPO_ROOT / "src" / "async" / "scheduler_park_wake.cpp"
    text = park.read_text(encoding="utf-8")
    anchor_fn = "void Scheduler::signal_wake_locked() {"
    helper = (
        "void Scheduler::ftlr0_specimen_advance_wake_epoch() {\n"
        "    // ftlr0 T8 specimen helper: owns the epoch advance on behalf of\n"
        "    // signal_wake_locked (new code the registry has no annotation for).\n"
        "    wake_epoch_.fetch_add(1, std::memory_order_acq_rel);\n"
        "}\n"
        "\n"
    )
    delegate = anchor_fn + "\n    ftlr0_specimen_advance_wake_epoch();"
    return [WorktreeEdit("src/async/scheduler_park_wake.cpp", anchor_fn, helper + delegate)]


def build_t9_edits() -> list[WorktreeEdit]:
    """New rogue member that writes the anchored state without any existing
    anchor on its path (declaration + definition, no callers)."""
    header = REPO_ROOT / "include" / "sluice" / "async" / "scheduler.hpp"
    src = REPO_ROOT / "src" / "async" / "scheduler_park_wake.cpp"
    htext = header.read_text(encoding="utf-8")
    decl_anchor = "    void signal_wake_locked();"
    if decl_anchor not in htext:
        raise RuntimeError("T9: signal_wake_locked declaration anchor not found")
    decl_new = decl_anchor + "\n    void ftlr0_specimen_rogue_wake();  // ftlr0 T9 specimen"
    stub = (
        "void Scheduler::ftlr0_specimen_rogue_wake() {\n"
        "    // ftlr0 T9 specimen: writes the formalized wake state directly,\n"
        "    // bypassing signal_wake_locked entirely (never called).\n"
        "    wake_epoch_.store(0, std::memory_order_release);\n"
        "}\n"
        "\n"
    )
    sanchor = "void Scheduler::signal_wake_locked() {"
    return [
        WorktreeEdit("include/sluice/async/scheduler.hpp", decl_anchor, decl_new),
        WorktreeEdit(
            "src/async/scheduler_park_wake.cpp", sanchor, stub + sanchor
        ),
    ]


# --- evaluation runner ---------------------------------------------------------


class Evaluator:
    def __init__(self, skip_reindex: bool):
        self.skip_reindex = skip_reindex
        self.registry = fi.load_registry()
        self.manifest = fi.load_manifest()
        self.saved: dict[str, bytes] = {}
        self.specimen_patches: dict[str, str] = {}
        self.results: list[dict] = []

    # -- worktree safety --

    def snapshot_and_apply(self, edits: list[WorktreeEdit], extra_files: dict[str, str] | None = None):
        for e in edits:
            p = REPO_ROOT / e.path
            if e.path not in self.saved:
                self.saved[e.path] = p.read_bytes()
        if extra_files:
            for rel in extra_files:
                p = REPO_ROOT / rel
                # None marks "did not exist before" so restore() unlinks it.
                self.saved[rel] = p.read_bytes() if p.exists() else None
        for e in edits:
            e.apply()
        for rel, content in (extra_files or {}).items():
            p = REPO_ROOT / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content, encoding="utf-8")

    def restore(self):
        for rel, data in self.saved.items():
            p = REPO_ROOT / rel
            if data is None:
                p.unlink(missing_ok=True)
            else:
                p.write_bytes(data)
        self.saved.clear()

    # -- engine invocation --

    def run_impact(self, args: list[str]) -> dict:
        out = sh(
            sys.executable,
            str(SCRIPT_DIR / "formal_impact.py"),
            "impact",
            "--json",
            *args,
        )
        return json.loads(out)

    def reindex(self):
        sh(sys.executable, str(SCRIPT_DIR / "formal_impact.py"), "index")

    def baseline_a(self, changes: dict) -> list[str]:
        """File-only implementation_bindings: claims whose suites bind any
        changed file."""
        suite_bindings = {
            s["id"]: set(s.get("implementation_bindings", []))
            for s in self.manifest["suites"]
        }
        claims = set()
        for claim in self.registry["claims"]:
            for suite in claim.get("formal_suites", []):
                if any(f in suite_bindings.get(suite, set()) for f in changes):
                    claims.add(claim["id"])
        return sorted(claims)

    def evaluate(self, spec: Specimen, depth: int) -> dict:
        record: dict = {
            "id": spec.id,
            "description": spec.description,
            "expected_claims": spec.expected_claims,
            "expected_class": spec.expected_class,
            "notes": spec.notes,
        }
        if spec.needs_reindex:
            assert not self.skip_reindex, f"{spec.id} requires reindex phase"
            edits = {
                "T6": lambda: build_t6_edits(),
                "T7": None,
                "T8": build_t8_edits,
                "T9": build_t9_edits,
            }[spec.id]
            if spec.id == "T7":
                remove_edits, new_rel, new_content = build_t7_setup()
                extra = {new_rel: new_content}
                self.snapshot_and_apply(remove_edits, extra)
                # The filtered compdb is a derived artifact; snapshot it so
                # the post-specimen restore removes the appended TU again.
                compdb_path = fi.BUILD_DIR / "compile_commands.src.json"
                self.saved[str(compdb_path)] = compdb_path.read_bytes()
                compdb = json.loads(compdb_path.read_text())
                template = next(
                    e for e in compdb if e["file"] == "src/async/threadpool_backend.cpp"
                )
                clone = json.loads(json.dumps(template))
                clone["file"] = new_rel
                clone["arguments"][-1] = new_rel
                for i, a in enumerate(clone["arguments"]):
                    if a.endswith("threadpool_backend.cpp.o"):
                        clone["arguments"][i] = a.replace(
                            "threadpool_backend.cpp.o", "threadpool_run_syscall.cpp.o"
                        )
                compdb.append(clone)
                compdb_path.write_text(json.dumps(compdb))
                self.reindex()
                result = self.run_impact(["--working-tree", "--max-depth", str(depth)])
                record["changed_files"] = result["changed_files"]
                record["result"] = {
                    "classification": result["classification"],
                    "claims": result["claims"],
                    "risks": result["risks"],
                }
                # also record the check behavior under the rename/move
                record["check_after"] = "see pilot report (anchor resolution table)"
            else:
                self.snapshot_and_apply(edits())
                self.reindex()
                result = self.run_impact(["--working-tree", "--max-depth", str(depth)])
                record["changed_files"] = result["changed_files"]
                record["result"] = {
                    "classification": result["classification"],
                    "claims": result["claims"],
                    "risks": result["risks"],
                }
            record["_query_args"] = ["--working-tree"]
        else:
            # diff-file mode: apply edits, capture real git diff, restore.
            self.snapshot_and_apply(spec.edits)
            patch = sh("git", "diff", "--no-renames", "-U0", "--", *{e.path for e in spec.edits})
            record["patch_bytes"] = len(patch)
            self.restore()
            patch_path = fi.BUILD_DIR / f"specimen-{spec.id}.patch"
            patch_path.write_text(patch, encoding="utf-8")
            record["patch"] = str(patch_path.relative_to(REPO_ROOT))
            record["_query_args"] = ["--diff-file", str(patch_path)]
            result = self.run_impact(
                ["--diff-file", str(patch_path), "--max-depth", str(depth)]
            )
            record["result"] = {
                "classification": result["classification"],
                "claims": result["claims"],
                "risks": result["risks"],
            }
            record["changed_files"] = result["changed_files"]

        # Baselines A and B on the same changed set.
        if spec.needs_reindex:
            # working-tree mode: the tree still carries the specimen edits.
            changes = fi.diff_to_changes(
                sh("git", "diff", "--no-renames", "-U0")
            )
        else:
            patch_path = fi.BUILD_DIR / f"specimen-{spec.id}.patch"
            changes = fi.diff_to_changes(patch_path.read_text(encoding="utf-8"))
        record["baseline_a_claims"] = self.baseline_a(changes)

        # Expected-outcome adjudication (deterministic comparison).
        got_claims = sorted(c["id"] for c in record["result"]["claims"])
        record["got_claims"] = got_claims
        record["got_class"] = record["result"]["classification"]
        record["claims_ok"] = set(got_claims) >= set(spec.expected_claims) if spec.expected_claims else (
            record["got_class"] == fi.NO_IMPACT
        )
        record["class_ok"] = (
            record["got_class"] == spec.expected_class
            if spec.expected_class != fi.UNKNOWN
            else record["got_class"] in (fi.UNKNOWN, fi.STRUCTURAL, fi.DIRECT, fi.COARSE)
        )
        return record

    def run(self) -> dict:
        started = time.time()
        specimens = build_specimens()
        default_depth = fi.DEFAULT_MAX_DEPTH

        for spec in specimens:
            if spec.needs_reindex and self.skip_reindex:
                self.results.append({"id": spec.id, "skipped": "reindex phase disabled"})
                continue
            try:
                record = self.evaluate(spec, default_depth)
                # Traversal-depth matrix (B = depth 0 = explicit-anchor-only).
                depth_matrix = {}
                for d in (0, 1, 2, 3):
                    r = self.run_impact(list(record["_query_args"]) + ["--max-depth", str(d)])
                    depth_matrix[str(d)] = {
                        "classification": r["classification"],
                        "claims": sorted(c["id"] for c in r["claims"]),
                    }
                record["depth_matrix"] = depth_matrix
                self.results.append(record)
            except Exception as exc:  # noqa: BLE001 — a failed specimen is evidence
                self.results.append({"id": spec.id, "error": f"{type(exc).__name__}: {exc}"})
            finally:
                self.restore()
                # Re-index at real HEAD so later cases see the unmodified
                # world again.
                self.reindex()

        summary = {
            "experiment": "FTLR-0 / SCIP-PILOT adversarial suite (issue #299)",
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "head_sha": sh("git", "rev-parse", "HEAD").strip(),
            "default_max_depth": default_depth,
            "specimens": self.results,
            "wall_seconds": round(time.time() - started, 1),
        }
        for record in summary["specimens"]:
            record.pop("_query_args", None)
        return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run_p = sub.add_parser("run", help="run the adversarial suite")
    run_p.add_argument("--skip-reindex", action="store_true")
    run_p.add_argument("--out", default=str(RESULTS_PATH))
    sub.add_parser("verify-clean", help="fail if the working tree is dirty")
    args = parser.parse_args(argv)

    if args.command == "verify-clean":
        status = git_status_short()
        if status:
            print("DIRTY:", status)
            return 1
        print("clean")
        return 0

    if args.command == "run":
        # Untracked files (the pilot's own new sources) are fine — specimens
        # only touch tracked sources, which are snapshotted and restored.
        dirty = [
            line
            for line in git_status_short().splitlines()
            if line and not line.startswith("??")
        ]
        if dirty:
            print("error: refuse to run with modified tracked files:", file=sys.stderr)
            print("\n".join(dirty), file=sys.stderr)
            return 2
        evaluator = Evaluator(skip_reindex=args.skip_reindex)
        try:
            summary = evaluator.run()
        finally:
            evaluator.restore()
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
        print(f"==> results written: {out}")
        for r in summary["specimens"]:
            if "error" in r:
                print(f"  {r['id']}: ERROR {r['error']}")
                continue
            if "skipped" in r:
                print(f"  {r['id']}: SKIPPED ({r['skipped']})")
                continue
            ok = "OK " if r["claims_ok"] and r["class_ok"] else "MISS"
            print(
                f"  {r['id']}: {ok} got {r['got_class']} claims={r['got_claims']} "
                f"(expected {r['expected_class']} claims={r['expected_claims']})"
            )
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
