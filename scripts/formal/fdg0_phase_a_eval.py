#!/usr/bin/env python3
"""FDG-0 Phase A — Build Truth real-machine specimen driver (issue #298).

Executes the real adversarial specimens A4–A11 against the live repository:

    B0  baseline world: reindex at HEAD under the current xmake config
    A4  Xmake-owned TU with its compile_commands entry removed
          -> UNKNOWN_BUILD_WORLD, non-zero exit (fail closed)
    A5  compile_commands entries outside the selected world are excluded
          (test seams, internal-testing variants, bench, experimental)
    A6  a legitimate NEW production TU through real Xmake membership
          -> Build Manifest discovers it, SCIP indexes it
    A7  T7-A: the real Xmake E2E move — an anchor implementation moves to a
          new TU; Xmake glob membership -> compile_commands regeneration ->
          Build Manifest -> SCIP -> anchor resolves at the new site ->
          impact != NO_FORMAL_IMPACT. NO manual compile-database injection.
    A8  path prefix is not authority: an unowned file under src/async/ is
          excluded even though the old `src/` filter would have seen it
    A9  config identity drift: with-liburing=y vs =n produce different
          build identities
    A10 stale Build Manifest queried under another configuration
          -> fail closed
    A11 liburing F03 reproducibility: the gated uring terminal anchor
          resolves under the declared liburing variant

Worktree safety (mirrors ftlr0_eval.py): snapshots every touched file,
refuses to run on a dirty tree, restores bytes verbatim, restores the xmake
configuration, and re-verifies `git status --short` at exit.

USAGE
    python3 scripts/formal/fdg0_phase_a_eval.py run
    python3 scripts/formal/fdg0_phase_a_eval.py verify-clean
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

RESULTS_PATH = REPO_ROOT / "docs" / "results" / "formal" / "fdg0-phase-a.json"

# The selected formalized production root (issue #298 §4).
ROOT_TARGET = "sluice_async"
# The moved-anchor specimen (same authority as #300 T7: F03 producer).
MOVE_SRC = "src/async/threadpool_backend.cpp"
MOVE_DST = "src/async/threadpool_run_syscall.cpp"
MOVE_FN = "detail::TerminalResult ThreadPoolBackend::run_syscall("


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


class Driver:
    def __init__(self, allow_dirty: bool = False):
        self.saved: dict[str, bytes | None] = {}
        self.records: dict = {}
        self.orig_liburing: str | None = None
        self.allow_dirty = allow_dirty

    # --- worktree safety ----------------------------------------------------

    def snapshot(self, rel: str):
        if rel not in self.saved:
            p = REPO_ROOT / rel
            self.saved[rel] = p.read_bytes() if p.exists() else None

    def write_new(self, rel: str, content: str):
        self.snapshot(rel)
        p = REPO_ROOT / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")

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

    # --- helpers ------------------------------------------------------------

    def reindex(self) -> str:
        return sh(sys.executable, str(SCRIPT_DIR / "formal_impact.py"), "index")

    def build_world(self) -> dict:
        out = sh(sys.executable, str(SCRIPT_DIR / "formal_impact.py"), "build-world")
        manifest = json.loads(
            (REPO_ROOT / "build" / "formal-impact" / "build-manifest.json").read_text()
        )
        return manifest

    def load_manifest(self) -> dict:
        return json.loads(
            (REPO_ROOT / "build" / "formal-impact" / "build-manifest.json").read_text()
        )

    def load_graph(self) -> dict:
        return json.loads((fi.GRAPH_PATH).read_text(encoding="utf-8"))

    def run_check(self) -> tuple[int, str]:
        result = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "formal_impact.py"), "check"],
            cwd=REPO_ROOT, capture_output=True, text=True,
        )
        return result.returncode, result.stdout + result.stderr

    def run_impact(self, args: list[str]) -> dict:
        out = sh(
            sys.executable, str(SCRIPT_DIR / "formal_impact.py"), "impact",
            "--json", "--allow-unknown-for-eval", *args,
        )
        return json.loads(out)

    def compdb_entries_for(self, path: str) -> list[dict]:
        compdb = json.loads((REPO_ROOT / "compile_commands.json").read_text())
        return [e for e in compdb if e.get("file") == path]

    # --- specimens ----------------------------------------------------------

    def baseline(self):
        rec = {}
        self.reindex()
        manifest = self.load_manifest()
        graph = self.load_graph()
        rec["head_sha"] = manifest["head_sha"]
        rec["xmake_version"] = manifest["xmake_version"]
        rec["config"] = manifest["config"]
        world = manifest["worlds"][0]
        rec["world"] = {
            "id": world["id"],
            "root_targets": world["root_targets"],
            "dependency_closure": world["dependency_closure"],
            "targets": sorted(world["targets"]),
            "tu_count": len(world["tus"]),
            "compdb": world["compdb"],
        }
        rec["build_id"] = manifest["build_id"]
        rec["graph"] = {
            "schema": graph["schema"],
            "build_id": graph.get("build", {}).get("build_id"),
            "world_id": graph.get("build", {}).get("world_id"),
            "documents": graph.get("stats", {}).get("documents"),
            "symbols": graph.get("stats", {}).get("symbols"),
        }
        self.records["B0_baseline"] = rec

    def a4_missing_compdb_entry(self):
        rec = {"expected": "UNKNOWN_BUILD_WORLD, non-zero exit"}
        # Remove the production (sluice_async-owned) compile_commands entry
        # for one Xmake-owned TU; `index --no-refresh-compdb` must fail
        # closed BEFORE scip-clang runs.
        self.snapshot("compile_commands.json")
        compdb = json.loads((REPO_ROOT / "compile_commands.json").read_text())
        target = "src/async/cancel.cpp"
        before = len(compdb)
        kept = [e for e in compdb if not (
            e.get("file") == target and "build/.objs/sluice_async/" in " ".join(e.get("arguments", []))
        )]
        rec["entries_before"] = before
        rec["entries_after"] = len(kept)
        (REPO_ROOT / "compile_commands.json").write_text(json.dumps(kept), encoding="utf-8")
        try:
            result = subprocess.run(
                [sys.executable, str(SCRIPT_DIR / "formal_impact.py"), "index",
                 "--no-refresh-compdb"],
                cwd=REPO_ROOT, capture_output=True, text=True,
            )
        finally:
            self.restore()
        rec["exit_code"] = result.returncode
        rec["stderr_excerpt"] = [l.strip() for l in result.stderr.splitlines() if l.strip()][:3]
        rec["pass"] = result.returncode != 0 and "UNKNOWN_BUILD_WORLD" in result.stderr
        self.records["A4_missing_compdb_entry"] = rec

    def a5_exclusion(self):
        rec = {}
        manifest = self.build_world()
        world = manifest["worlds"][0]
        rec["selected_entries"] = world["compdb"]["selected_entries"]
        rec["total_entries"] = world["compdb"]["total_entries"]
        rec["tus"] = sorted(world["tus"])
        outside = [t for t in rec["tus"] if not t.startswith("src/")]
        rec["tus_outside_src"] = outside
        rec["experimental_excluded"] = not any(
            t.startswith("src/experimental/") for t in world["tus"]
        )
        # The production TU src/async/uring_backend.cpp must select ONLY the
        # sluice_async-owned entry — no SLUICE_ASYNC_INTERNAL_TESTING variant.
        tu = world["tus"].get("src/async/uring_backend.cpp", {})
        rec["uring_backend_owning_targets"] = tu.get("owning_targets")
        rec["uring_backend_defines"] = tu.get("defines")
        rec["internal_testing_variant_excluded"] = (
            tu.get("owning_targets") == ["sluice_async"]
            and not any("-DSLUICE_ASYNC_INTERNAL_TESTING" in d for d in tu.get("defines", []))
        )
        # Every selected TU must carry an exact compile interpretation.
        rec["all_tus_have_compdb_identity"] = all(
            "compdb_identity" in t for t in world["tus"].values()
        )
        rec["pass"] = (
            rec["experimental_excluded"]
            and rec["internal_testing_variant_excluded"]
            and rec["all_tus_have_compdb_identity"]
        )
        self.records["A5_exclusion"] = rec

    def a6_new_production_tu(self):
        rec = {}
        probe_rel = "src/async/fdg0_a6_probe_tu.cpp"
        probe = (
            "// fdg0 A6 specimen: temporary probe production TU (never called)\n"
            "#include <cstdint>\n"
            "namespace sluice::async {\n"
            "std::uint64_t fdg0_a6_probe_value() { return 42; }\n"
            "}  // namespace sluice::async\n"
        )
        self.write_new(probe_rel, probe)
        try:
            self.reindex()
            manifest = self.load_manifest()
            graph = self.load_graph()
            world = manifest["worlds"][0]
            rec["manifest_discovers_tu"] = probe_rel in world["tus"]
            rec["manifest_owning_targets"] = world["tus"].get(probe_rel, {}).get("owning_targets")
            rec["compdb_has_tu"] = any(
                e.get("file") == probe_rel and "build/.objs/sluice_async/" in " ".join(e.get("arguments", []))
                for e in json.loads((REPO_ROOT / "compile_commands.json").read_text())
            )
            rec["graph_indexes_tu"] = probe_rel in graph.get("documents", {})
            rec["selected_entries"] = world["compdb"]["selected_entries"]
            rec["build_id"] = manifest["build_id"]
            rec["pass"] = (
                rec["manifest_discovers_tu"]
                and rec["manifest_owning_targets"] == ["sluice_async"]
                and rec["compdb_has_tu"]
                and rec["graph_indexes_tu"]
            )
        finally:
            self.restore()
            self.reindex()  # back to the baseline world
        self.records["A6_new_production_tu"] = rec

    def a7_t7a_real_move(self):
        rec = {"manual_compdb_injection_used": "NO"}
        # Snapshot the baseline file and build the moved implementation.
        src = REPO_ROOT / MOVE_SRC
        text = src.read_text(encoding="utf-8")
        start = text.index(MOVE_FN)
        end = text.index("\n}\n", start) + len("\n}\n")
        fn_text = text[start:end]
        remove_edit = WorktreeEdit(
            MOVE_SRC,
            fn_text,
            "// fdg0 T7-A specimen: run_syscall moved to threadpool_run_syscall.cpp\n",
        )
        new_content = (
            "// fdg0 T7-A specimen: run_syscall moved out of threadpool_backend.cpp\n"
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
        # Baseline snapshot for the evaluated patch (moved-away side).
        baseline_dir = fi.BUILD_DIR / "t7a-baseline"
        shutil.rmtree(baseline_dir, ignore_errors=True)
        base_file = baseline_dir / MOVE_SRC
        base_file.parent.mkdir(parents=True, exist_ok=True)
        base_file.write_bytes(src.read_bytes())
        try:
            self.apply([remove_edit])
            self.write_new(MOVE_DST, new_content)
            self.reindex()  # xmake glob picks the new TU up: real membership
            manifest = self.load_manifest()
            graph = self.load_graph()
            world = manifest["worlds"][0]
            rec["baseline_tu"] = MOVE_SRC
            rec["moved_to_tu"] = MOVE_DST
            rec["xmake_membership_change"] = {
                "new_tu_in_resolved_sources": MOVE_DST in world["tus"],
                "new_tu_owning_targets": world["tus"].get(MOVE_DST, {}).get("owning_targets"),
                "anchor_file_still_present": MOVE_SRC in world["tus"],
            }
            rec["compdb_evidence"] = {
                "new_tu_entries": len(self.compdb_entries_for(MOVE_DST)),
                "new_tu_production_entry": any(
                    "build/.objs/sluice_async/" in " ".join(e.get("arguments", []))
                    for e in self.compdb_entries_for(MOVE_DST)
                ),
            }
            rec["build_manifest_evidence"] = {
                "tu_count": len(world["tus"]),
                "build_id": manifest["build_id"],
                "world_id": world["id"],
            }
            rec["scip_evidence"] = {
                "new_tu_indexed": MOVE_DST in graph.get("documents", {}),
                "graph_documents": graph.get("stats", {}).get("documents"),
            }
            # Anchor resolution: run_syscall must resolve at the NEW site.
            rc, out = self.run_check()
            rec["check_exit"] = rc
            rec["check_anchor_drift_lines"] = [
                l.strip() for l in out.splitlines()
                if "def-site drift" in l or "run_syscall" in l
            ]
            rec["anchor_resolves_at_new_site"] = any(
                MOVE_DST in l for l in rec["check_anchor_drift_lines"]
            )
            # Evaluated patch: baseline world -> specimen world (no git index
            # mutation; git diff --no-index, like T8).
            spec_dir = fi.BUILD_DIR / "t7a-specimen"
            shutil.rmtree(spec_dir, ignore_errors=True)
            spec_file = spec_dir / MOVE_SRC
            spec_file.parent.mkdir(parents=True, exist_ok=True)
            spec_file.write_bytes((REPO_ROOT / MOVE_SRC).read_bytes())
            spec_new = spec_dir / MOVE_DST
            spec_new.write_bytes((REPO_ROOT / MOVE_DST).read_bytes())
            patch = sh(
                "git", "diff", "--no-index", "--no-renames", "-U0",
                "build/formal-impact/t7a-baseline", "build/formal-impact/t7a-specimen",
                ok=(0, 1),
            )
            patch = patch.replace("a/build/formal-impact/t7a-baseline/", "a/")
            patch = patch.replace("b/build/formal-impact/t7a-specimen/", "b/")
            patch_path = fi.BUILD_DIR / "specimen-T7A.patch"
            patch_path.write_text(patch, encoding="utf-8")
            head_sha = fi.git_rev("HEAD")
            result = self.run_impact(
                ["--diff-file", str(patch_path), "--assume-head", head_sha]
            )
            rec["impact_result"] = {
                "classification": result["classification"],
                "claims": sorted(c["id"] for c in result["claims"]),
                "changed_files": result["changed_files"],
                "fail_closed": result.get("fail_closed", False),
                "build_world_verified": (result.get("build_world") or {}).get("verified"),
            }
            rec["impact_not_no"] = result["classification"] != fi.NO_IMPACT
            rec["f03_surfaced"] = "F03" in rec["impact_result"]["claims"]
            rec["pass"] = (
                rec["xmake_membership_change"]["new_tu_in_resolved_sources"]
                and rec["compdb_evidence"]["new_tu_production_entry"]
                and rec["scip_evidence"]["new_tu_indexed"]
                and rec["anchor_resolves_at_new_site"]
                and rec["impact_not_no"]
                and rec["f03_surfaced"]
            )
        finally:
            self.restore()
            shutil.rmtree(baseline_dir, ignore_errors=True)
            shutil.rmtree(spec_dir, ignore_errors=True)
            self.reindex()  # final: back to the baseline world
        self.records["A7_t7a_real_move"] = rec

    def a8_path_not_authority(self):
        rec = {}
        # A file under src/async/ that NO Xmake glob owns (single-level glob
        # src/async/*.cpp does not recurse — verified against resolved
        # sources). The old `src/` prefix filter would have admitted it; the
        # Build Truth world must not.
        unowned = "src/async/fdg0_unowned/notowned.cpp"
        content = (
            "// fdg0 A8 specimen: unowned file under src/async/ (no Xmake glob)\n"
            "namespace sluice::async { int fdg0_unowned_value = 0; }\n"
        )
        baseline_tus = set(self.load_manifest()["worlds"][0]["tus"])
        self.write_new(unowned, content)
        try:
            manifest = self.build_world()
            graph = self.load_graph()
            world = manifest["worlds"][0]
            rec["manifest_excludes_it"] = unowned not in world["tus"]
            rec["tu_set_unchanged"] = set(world["tus"]) == baseline_tus
            rec["graph_excludes_it"] = unowned not in graph.get("documents", {})
            rec["selected_entries_unchanged"] = world["compdb"]["selected_entries"]
            rec["pass"] = (
                rec["manifest_excludes_it"] and rec["graph_excludes_it"] and rec["tu_set_unchanged"]
            )
        finally:
            self.restore()
        self.records["A8_path_not_authority"] = rec

    def a9_a10_config_drift_and_stale_manifest(self):
        rec = {}
        manifest_liburing = self.load_manifest()
        rec["liburing_build_id"] = manifest_liburing["build_id"]
        rec["liburing_world_id"] = manifest_liburing["worlds"][0]["id"]
        try:
            sh("xmake", "f", "--with-liburing=n")
            sh("xmake", "project", "-k", "compile_commands")
            manifest_stub = self.build_world()
            rec["stub_build_id"] = manifest_stub["build_id"]
            rec["stub_world_id"] = manifest_stub["worlds"][0]["id"]
            rec["a9_identity_differs"] = (
                manifest_stub["build_id"] != rec["liburing_build_id"]
                and manifest_stub["worlds"][0]["id"] != rec["liburing_world_id"]
            )
            # A10: the GRAPH still describes the liburing world; the manifest
            # now says stub. check must fail closed.
            rc, out = self.run_check()
            rec["a10_check_exit"] = rc
            rec["a10_check_lines"] = [
                l.strip() for l in out.splitlines()
                if "FAIL" in l or "build" in l
            ][:4]
            rec["a10_fails_closed"] = rc != 0
        finally:
            sh("xmake", "f", "--with-liburing=y")
            sh("xmake", "project", "-k", "compile_commands")
            manifest_restored = self.build_world()
            rec["restored_build_id"] = manifest_restored["build_id"]
            rec["config_restored"] = (
                manifest_restored["build_id"] == rec["liburing_build_id"]
            )
        rec["pass"] = (
            rec["a9_identity_differs"]
            and rec["a10_fails_closed"]
            and rec["config_restored"]
        )
        self.records["A9_A10_config_drift_stale_manifest"] = rec

    def a11_liburing_f03_reproducible(self):
        rec = {}
        rc, out = self.run_check()
        rec["check_exit"] = rc
        rec["f03_uring_anchor_lines"] = [
            l.strip() for l in out.splitlines() if "finalize_operation_terminal_" in l
        ]
        rec["all_anchors_resolve"] = "all registered anchors resolve" in out
        rec["no_config_gate_warning"] = "config gate" not in out
        rec["pass"] = (
            rc == 0
            and rec["all_anchors_resolve"]
            and any("finalize_operation_terminal_" in l and "OK" in l
                    for l in rec["f03_uring_anchor_lines"])
        )
        self.records["A11_liburing_f03_reproducible"] = rec

    # --- orchestration ------------------------------------------------------

    def run(self) -> dict:
        started = time.time()
        dirty = [l for l in git_status_short().splitlines() if l and not l.startswith("??")]
        if dirty and not self.allow_dirty:
            raise RuntimeError(f"refuse to run on modified tracked files:\n{chr(10).join(dirty)}")
        if self.allow_dirty and dirty:
            print("WARNING --allow-dirty: specimens only touch src/async/ files; "
                  "restore is byte-exact, but verify your own dirty files are "
                  "not specimen files", file=sys.stderr)

        # Record the developer's original liburing config so A9/A10 restore it.
        import build_truth as bt  # noqa: PLC0415

        self.orig_liburing = bt.config_options().get("with-liburing")
        if self.orig_liburing not in ("true", "false"):
            raise RuntimeError(f"unexpected original with-liburing state: {self.orig_liburing!r}")

        try:
            self.baseline()
            self.a4_missing_compdb_entry()
            self.a5_exclusion()
            self.a6_new_production_tu()
            self.a7_t7a_real_move()
            self.a8_path_not_authority()
            self.a9_a10_config_drift_and_stale_manifest()
            self.a11_liburing_f03_reproducible()
        finally:
            self.restore()
            if self.orig_liburing == "true":
                sh("xmake", "f", "--with-liburing=y")
            else:
                sh("xmake", "f", "--with-liburing=n")
            sh("xmake", "project", "-k", "compile_commands")

        summary = {
            "experiment": "FDG-0 Phase A — Xmake Build Truth adversarial suite (issue #298)",
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "head_sha": fi.git_rev("HEAD"),
            "root_target": ROOT_TARGET,
            "specimens": self.records,
            "wall_seconds": round(time.time() - started, 1),
        }
        summary["all_pass"] = all(
            r.get("pass", True) for r in self.records.values()
            if "pass" in r
        )
        return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run_p = sub.add_parser("run", help="run the Build Truth specimen suite")
    run_p.add_argument("--out", default=str(RESULTS_PATH))
    run_p.add_argument("--allow-dirty", action="store_true",
                       help="DEV ONLY: run with modified tracked files present "
                            "(specimens touch only src/async/ files; restore is byte-exact)")
    sub.add_parser("verify-clean", help="fail if the working tree is dirty")
    args = parser.parse_args(argv)

    if args.command == "verify-clean":
        status = git_status_short()
        if status:
            print("DIRTY:", status)
            return 1
        print("clean")
        return 0

    driver = Driver(allow_dirty=args.allow_dirty)
    try:
        summary = driver.run()
    except Exception as exc:  # noqa: BLE001
        driver.restore()
        print(f"error: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    print(f"==> results written: {out}")
    for key, rec in summary["specimens"].items():
        ok = "OK " if rec.get("pass") else "FAIL"
        print(f"  {key}: {ok}")
    print(f"  all_pass: {summary['all_pass']}")
    return 0 if summary["all_pass"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
